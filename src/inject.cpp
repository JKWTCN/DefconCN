#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>

std::uintptr_t RemoteModule(DWORD pid, const wchar_t* name) {
  HANDLE snapshot = INVALID_HANDLE_VALUE;
  for (int retry=0; retry<8; ++retry) {
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot != INVALID_HANDLE_VALUE || GetLastError()!=ERROR_BAD_LENGTH) break;
    Sleep(50);
  }
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  MODULEENTRY32W m{}; m.dwSize = sizeof(m);
  std::uintptr_t result = 0;
  if (Module32FirstW(snapshot, &m)) do {
    if (!_wcsicmp(m.szModule, name)) { result = reinterpret_cast<std::uintptr_t>(m.modBaseAddr); break; }
  } while (Module32NextW(snapshot, &m));
  CloseHandle(snapshot);
  return result;
}

void ReportFailure(const wchar_t* step, DWORD code) {
  wchar_t message[1024]{};
  swprintf_s(message,L"失败步骤：%s\n错误代码：%lu\n\n日志：%%TEMP%%\\DefconCN-poc-进程ID.log\n如果没有日志，说明尚未进入 DLL 初始化。",step,code);
  fwprintf(stderr,L"%s\n",message);
#ifdef DEFCON_CN_LAUNCHER
  MessageBoxW(nullptr,message,L"DEFCON 汉化启动",MB_OK | MB_ICONERROR);
#endif
}

bool RunRemote(HANDLE process, std::uintptr_t fn, void* arg, DWORD& result) {
  HANDLE thread = CreateRemoteThread(process, nullptr, 0,
      reinterpret_cast<LPTHREAD_START_ROUTINE>(fn), arg, 0, nullptr);
  if (!thread) return false;
  // 等待远程线程结束后再释放内存，避免线程继续访问已释放的数据。
  DWORD waited = WaitForSingleObject(thread, INFINITE);
  bool ok = waited == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result);
  CloseHandle(thread);
  return ok;
}

int Inject(DWORD pid, const std::wstring& path) {
  if (!std::filesystem::is_regular_file(path)) { ReportFailure(L"DLL 文件不存在",ERROR_FILE_NOT_FOUND); return 1; }
  HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
  if (!process) { ReportFailure(L"OpenProcess",GetLastError()); return 2; }
  auto fail = [&](const wchar_t* msg) { DWORD code=GetLastError(); CloseHandle(process); ReportFailure(msg,code); return 3; };
  wchar_t exe[32768]{}; DWORD exeSize = 32768;
  if (!QueryFullProcessImageNameW(process, 0, exe, &exeSize) ||
      _wcsicmp(std::filesystem::path(exe).filename().c_str(), L"Defcon.exe"))
    return fail(L"Target must be Defcon.exe");
  USHORT targetMachine = 0, nativeMachine = 0;
  if (!IsWow64Process2(process, &targetMachine, &nativeMachine) ||
      targetMachine != IMAGE_FILE_MACHINE_UNKNOWN || nativeMachine != IMAGE_FILE_MACHINE_AMD64)
    return fail(L"Target must be native Windows x64");
  auto name = std::filesystem::path(path).filename().wstring();
  if (RemoteModule(pid, name.c_str())) return fail(L"Proof DLL already loaded; restart game before retrying");
  // 根据所属系统模块计算远程函数地址，兼容两个进程不同的模块加载位置。
  auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
  HMODULE owner = nullptr;
  if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(load), &owner))
    return fail(L"Cannot resolve LoadLibraryW");
  wchar_t ownerPath[MAX_PATH]{};
  GetModuleFileNameW(owner, ownerPath, MAX_PATH);
  auto remoteOwner = RemoteModule(pid, std::filesystem::path(ownerPath).filename().c_str());
  if (!remoteOwner) return fail(L"Cannot find remote system module");
  auto remoteLoad = remoteOwner + reinterpret_cast<std::uintptr_t>(load) - reinterpret_cast<std::uintptr_t>(owner);
  SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
  void* memory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!memory) return fail(L"VirtualAllocEx failed");
  SIZE_T written = 0;
  if (!WriteProcessMemory(process, memory, path.c_str(), bytes, &written) || written != bytes) {
    VirtualFreeEx(process, memory, 0, MEM_RELEASE); return fail(L"WriteProcessMemory failed");
  }
  DWORD result = 0;
  bool loaded = RunRemote(process, remoteLoad, memory, result);
  VirtualFreeEx(process, memory, 0, MEM_RELEASE);
  if (!loaded) return fail(L"Remote LoadLibraryW failed");
  // 线程退出码只有 32 位，需要枚举模块才能取得完整的 64 位模块地址。
  auto remoteDll = RemoteModule(pid, name.c_str());
  if (!remoteDll) return fail(L"DLL was not loaded");
  HMODULE localDll = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!localDll) return fail(L"Cannot inspect DLL exports");
  auto start = GetProcAddress(localDll, "DefconCN_Start");
  if (!start) { FreeLibrary(localDll); return fail(L"DefconCN_Start export missing"); }
  auto remoteStart = remoteDll + reinterpret_cast<std::uintptr_t>(start) - reinterpret_cast<std::uintptr_t>(localDll);
  FreeLibrary(localDll);
  if (!RunRemote(process, remoteStart, nullptr, result)) return fail(L"Remote initialization failed");
  CloseHandle(process);
  wprintf(L"Initialization result: %lu. Log: %%TEMP%%\\DefconCN-poc-%lu.log\n", result, pid);
  if (result!=0) {
    wchar_t detail[256]{};
    swprintf_s(detail,L"DLL 初始化（PID %lu）：11=版本不符，12=入口不符，13/14=Hook 失败，15=语言文件错误，16=字体错误",pid);
    ReportFailure(detail,result);
  }
  return result == 0 ? 0 : 4;
}

#ifndef DEFCON_CN_LAUNCHER
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) { fwprintf(stderr, L"Usage: DefconCNInject.exe PID full-path-to-DefconCNProof.dll\n"); return 1; }
  wchar_t* end = nullptr;
  unsigned long parsed = wcstoul(argv[1], &end, 10);
  if (!parsed || *end) return 1;
  return Inject(static_cast<DWORD>(parsed), std::filesystem::absolute(argv[2]).wstring());
}
#else
struct GameWindowSearch {
  DWORD pid;
  bool found = false;
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM parameter) {
  auto& search = *reinterpret_cast<GameWindowSearch*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != search.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
  wchar_t title[256]{};
  GetWindowTextW(window, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
  if (_wcsicmp(title, L"DEFCON")) return TRUE;
  search.found = true;
  return FALSE;
}

bool HasGameWindow(DWORD pid) {
  GameWindowSearch search{pid};
  EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

DWORD FindGame(const std::filesystem::path& expected, bool requireWindow = true) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
  DWORD result = 0;
  if (Process32FirstW(snapshot, &entry)) do {
    if (_wcsicmp(entry.szExeFile, L"Defcon.exe")) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (!process) continue;
    wchar_t path[32768]{}; DWORD size = 32768;
    if (QueryFullProcessImageNameW(process, 0, path, &size) && !_wcsicmp(path, expected.c_str()) &&
        (!requireWindow || HasGameWindow(entry.th32ProcessID)))
      result = entry.th32ProcessID;
    CloseHandle(process);
    if (result) break;
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return result;
}

DWORD WaitForGame(const std::filesystem::path& executable) {
  DWORD previous=0;
  ULONGLONG stableSince=0, deadline=GetTickCount64()+30000;
  while (GetTickCount64()<deadline) {
    DWORD pid=FindGame(executable);
    bool ready=pid && RemoteModule(pid,L"kernel32.dll") && RemoteModule(pid,L"SDL3.dll");
    if (!ready || pid!=previous) { previous=pid; stableSince=GetTickCount64(); }
    else if (GetTickCount64()-stableSince>=1500) return pid;
    Sleep(250);
  }
  return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DefconCNProofLauncher");
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
  auto run = []() -> int {
    wchar_t self[32768]{};
    if (!GetModuleFileNameW(nullptr, self, 32768)) return 1;
    auto directory = std::filesystem::path(self).parent_path();
    auto executable = directory / L"Defcon.exe";
    auto dll = directory / L"DefconCNProof.dll";
    auto error = [](const wchar_t* text) { MessageBoxW(nullptr, text, L"DEFCON 汉化启动", MB_OK | MB_ICONERROR); return 1; };
    if (!std::filesystem::is_regular_file(executable) || !std::filesystem::is_regular_file(dll))
      return error(L"请把启动器和 DefconCNProof.dll 复制到 Defcon.exe 所在目录。");
    if (!std::filesystem::is_regular_file(directory / L"data" / L"defconcn" / L"chinese.txt"))
      return error(L"缺少 chinese.txt，请同时复制发布包中的 data 文件夹。无需 Python 安装。");
    if (!SetCurrentDirectoryW(directory.c_str())) return error(L"无法设置游戏工作目录。");
    // 已有进程可能仍在创建窗口，此时等待即可，避免重复启动游戏。
    DWORD pid = FindGame(executable, false);
    if (!pid) {
      std::wstring command = L"\"" + executable.wstring() + L"\"";
      STARTUPINFOW si{}; si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                           0, nullptr, directory.c_str(), &si, &pi))
        return error(L"无法启动 DEFCON。请确认 Steam 已运行，且游戏可以正常启动。");
      CloseHandle(pi.hThread);
      WaitForInputIdle(pi.hProcess, 10000);
      CloseHandle(pi.hProcess);
    }
    // Steam 可能创建临时进程，只向拥有可见游戏窗口的进程加载汉化模块。
    pid=WaitForGame(executable);
    if (!pid) return error(L"等待 DEFCON 游戏窗口就绪超时。请从 Steam 启动 DEFCON，等主菜单出现后，再双击汉化启动器。");
    if (RemoteModule(pid, L"DefconCNProof.dll"))
      return error(L"该游戏进程已加载验证 DLL。请退出游戏后重新启动，以便加载本次版本。");
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process) { WaitForInputIdle(process, 10000); CloseHandle(process); }
    if (Inject(pid, dll.wstring()) != 0) return 1;
    return 0;
  };
  int result = 1;
  try { result = run(); }
  catch (const std::exception&) {
    MessageBoxW(nullptr, L"读取启动文件失败，请检查发布包与目录权限。", L"DEFCON 汉化启动", MB_OK | MB_ICONERROR);
  }
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return result;
}
#endif

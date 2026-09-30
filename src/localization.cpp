#include <windows.h>
#include <wincrypt.h>
#include <MinHook.h>
#include "utf8.h"
#include "native_font.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <array>
#include <stdexcept>
#include <utility>

namespace {
constexpr char kHash[] = "DB21B55FF4DE545677DA6C056D90604329FA70ECE03D3E8898639768DF3CC195";
using DrawFn = std::int64_t(__fastcall*)(void*, float, float, const void*, float, const char*, char);
using WidthFn = float(__fastcall*)(void*, const char*, float);
using FontFn = cn::NativeFont*(__fastcall*)(void*, const char*);
using LookupFn = const char*(__fastcall*)(void*, const char*, void*);
using UpperFn = char*(__fastcall*)(char*);
using WrapFn = void*(__fastcall*)(void*, const char*, float, float, char);
DrawFn originalDraw{}; WidthFn originalWidth{}; FontFn originalFont{};
LookupFn originalLookup{}; UpperFn originalUpper{}; WrapFn originalWrap{};
std::uintptr_t base{};
std::unordered_map<std::string, std::string> translations;
thread_local cn::NativeFont* activeFont = nullptr;
thread_local cn::NativeFont measureFont;
std::atomic<bool> drawEntered{false}, widthEntered{false}, drawUnicode{false}, hitLanguage{false}, renderError{false};
std::atomic<bool> invalidUtf8Logged{false};
volatile LONG started = 0;

void Log(const char* message) noexcept {
  wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{};
  if (!GetTempPathW(MAX_PATH, folder)) return;
  swprintf_s(path, L"%sDefconCN-poc-%lu.log", folder, GetCurrentProcessId());
  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written;
  WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
  WriteFile(file, "\r\n", 2, &written, nullptr); CloseHandle(file);
}

bool ExactExecutable() {
  wchar_t path[32768]{};
  if (!GetModuleFileNameW(nullptr, path, 32768)) return false;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  HCRYPTPROV provider{}; HCRYPTHASH hash{};
  bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
         && CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
  BYTE buffer[65536]{}, digest[32]{}; DWORD count{};
  while (ok) {
    if (!ReadFile(file, buffer, sizeof(buffer), &count, nullptr)) { ok = false; break; }
    if (!count) break;
    ok = CryptHashData(hash, buffer, count, 0) != FALSE;
  }
  count = sizeof(digest);
  if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &count, 0) != FALSE;
  char hex[65]{};
  if (ok) for (unsigned i=0; i<32; ++i) sprintf_s(hex+2*i, 3, "%02X", digest[i]);
  if (hash) CryptDestroyHash(hash);
  if (provider) CryptReleaseContext(provider, 0);
  CloseHandle(file);
  return ok && strcmp(hex, kHash)==0;
}

std::string Lower(std::string key) {
  for (char& c : key) if (c>='A' && c<='Z') c+=32;
  return key;
}

void ReadLanguage(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("Missing data/defconcn/chinese.txt");
  std::string line; bool first = true;
  while (std::getline(input, line)) {
    if (first && line.compare(0,3,"\xEF\xBB\xBF")==0) line.erase(0,3);
    first = false;
    if (!line.empty() && line.back()=='\r') line.pop_back();
    auto start = line.find_first_not_of(" \t\r");
    if (start==std::string::npos || line[start]=='#') continue;
    auto end = line.find_first_of(" \t",start);
    if (end==std::string::npos) continue;
    auto valueStart = line.find_first_not_of(" \t",end);
    if (valueStart==std::string::npos) continue;
    auto key = Lower(line.substr(start,end-start));
    auto value = line.substr(valueStart);
    if (value.find('\0')!=std::string::npos) throw std::runtime_error("NUL in translation");
    std::vector<cn::Symbol> symbols; bool unicode;
    if (!cn::Decode(value,symbols,unicode)) throw std::runtime_error("Language file must use valid UTF-8");
    for (std::size_t i=0; (i=value.find("\\n",i))!=std::string::npos; ++i) value.replace(i,2,"\n");
    if (value=="***EMPTY***") value=" ";
    translations.insert_or_assign(std::move(key),std::move(value));
  }
  if (translations.empty()) throw std::runtime_error("No translation entries found");
  // 地图提示根据鼠标标记显示对应图标，并固定跳过五个字节。
  // 将旧语言包中的中文标记还原为固定长度标记，避免截断中文字符。
  for (const auto& marker : std::array<std::pair<const char*, const char*>,2>{{
      {"tooltip_lmb","[LMB]"},{"tooltip_rmb","[RMB]"}}}) {
    auto found = translations.find(marker.first);
    if (found == translations.end()) continue;
    const auto previous = found->second;
    if (previous == marker.second) continue;
    if (!previous.empty()) for (auto& entry : translations) {
      if (entry.second.compare(0,previous.size(),previous)==0)
        entry.second.replace(0,previous.size(),marker.second);
    }
    found->second = marker.second;
    Log("LANGUAGE: normalized five-byte mouse icon marker.");
  }
  char message[128]{};
  sprintf_s(message,"LANGUAGE: loaded %zu entries from chinese.txt.",translations.size()); Log(message);
}

const char* __fastcall Lookup(void* table, const char* key, void* fallback) {
  try {
    auto language = *reinterpret_cast<std::uintptr_t*>(base+0x4D5A18);
    auto address = reinterpret_cast<std::uintptr_t>(table);
    if (language && (address==language || address==language+64) && key) {
      auto found = translations.find(Lower(key));
      if (found!=translations.end()) {
        if (!hitLanguage.exchange(true)) Log("LOOKUP: game requested a translated language key.");
        return found->second.c_str(); // 启用文本替换后不再修改词典，返回的字符串地址保持有效。
      }
    }
  } catch (...) { Log("LOOKUP: exception; using original language."); }
  return originalLookup(table,key,fallback);
}

cn::NativeFont* __fastcall Font(void* owner,const char* name) {
  return activeFont ? activeFont : originalFont(owner,name);
}

struct FontScope {
  cn::NativeFont* previous;
  unsigned char* fixedFlag;
  unsigned char fixed;
  FontScope(void* renderer,cn::NativeFont* font) : previous(activeFont),
    fixedFlag(reinterpret_cast<unsigned char*>(renderer)+23042),fixed(*fixedFlag) {
    // 保留当前投影使用的垂直翻转状态，确保地图文字的显示方向正确。
    // 中文只关闭面向窄字符的等宽模式，不改变文字方向。
    activeFont=font; *fixedFlag=0;
  }
  ~FontScope() { *fixedFlag=fixed; activeFont=previous; }
};

float Advance(void* renderer,const cn::Symbol& symbol,float size) {
  if (symbol.code<128) {
    char ascii[2]{static_cast<char>(symbol.code),0};
    return originalWidth(renderer,ascii,size);
  }
  char mapped[2]{static_cast<char>(cn::GlyphSlot(symbol.code)),0};
  FontScope scope(renderer,&measureFont);
  return originalWidth(renderer,mapped,size);
}

float __fastcall Width(void* renderer,const char* text,float size) {
  if (!widthEntered.exchange(true)) Log("WIDTH_ENTER: game text measurement hook reached.");
  try {
    std::vector<cn::Symbol> symbols; bool unicode;
    if (!text || !cn::Decode(text,symbols,unicode) || !unicode) return originalWidth(renderer,text,size);
    float line=0, maximum=0;
    for (auto& s:symbols) {
      if (s.code=='\n') { maximum=std::max(maximum,line); line=0; }
      else if (s.code!='\r') line+=Advance(renderer,s,size);
    }
    return std::max(maximum,line);
  } catch (...) { Log("WIDTH: exception; using original measurement."); return originalWidth(renderer,text,size); }
}

std::int64_t __fastcall Draw(void* renderer,float x,float y,const void* color,float size,const char* text,char mode) {
  if (!drawEntered.exchange(true)) Log("DRAW_ENTER: game text drawing hook reached.");
  try {
    std::vector<cn::Symbol> symbols; bool unicode;
    if (!text) return originalDraw(renderer,x,y,color,size,text,mode);
    if (!cn::Decode(text,symbols,unicode)) {
      if (!invalidUtf8Logged.exchange(true)) {
        char detail[320]{};
        std::size_t offset = static_cast<std::size_t>(sprintf_s(detail,"DRAW_INVALID_UTF8: bytes="));
        for (std::size_t i=0; text[i] && i<48; ++i)
          offset += static_cast<std::size_t>(sprintf_s(detail+offset,sizeof(detail)-offset,"%02X ",static_cast<unsigned char>(text[i])));
        Log(detail);
      }
      return originalDraw(renderer,x,y,color,size,text,mode);
    }
    if (!unicode) return originalDraw(renderer,x,y,color,size,text,mode);
    std::int64_t result=0; float start=x;
    for (auto& s:symbols) {
      if (s.code=='\n') { x=start; y+=size*1.2f; continue; }
      if (s.code=='\r') continue;
      float advance=Advance(renderer,s,size);
      if (s.code<128) {
        char ascii[2]{static_cast<char>(s.code),0};
        result=originalDraw(renderer,x,y,color,size,ascii,mode);
      } else {
        auto font=cn::GlyphFont(base,s.code);
        char mapped[2]{static_cast<char>(cn::GlyphSlot(s.code)),0};
        FontScope scope(renderer,font);
        result=originalDraw(renderer,x,y,color,size,mapped,mode);
        if (!drawUnicode.exchange(true)) Log("DRAW_UTF8: Unicode atlas uploaded; first Unicode glyph submitted.");
      }
      x+=advance;
    }
    return result;
  } catch (const std::exception& error) {
    if (!renderError.exchange(true)) { Log("DRAW_ERROR:"); Log(error.what()); }
    return originalDraw(renderer,x,y,color,size,"[CN render error]",mode);
  }
}

char* __fastcall Upper(char* text) {
  try {
    std::vector<cn::Symbol> symbols; bool unicode;
    if (text && cn::Decode(text,symbols,unicode) && unicode) {
      for (char* p=text; *p; ++p) if (*p>='a' && *p<='z') *p-=32;
      return text; // 仅转换英文字母，保留中文的全部字节，不受系统区域设置影响。
    }
  } catch (...) {}
  return originalUpper(text);
}

void* __fastcall Wrap(void* result,const char* text,float maximum,float size,char enabled) {
  try {
    std::vector<cn::Symbol> symbols; bool unicode;
    if (!text || !enabled || maximum<=0 || !cn::Decode(text,symbols,unicode) || !unicode)
      return originalWrap(result,text,maximum,size,enabled);
    auto renderer=*reinterpret_cast<void**>(base+0x4D5A90);
    if (!renderer) return originalWrap(result,text,maximum,size,enabled);
    std::string output; float width=0;
    for (auto& s:symbols) {
      if (s.code=='\n') { output+='\n'; width=0; continue; }
      if (s.code=='\r') continue;
      auto advance=Advance(renderer,s,size);
      if (width>0 && width+advance>maximum) { output+='\n'; width=0; }
      if (width==0 && s.code==' ') continue;
      output.append(text+s.offset,s.bytes); width+=advance;
    }
    // 由游戏自己的运行库分配并复制行数据，确保内存分配与释放方式一致。
    return originalWrap(result,output.c_str(),maximum,size,0);
  } catch (...) { Log("WRAP: exception; using original layout."); return originalWrap(result,text,maximum,size,enabled); }
}
}

extern "C" __declspec(dllexport) DWORD WINAPI DefconCN_Start(void*) {
  if (InterlockedCompareExchange(&started,1,0)) return 10;
  Log("START: generic UTF-8 localization v2.1 (packed atlas, projection flip, five-byte mouse markers).");
  try {
    if (!ExactExecutable()) { Log("ERROR: EXE SHA256 mismatch."); return 11; }
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr,executable,32768)) return 15;
    auto directory=std::filesystem::path(executable).parent_path()/L"data"/L"defconcn";
    ReadLanguage(directory/L"chinese.txt");
    std::vector<std::uint32_t> glyphs;
    std::vector<cn::Symbol> symbols; bool unicode;
    for (const auto& entry:translations) {
      if (!cn::Decode(entry.second,symbols,unicode)) throw std::runtime_error("Invalid UTF-8 in language glyph collection");
      for (const auto& symbol:symbols) if (symbol.code>=128) glyphs.push_back(symbol.code);
    }
    std::sort(glyphs.begin(),glyphs.end());
    glyphs.erase(std::unique(glyphs.begin(),glyphs.end()),glyphs.end());
    char atlasMessage[160]{};
    sprintf_s(atlasMessage,"FONT_ATLAS: %zu language glyphs packed into %zu pages.",glyphs.size(),(glyphs.size()+254)/255);
    Log(atlasMessage);
    if (!cn::InitFonts(directory/L"config.ini",std::move(glyphs))) { Log("ERROR: GDI font initialization failed."); return 16; }
    struct Hook { std::uintptr_t rva; void* detour; void** original; std::vector<unsigned char> prefix; };
    std::array<Hook,6> hooks{{
      {0x1CA500,reinterpret_cast<void*>(&Draw),reinterpret_cast<void**>(&originalDraw),{0x48,0x8b,0xc4,0x48,0x89,0x58,0x10}},
      {0x1CAA70,reinterpret_cast<void*>(&Width),reinterpret_cast<void**>(&originalWidth),{0x40,0x53,0x55,0x56,0x57,0x41,0x56}},
      {0x19BEC0,reinterpret_cast<void*>(&Font),reinterpret_cast<void**>(&originalFont),{0x48,0x8b,0xc4,0x48,0x89,0x48,0x08}},
      {0x040CA0,reinterpret_cast<void*>(&Lookup),reinterpret_cast<void**>(&originalLookup),{0x40,0x53,0x55,0x56,0x57,0x41,0x56}},
      {0x3BB150,reinterpret_cast<void*>(&Upper),reinterpret_cast<void**>(&originalUpper),{0x40,0x53,0x48,0x83,0xec,0x20}},
      {0x1D8F70,reinterpret_cast<void*>(&Wrap),reinterpret_cast<void**>(&originalWrap),{0x48,0x8b,0xc4,0x48,0x89,0x58,0x18}}
    }};
    for (auto& h:hooks) if (memcmp(reinterpret_cast<void*>(base+h.rva),h.prefix.data(),h.prefix.size())) {
      Log("ERROR: function entry mismatch."); return 12;
    }
    auto status=MH_Initialize();
    if (status!=MH_OK) { Log(MH_StatusToString(status)); return 13; }
    for (auto& h:hooks) {
      status=MH_CreateHook(reinterpret_cast<void*>(base+h.rva),h.detour,h.original);
      if (status!=MH_OK) break;
      status=MH_QueueEnableHook(reinterpret_cast<void*>(base+h.rva));
      if (status!=MH_OK) break;
    }
    if (status==MH_OK) status=MH_ApplyQueued();
    if (status!=MH_OK) {
      Log(MH_StatusToString(status)); MH_DisableHook(MH_ALL_HOOKS);
      MH_RemoveHook(MH_ALL_HOOKS); MH_Uninitialize(); return 14;
    }
    Log("READY: language lookup, UTF-8 draw/width/wrap, font and uppercase hooks installed.");
    return 0;
  } catch (const std::exception& error) { Log("INIT_ERROR:"); Log(error.what()); return 15; }
}

BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
  if (reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
  return TRUE;
}

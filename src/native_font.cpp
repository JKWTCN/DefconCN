#include "native_font.h"
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace cn {
namespace {
HDC dc = nullptr;
HFONT font = nullptr;
std::mutex lock;
struct Page { NativeFont native; };
std::map<std::pair<std::uintptr_t, std::uint32_t>, std::unique_ptr<Page>> pages;
// 初始化后保持不变，文字测量和绘制必须使用相同的字形槽位。
std::vector<std::uint32_t> languageGlyphs;
std::unordered_map<std::uint32_t, std::uint32_t> glyphIndices;
std::uint32_t languagePageCount = 0;

std::uint32_t GlyphIndex(std::uint32_t cp) {
  auto found = glyphIndices.find(cp);
  if (found != glyphIndices.end()) return found->second;
  // 玩家名称等动态文本可能包含语言文件中没有的字符。
  // 为这些字符保留独立的编码区段，避免与紧凑排列的语言字形冲突。
  return languagePageCount * 255 + cp;
}
}

bool InitFonts(const std::filesystem::path& config, std::vector<std::uint32_t> glyphs) {
  glyphs.erase(std::remove_if(glyphs.begin(), glyphs.end(), [](std::uint32_t cp) {
    return cp < 128 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF);
  }), glyphs.end());
  std::sort(glyphs.begin(), glyphs.end());
  glyphs.erase(std::unique(glyphs.begin(), glyphs.end()), glyphs.end());
  languageGlyphs = std::move(glyphs);
  glyphIndices.clear();
  for (std::size_t i = 0; i < languageGlyphs.size(); ++i)
    glyphIndices.emplace(languageGlyphs[i], static_cast<std::uint32_t>(i));
  languagePageCount = static_cast<std::uint32_t>((languageGlyphs.size() + 254) / 255);
  wchar_t family[128]{};
  GetPrivateProfileStringW(L"Font", L"Family", L"Microsoft YaHei", family, 128, config.c_str());
  auto privateFont = config.parent_path() / L"font.ttf";
  if (std::filesystem::is_regular_file(privateFont)) AddFontResourceExW(privateFont.c_str(), FR_PRIVATE, nullptr);
  dc = CreateCompatibleDC(nullptr);
  auto weight = std::clamp(GetPrivateProfileIntW(L"Font", L"Weight", FW_SEMIBOLD, config.c_str()), 100u, 900u);
  // 增大字形并加粗笔画，提高缩小到游戏界面尺寸后的清晰度。
  font = CreateFontW(-56, 0, 0, 0, static_cast<int>(weight), FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                    DEFAULT_PITCH, family);
  if (!dc || !font) return false;
  SelectObject(dc, font);
  SetTextColor(dc, RGB(255,255,255));
  SetBkColor(dc, RGB(0,0,0));
  SetBkMode(dc, OPAQUE);
  SetTextAlign(dc, TA_LEFT | TA_BASELINE);
  return true;
}

unsigned char GlyphSlot(std::uint32_t cp) {
  return static_cast<unsigned char>(GlyphIndex(cp) % 255 + 1);
}

NativeFont* GlyphFont(std::uintptr_t base, std::uint32_t cp) {
  std::lock_guard<std::mutex> guard(lock);
  // 使用游戏的纹理管理对象创建字形纹理，与原有字体共用渲染后端。
  auto windowManager = *reinterpret_cast<std::uintptr_t*>(base + 0x4D5A48);
  if (!windowManager) throw std::runtime_error("Window manager is not ready");
  auto backend = *reinterpret_cast<std::uintptr_t*>(windowManager + 32);
  if (!backend) throw std::runtime_error("Texture backend is not ready");
  auto key = std::make_pair(backend, GlyphIndex(cp) / 255);
  auto found = pages.find(key);
  if (found != pages.end()) return &found->second->native;
  if (pages.size() >= 64) throw std::runtime_error("Unicode atlas cache limit reached; restart game");
  constexpr int side = 1024, cell = 64;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = side; info.bmiHeader.biHeight = -side;
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* raw = nullptr;
  HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
  if (!bitmap || !raw) throw std::runtime_error("Cannot allocate Unicode atlas");
  auto old = SelectObject(dc, bitmap);
  memset(raw, 0, side * side * 4);
  bool ok = true;
  const MAT2 identity{{0,1},{0,0},{0,0},{0,1}};
  TEXTMETRICW metrics{};
  if (!GetTextMetricsW(dc, &metrics)) ok = false;
  for (unsigned slot = 1; ok && slot <= 255; ++slot) {
    std::uint32_t code;
    if (key.second < languagePageCount) {
      auto index = static_cast<std::size_t>(key.second) * 255 + slot - 1;
      if (index >= languageGlyphs.size()) continue;
      code = languageGlyphs[index];
    } else {
      code = (key.second - languagePageCount) * 255 + slot - 1;
    }
    if (code < 128 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) continue;
    wchar_t text[2]{}; int length = 1;
    if (code <= 0xFFFF) text[0] = static_cast<wchar_t>(code);
    else {
      auto value = code - 0x10000;
      text[0] = static_cast<wchar_t>(0xD800 + (value >> 10));
      text[1] = static_cast<wchar_t>(0xDC00 + (value & 1023)); length = 2;
    }
    SIZE extent{};
    if (!GetTextExtentPoint32W(dc, text, length, &extent)) { ok = false; break; }
    RECT bounds{static_cast<LONG>((slot % 16) * cell), static_cast<LONG>((slot / 16) * cell),
                static_cast<LONG>((slot % 16 + 1) * cell), static_cast<LONG>((slot / 16 + 1) * cell)};
    auto x = bounds.left + (cell - extent.cx) / 2;
    auto baseline = bounds.top + (cell - extent.cy) / 2 + metrics.tmAscent;
    GLYPHMETRICS ink{};
    if (length == 1 && GetGlyphOutlineW(dc, text[0], GGO_METRICS, &ink, 0, nullptr, &identity) != GDI_ERROR &&
        ink.gmBlackBoxX && ink.gmBlackBoxY) {
      // 按实际可见笔画居中，避免字体行高中的空白使字形偏移。
      x = bounds.left + (cell - static_cast<LONG>(ink.gmBlackBoxX)) / 2 - ink.gmptGlyphOrigin.x;
      baseline = bounds.top + (cell - static_cast<LONG>(ink.gmBlackBoxY)) / 2 + ink.gmptGlyphOrigin.y;
    }
    if (!ExtTextOutW(dc, x, baseline,
                     ETO_CLIPPED, &bounds, text, length, nullptr)) { ok = false; break; }
  }
  GdiFlush();
  std::vector<std::uint32_t> pixels(side * side);
  const auto* source = static_cast<const std::uint32_t*>(raw);
  // 纹理接口使用从下到上的像素排列，并要求不透明的透明度通道。
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x)
      pixels[(side - y - 1) * side + x] = source[y * side + x] | 0xFF000000u;
  SelectObject(dc, old); DeleteObject(bitmap);
  if (!ok) throw std::runtime_error("GDI glyph rasterization failed");
  using Upload = int(__fastcall*)(void*, int, int, const void*, int);
  auto table = *reinterpret_cast<std::uintptr_t**>(backend);
  auto upload = reinterpret_cast<Upload>(table[16]); // 纹理创建接口位于虚函数表的第十六号槽位。
  auto page = std::make_unique<Page>();
  page->native.texture = upload(reinterpret_cast<void*>(backend), side, side, pixels.data(), 1);
  if (page->native.texture < 0) throw std::runtime_error("Native texture upload failed");
  auto* result = &page->native;
  pages.emplace(key, std::move(page));
  // 纹理由游戏渲染后端管理，字体对象保留至进程退出。
  return result;
}
}

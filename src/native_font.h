#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace cn {
// 与游戏字体对象保持相同的内存布局，总大小为 2072 字节。
struct NativeFont {
  const char* label = "DefconCN";
  std::int32_t texture = 0;
  std::uint8_t negative = 0, padding0[3]{};
  float spacing = 0.05f;
  std::uint8_t fixed = 0, padding1[3]{};
  float left[256]{};
  float right[256]{};
  NativeFont() { for (auto& value : right) value = 1.0f / 16.0f; }
};
static_assert(sizeof(NativeFont) == 2072);
static_assert(offsetof(NativeFont, left) == 24 && offsetof(NativeFont, right) == 1048);
bool InitFonts(const std::filesystem::path& config, std::vector<std::uint32_t> glyphs);
NativeFont* GlyphFont(std::uintptr_t base, std::uint32_t codepoint);
// 零号字形槽保留给字符串结束符，实际字形使用一至二百五十五号槽位。
unsigned char GlyphSlot(std::uint32_t codepoint);
}

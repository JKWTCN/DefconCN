#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace cn {
struct Symbol { std::uint32_t code; std::size_t offset, bytes; };
inline bool Decode(std::string_view text, std::vector<Symbol>& out, bool& nonAscii) {
  out.clear(); nonAscii = false;
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    unsigned n = 1; std::uint32_t cp = first, minimum = 0;
    if (first >= 0x80) {
      nonAscii = true;
      if (first >= 0xC2 && first <= 0xDF) { n = 2; cp = first & 31; minimum = 0x80; }
      else if (first >= 0xE0 && first <= 0xEF) { n = 3; cp = first & 15; minimum = 0x800; }
      else if (first >= 0xF0 && first <= 0xF4) { n = 4; cp = first & 7; minimum = 0x10000; }
      else return false;
      if (i + n > text.size()) return false;
      for (unsigned j = 1; j < n; ++j) {
        auto b = static_cast<unsigned char>(text[i + j]);
        if ((b & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (b & 63);
      }
      if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    }
    out.push_back({cp, i, n}); i += n;
  }
  return true;
}
}

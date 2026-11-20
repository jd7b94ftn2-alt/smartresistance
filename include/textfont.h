#pragma once

#include <Arduino.h>

// 16 px, 2-bit Chinese text font (Noto Sans SC, GB2312 + ASCII) for text the UI fonts do not cover,
// e.g. iPhone notifications. Data: data/font_text16.bin (tools/make_textfont.py), linked in through
// board_build.embed_files. Renders into the 2-bit card format of messages.h.
namespace textfont {

bool ready();
int lineHeight();                                    // 20 px
int width(const char* utf8);
// Longest prefix that fits maxWidth px; "…" is appended when the text had to be cut.
String fit(const char* utf8, int maxWidth);
// Draws into a 2-bit bitmap (stride (w + 3) / 4, MSB first) with the line box's top at y; glyph
// levels are scaled by level (1..3) and clipped to x < maxX. Returns the x after the text.
int draw2bpp(std::uint8_t* bitmap, int w, int h, int x, int y, const char* utf8, int level, int maxX);
// For wrapping long text (分享 page reader): next code point of [p, end) (advances p; bad bytes = U+FFFD)
// and the advance width of one code point in px.
std::uint32_t decode(const char*& p, const char* end);
int advance(std::uint32_t code);

}  // namespace textfont

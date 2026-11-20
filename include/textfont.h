#pragma once

#include <Arduino.h>

// 2-bit Chinese text fonts (Noto Sans SC, GB2312 + ASCII) for text the UI fonts do not cover:
// iPhone notifications, file names, the 分享 reader. Data: data/font_text16.bin and font_text24.bin
// (tools/make_textfont.py), linked in through board_build.embed_files. Renders into the 2-bit card
// format of messages.h. The plain functions use the 16 px face; Style picks 24 px and / or 2x pixels.
namespace textfont {

enum class Face : std::uint8_t { Small = 0, Large = 1 };    // 16 px / 24 px

struct Style {
    Face face;
    int scale;                                       // 1, or 2 = every font pixel drawn as 2 x 2
    Style(Face f = Face::Small, int s = 1) : face(f), scale(s) {}
};

bool ready();
int lineHeight();                                    // 20 px (16 px face)
int lineHeight(Style style);
int width(const char* utf8);
// Longest prefix that fits maxWidth px; "…" is appended when the text had to be cut.
String fit(const char* utf8, int maxWidth);
// Draws into a 2-bit bitmap (stride (w + 3) / 4, MSB first) with the line box's top at y; glyph
// levels are scaled by level (1..3) and clipped to x < maxX. Returns the x after the text.
int draw2bpp(std::uint8_t* bitmap, int w, int h, int x, int y, const char* utf8, int level, int maxX);
int draw2bpp(Style style, std::uint8_t* bitmap, int w, int h, int x, int y, const char* utf8, int level, int maxX);
// For wrapping long text (分享 reader): next code point of [p, end) (advances p; bad bytes = U+FFFD)
// and the advance width of one code point in px.
std::uint32_t decode(const char*& p, const char* end);
int advance(std::uint32_t code);
int advance(std::uint32_t code, Style style);

}  // namespace textfont

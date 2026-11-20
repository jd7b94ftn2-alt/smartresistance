#include "textfont.h"

#include <algorithm>
#include <cstring>

// Linked from data/font_text16.bin / font_text24.bin by board_build.embed_files (symbol = path with '_').
extern const std::uint8_t kTextFont16[] asm("_binary_data_font_text16_bin_start");
extern const std::uint8_t kTextFont24[] asm("_binary_data_font_text24_bin_start");

namespace textfont {
namespace {

// Header "TF16" u16 count u8 line u8 ascent, then 12-byte entries, then 2-bit bitmaps (see the
// generator). The data sits in memory-mapped flash: every field is read byte by byte, because an
// unaligned 16 / 32-bit load from flash faults on the ESP32-S3.
constexpr int kEntryBytes = 12;

struct Font {                          // plain aggregate (gnu++11: no member initializers)
    const std::uint8_t* source;
    const std::uint8_t* entries;
    const std::uint8_t* bitmaps;
    int glyphCount;
    int line;
    bool checked;
    bool valid;
};
Font fonts[2] = {
    {kTextFont16, nullptr, nullptr, 0, 20, false, false},
    {kTextFont24, nullptr, nullptr, 0, 29, false, false},
};

struct Glyph {
    int w = 0;
    int h = 0;
    int dx = 0;
    int top = 0;
    int advance = 0;
    const std::uint8_t* data = nullptr;
};

Font& fontFor(Face face) {
    Font& f = fonts[face == Face::Large ? 1 : 0];
    if (!f.checked) {
        f.checked = true;
        const std::uint8_t* d = f.source;
        if (d[0] == 'T' && d[1] == 'F' && d[2] == '1' && d[3] == '6') {
            f.glyphCount = d[4] | (d[5] << 8);
            f.line = d[6];
            f.entries = d + 8;
            f.bitmaps = f.entries + static_cast<std::size_t>(f.glyphCount) * kEntryBytes;
            f.valid = true;
        }
    }
    return f;
}

bool lookup(const Font& f, std::uint32_t code, Glyph& g) {
    if (!f.valid || code > 0xFFFF) {
        return false;
    }
    int lo = 0;
    int hi = f.glyphCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const std::uint8_t* e = f.entries + static_cast<std::size_t>(mid) * kEntryBytes;
        const std::uint32_t c = e[0] | (e[1] << 8);
        if (c == code) {
            g.w = e[2];
            g.h = e[3];
            g.dx = static_cast<std::int8_t>(e[4]);
            g.top = static_cast<std::int8_t>(e[5]);
            g.advance = e[6];
            const std::uint32_t offset = e[8] | (e[9] << 8) | (static_cast<std::uint32_t>(e[10]) << 16)
                | (static_cast<std::uint32_t>(e[11]) << 24);
            g.data = f.bitmaps + offset;
            return true;
        }
        if (c < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return false;
}

// Next code point; malformed bytes become U+FFFD. Advances p.
std::uint32_t nextCode(const char*& p) {
    const std::uint8_t b = static_cast<std::uint8_t>(*p++);
    if (b < 0x80) {
        return b;
    }
    int extra = 0;
    std::uint32_t code = 0;
    if ((b & 0xE0) == 0xC0) {
        extra = 1;
        code = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        extra = 2;
        code = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        extra = 3;
        code = b & 0x07;
    } else {
        return 0xFFFD;
    }
    for (int i = 0; i < extra; ++i) {
        const std::uint8_t c = static_cast<std::uint8_t>(*p);
        if ((c & 0xC0) != 0x80) {
            return 0xFFFD;
        }
        ++p;
        code = (code << 6) | (c & 0x3F);
    }
    return code;
}

// Glyph for a code point; unknown characters (emoji, rare hanzi) show as a hollow box.
bool glyphFor(const Font& f, std::uint32_t code, Glyph& g) {
    if (code == '\n' || code == '\r' || code == '\t') {
        code = ' ';
    }
    if (lookup(f, code, g)) {
        return true;
    }
    if (code == 0xFE0F || code == 0x200D || (code >= 0x1F3FB && code <= 0x1F3FF)) {
        g = Glyph();               // emoji modifiers: nothing
        return true;
    }
    return lookup(f, 0x25A1, g) || lookup(f, '?', g);
}

const Font& small() {
    return fontFor(Face::Small);
}

}  // namespace

bool ready() {
    return small().valid;
}

int lineHeight() {
    return small().line;
}

int lineHeight(Style style) {
    return fontFor(style.face).line * std::max(1, style.scale);
}

int width(const char* utf8) {
    const Font& f = small();
    int total = 0;
    for (const char* p = utf8; *p != '\0';) {
        Glyph g;
        if (glyphFor(f, nextCode(p), g)) {
            total += g.advance;
        }
    }
    return total;
}

String fit(const char* utf8, int maxWidth) {
    if (width(utf8) <= maxWidth) {
        return String(utf8);
    }
    const Font& f = small();
    Glyph dots;
    const int ellipsis = lookup(f, 0x2026, dots) ? dots.advance : 0;
    int used = 0;
    const char* p = utf8;
    const char* cut = utf8;
    while (*p != '\0') {
        const char* before = p;
        Glyph g;
        if (!glyphFor(f, nextCode(p), g)) {
            continue;
        }
        if (used + g.advance + ellipsis > maxWidth) {
            p = before;
            break;
        }
        used += g.advance;
        cut = p;
    }
    String out;
    out.reserve(static_cast<unsigned int>(cut - utf8) + 4);
    for (const char* q = utf8; q < cut; ++q) {
        out += *q;
    }
    out += "\xE2\x80\xA6";                // …
    return out;
}

std::uint32_t decode(const char*& p, const char* end) {
    if (p >= end) {
        return 0;
    }
    const std::uint8_t b = static_cast<std::uint8_t>(*p++);
    if (b < 0x80) {
        return b;
    }
    const int extra = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) {
        return 0xFFFD;
    }
    std::uint32_t code = b & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
    for (int i = 0; i < extra; ++i) {
        if (p >= end || (static_cast<std::uint8_t>(*p) & 0xC0) != 0x80) {
            return 0xFFFD;
        }
        code = (code << 6) | (static_cast<std::uint8_t>(*p++) & 0x3F);
    }
    return code;
}

int advance(std::uint32_t code) {
    Glyph g;
    return glyphFor(small(), code, g) ? g.advance : 0;
}

int advance(std::uint32_t code, Style style) {
    Glyph g;
    return glyphFor(fontFor(style.face), code, g) ? g.advance * std::max(1, style.scale) : 0;
}

int draw2bpp(Style style, std::uint8_t* bitmap, int w, int h, int x, int y, const char* utf8, int level, int maxX) {
    const Font& f = fontFor(style.face);
    const int s = std::max(1, std::min(4, style.scale));
    const int stride = (w + 3) / 4;
    maxX = std::min(maxX, w);
    for (const char* p = utf8; *p != '\0';) {
        Glyph g;
        if (!glyphFor(f, nextCode(p), g)) {
            continue;
        }
        if (x + g.advance * s > maxX) {
            break;
        }
        for (int r = 0; r < g.h; ++r) {
            for (int c = 0; c < g.w; ++c) {
                const int k = r * g.w + c;
                int v = (g.data[k >> 2] >> (6 - 2 * (k & 3))) & 3;
                if (v == 0) {
                    continue;
                }
                v = (v * level + 2) / 3;
                for (int sy = 0; sy < s; ++sy) {
                    const int py = y + (g.top + r) * s + sy;
                    if (py < 0 || py >= h) {
                        continue;
                    }
                    for (int sx = 0; sx < s; ++sx) {
                        const int px = x + (g.dx + c) * s + sx;
                        if (px < 0 || px >= maxX) {
                            continue;
                        }
                        std::uint8_t& byte = bitmap[py * stride + (px >> 2)];
                        const int shift = 6 - 2 * (px & 3);
                        const int old = (byte >> shift) & 3;
                        if (v > old) {
                            byte = static_cast<std::uint8_t>((byte & ~(3 << shift)) | (v << shift));
                        }
                    }
                }
            }
        }
        x += g.advance * s;
    }
    return x;
}

int draw2bpp(std::uint8_t* bitmap, int w, int h, int x, int y, const char* utf8, int level, int maxX) {
    return draw2bpp(Style(), bitmap, w, h, x, y, utf8, level, maxX);
}

}  // namespace textfont

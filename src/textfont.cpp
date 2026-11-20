#include "textfont.h"

#include <algorithm>
#include <cstring>

// Linked from data/font_text16.bin by board_build.embed_files (symbol name = path with '_').
extern const std::uint8_t kTextFontStart[] asm("_binary_data_font_text16_bin_start");

namespace textfont {
namespace {

// Header "TF16" u16 count u8 line u8 ascent, then 12-byte entries, then 2-bit bitmaps (see the
// generator). The data sits in memory-mapped flash: every field is read byte by byte, because an
// unaligned 16 / 32-bit load from flash faults on the ESP32-S3.
constexpr int kEntryBytes = 12;
const std::uint8_t* base = nullptr;
const std::uint8_t* entries = nullptr;
const std::uint8_t* bitmaps = nullptr;
int glyphCount = 0;
int line = 20;
bool checked = false;

struct Glyph {
    int w = 0;
    int h = 0;
    int dx = 0;
    int top = 0;
    int advance = 0;
    const std::uint8_t* data = nullptr;
};

bool init() {
    if (checked) {
        return base != nullptr;
    }
    checked = true;
    const std::uint8_t* d = kTextFontStart;
    if (d[0] != 'T' || d[1] != 'F' || d[2] != '1' || d[3] != '6') {
        return false;
    }
    glyphCount = d[4] | (d[5] << 8);
    line = d[6];
    entries = d + 8;
    bitmaps = entries + static_cast<std::size_t>(glyphCount) * kEntryBytes;
    base = d;
    return true;
}

bool lookup(std::uint32_t code, Glyph& g) {
    if (!init() || code > 0xFFFF) {
        return false;
    }
    int lo = 0;
    int hi = glyphCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const std::uint8_t* e = entries + static_cast<std::size_t>(mid) * kEntryBytes;
        const std::uint32_t c = e[0] | (e[1] << 8);
        if (c == code) {
            g.w = e[2];
            g.h = e[3];
            g.dx = static_cast<std::int8_t>(e[4]);
            g.top = static_cast<std::int8_t>(e[5]);
            g.advance = e[6];
            const std::uint32_t offset = e[8] | (e[9] << 8) | (static_cast<std::uint32_t>(e[10]) << 16)
                | (static_cast<std::uint32_t>(e[11]) << 24);
            g.data = bitmaps + offset;
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
bool glyphFor(std::uint32_t code, Glyph& g) {
    if (code == '\n' || code == '\r' || code == '\t') {
        code = ' ';
    }
    if (lookup(code, g)) {
        return true;
    }
    if (code == 0xFE0F || code == 0x200D || (code >= 0x1F3FB && code <= 0x1F3FF)) {
        g = Glyph();               // emoji modifiers: nothing
        return true;
    }
    return lookup(0x25A1, g) || lookup('?', g);
}

}  // namespace

bool ready() {
    return init();
}

int lineHeight() {
    init();
    return line;
}

int width(const char* utf8) {
    int total = 0;
    for (const char* p = utf8; *p != '\0';) {
        Glyph g;
        if (glyphFor(nextCode(p), g)) {
            total += g.advance;
        }
    }
    return total;
}

String fit(const char* utf8, int maxWidth) {
    if (width(utf8) <= maxWidth) {
        return String(utf8);
    }
    Glyph dots;
    const int ellipsis = lookup(0x2026, dots) ? dots.advance : 0;
    int used = 0;
    const char* p = utf8;
    const char* cut = utf8;
    while (*p != '\0') {
        const char* before = p;
        Glyph g;
        if (!glyphFor(nextCode(p), g)) {
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

int draw2bpp(std::uint8_t* bitmap, int w, int h, int x, int y, const char* utf8, int level, int maxX) {
    const int stride = (w + 3) / 4;
    maxX = std::min(maxX, w);
    for (const char* p = utf8; *p != '\0';) {
        Glyph g;
        if (!glyphFor(nextCode(p), g)) {
            continue;
        }
        if (x + g.advance > maxX) {
            break;
        }
        for (int r = 0; r < g.h; ++r) {
            const int py = y + g.top + r;
            if (py < 0 || py >= h) {
                continue;
            }
            for (int c = 0; c < g.w; ++c) {
                const int k = r * g.w + c;
                int v = (g.data[k >> 2] >> (6 - 2 * (k & 3))) & 3;
                if (v == 0) {
                    continue;
                }
                v = (v * level + 2) / 3;
                const int px = x + g.dx + c;
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
        x += g.advance;
    }
    return x;
}

}  // namespace textfont

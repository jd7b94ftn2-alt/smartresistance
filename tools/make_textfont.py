"""Builds data/font_text16.bin: a 16 px, 2-bit anti-aliased font for arbitrary Chinese text
(iPhone notifications rendered on the board, src/textfont.cpp).

Glyphs: printable ASCII + every GB2312 character (6763 hanzi, punctuation, full-width forms) + a few
extras. Font: Noto Sans SC (SIL Open Font License, C:\\Windows\\Fonts\\NotoSansSC-VF.ttf or pass a path).

Layout (little-endian):
    "TF16"  u16 count  u8 lineHeight  u8 ascent
    count x { u16 code; u8 w; u8 h; i8 dx; i8 top; u8 advance; u8 reserved; u32 offset }   sorted by code
    bitmaps: per glyph w*h pixels, 2 bits each, packed 4 per byte, high bits first, no row padding

    python tools/make_textfont.py [font.ttf]
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'data', 'font_text16.bin')
FONT = sys.argv[1] if len(sys.argv) > 1 else r'C:\Windows\Fonts\NotoSansSC-VF.ttf'
SIZE = 16
EXTRA = '…·•—–‘’“”•°℃Ω×÷←→↑↓★☆✓✔✕✖€£¥'


def charset():
    chars = {chr(c) for c in range(0x20, 0x7F)}
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode('gb2312'))
            except UnicodeDecodeError:
                pass
    chars.update(EXTRA)
    return sorted(c for c in chars if ord(c) <= 0xFFFF)


def main():
    font = ImageFont.truetype(FONT, SIZE)
    try:
        font.set_variation_by_name('Regular')
    except Exception:  # noqa: BLE001  (static fonts have no variations)
        pass
    ascent, descent = font.getmetrics()
    line = ascent + descent
    entries, blobs, offset = [], [], 0
    for ch in charset():
        advance = int(round(font.getlength(ch)))
        x0, y0, x1, y1 = font.getbbox(ch, anchor='ls')
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        data = b''
        if ch != ' ' and w and h:
            img = Image.new('L', (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor='ls')
            px = [min(3, (v + 42) // 85) for v in img.tobytes()]       # 4 levels
            if not any(px):
                w = h = 0
            else:
                px += [0] * (-len(px) % 4)
                data = bytes((px[i] << 6) | (px[i + 1] << 4) | (px[i + 2] << 2) | px[i + 3] for i in range(0, len(px), 4))
        else:
            w = h = 0
        top = ascent + y0                                                # rows from the line top
        entries.append(struct.pack('<HBBbbBBI', ord(ch), w, h, max(-128, min(127, x0)), max(-128, min(127, top)),
                                   min(255, advance), 0, offset))
        blobs.append(data)
        offset += len(data)
    head = b'TF16' + struct.pack('<HBB', len(entries), line, ascent)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(head + b''.join(entries) + b''.join(blobs))
    print(f'{len(entries)} glyphs, line {line}px, ascent {ascent}, {os.path.getsize(OUT) // 1024} KB -> {OUT}')


if __name__ == '__main__':
    main()

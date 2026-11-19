"""Preview of the planned TFT look (320x240), drawn with the same primitives the firmware uses."""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
YAHEI = r'C:\Windows\Fonts\msyh.ttc'
F16 = ImageFont.truetype(YAHEI, 16)
F24 = ImageFont.truetype(YAHEI, 24)
F7 = ImageFont.truetype(r'C:\Windows\Fonts\bahnschrift.ttf', 50)

def c565(h):
    r, g, b = int(h[1:3], 16), int(h[3:5], 16), int(h[5:7], 16)
    return (r & 0xF8, g & 0xFC, b & 0xF8)

BG = c565('#0B1220'); CARD = c565('#172133'); BTN = c565('#25334D'); BORDER = c565('#2A3A58')
FG = c565('#E5E9F0'); DIM = c565('#8B97AB'); ACC = c565('#2A9DF4'); ON = c565('#16A34A'); RED = c565('#DC2626')
BAR = c565('#111A2B')
HOME = [('电阻', '0~999kΩ', '#2A9DF4'), ('NTC', '温度模拟', '#22C55E'), ('蓝牙', '连接/透传', '#6366F1'),
        ('旋钮', 'USB 键鼠', '#A855F7'), ('串口', 'J_DBG 监视', '#F97316'), ('曲线', '板温记录', '#14B8A6'),
        ('时钟', '信息屏', '#EC4899'), ('继电器', '循环测试', '#EF4444'), ('设置', '背光/触摸', '#94A3B8')]

def new():
    im = Image.new('RGB', (320, 240), BG)
    return im, ImageDraw.Draw(im)

def rr(d, x, y, w, h, r, fill=None, outline=None):
    d.rounded_rectangle((x, y, x + w - 1, y + h - 1), r, fill=fill, outline=outline)

def text(d, s, x, y, font, fill, anchor='la'):
    d.text((x, y), s, font=font, fill=fill, anchor=anchor)

def status_bar(d, title, home=False, out=False):
    d.rectangle((0, 0, 319, 25), fill=BAR)
    d.line((0, 26, 319, 26), fill=BORDER)
    x = 8
    if not home:
        rr(d, 3, 3, 60, 20, 6, fill=BTN)
        d.line((12, 13, 17, 8), fill=FG, width=2); d.line((12, 13, 17, 18), fill=FG, width=2)
        text(d, '返回', 22, 13, F16, FG, 'lm')
        x = 72
    text(d, title, x, 13, F16, FG, 'lm')
    text(d, '25.1°C  WiFi', 262, 13, F16, DIM, 'rm')
    rr(d, 268, 4, 48, 18, 9, fill=ON if out else BTN)
    text(d, 'ON' if out else 'OFF', 292, 13, F16, (255, 255, 255) if out else DIM, 'mm')

def icon(d, i, cx, cy, col):
    if i == 0:
        pts = [(cx - 9, cy)] + [(cx - 6 + k * 3, cy + (-4 if k % 2 == 0 else 4)) for k in range(5)] + [(cx + 9, cy)]
        d.line(pts, fill=col, width=2)
    elif i == 1:
        rr(d, cx - 3, cy - 9, 7, 13, 3, outline=col); d.ellipse((cx - 5, cy + 2, cx + 5, cy + 10), fill=col)
    elif i == 2:
        d.line([(cx - 5, cy - 4), (cx + 4, cy + 4), (cx, cy + 8), (cx, cy - 8), (cx + 4, cy - 4), (cx - 5, cy + 4)], fill=col, width=2)
    elif i == 3:
        d.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), outline=col, width=2); d.line((cx, cy, cx + 4, cy - 5), fill=col, width=2)
    elif i == 4:
        rr(d, cx - 9, cy - 7, 19, 15, 3, outline=col); d.line([(cx - 5, cy - 3), (cx - 1, cy), (cx - 5, cy + 3)], fill=col, width=2)
        d.line((cx + 1, cy + 3, cx + 5, cy + 3), fill=col, width=2)
    elif i == 5:
        d.line([(cx - 9, cy + 6), (cx - 4, cy - 1), (cx, cy + 3), (cx + 5, cy - 6), (cx + 9, cy - 3)], fill=col, width=2)
    elif i == 6:
        d.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), outline=col, width=2); d.line([(cx, cy - 5), (cx, cy), (cx + 4, cy + 2)], fill=col, width=2)
    elif i == 7:
        d.rectangle((cx - 9, cy - 3, cx - 3, cy + 7), outline=col); d.line((cx - 1, cy + 6, cx + 8, cy - 4), fill=col, width=2)
        d.ellipse((cx + 6, cy + 4, cx + 9, cy + 7), fill=col)
    else:
        for k in range(8):
            a = k * math.pi / 4
            d.line((cx + 6 * math.cos(a), cy + 6 * math.sin(a), cx + 9 * math.cos(a), cy + 9 * math.sin(a)), fill=col, width=3)
        d.ellipse((cx - 6, cy - 6, cx + 6, cy + 6), outline=col, width=2)

def home(sel=0):
    im, d = new()
    status_bar(d, '智能电阻箱', home=True)
    for i, (t, s, col) in enumerate(HOME):
        x, y = 4 + (i % 3) * 106, 30 + (i // 3) * 70
        col = c565(col)
        rr(d, x, y, 102, 66, 8, fill=CARD)
        d.rounded_rectangle((x, y + 10, x + 3, y + 55), 2, fill=col)
        if i == sel:
            rr(d, x, y, 102, 66, 8, outline=ACC); rr(d, x + 1, y + 1, 100, 64, 7, outline=ACC)
        text(d, t, x + 12, y + 23, F24, FG, 'lm')
        text(d, s, x + 12, y + 49, F16, DIM, 'lm')
        tw, sw = d.textlength(t, font=F24), d.textlength(s, font=F16)
        left = 102 - 13 - 10
        if 12 + tw + 3 <= left: icon(d, i, x + 89, y + 18, col)
        elif 12 + sw + 3 <= left: icon(d, i, x + 89, y + 48, col)
    return im

def resistor(value=4847, field=5, out=False):
    im, d = new()
    status_bar(d, '电阻', out=out)
    rr(d, 6, 28, 308, 86, 10, fill=CARD)
    digits = f'{value:06d}'
    leading = True
    for i, ch in enumerate(digits):
        x = 18 + i * 40 + (12 if i >= 3 else 0)
        if ch != '0' or i == 5:
            leading = False
        d.polygon([(x + 6, 40), (x + 26, 40), (x + 16, 32)], fill=BORDER if i != field else ACC)
        d.polygon([(x + 6, 94), (x + 26, 94), (x + 16, 102)], fill=BORDER if i != field else ACC)
        text(d, ch, x + 16, 67, F7, BORDER if leading else FG, 'mm')
        if i == field:
            rr(d, x + 2, 106, 28, 4, 2, fill=ACC)
    text(d, 'Ω', 286, 68, F24, DIM, 'mm')
    text(d, '实际 4847.000 Ω', 12, 133, F24, c565('#5CC8FF'), 'lm')
    text(d, '点数字上半 +1 / 下半 -1，旋钮转动修改', 12, 160, F16, DIM, 'lm')
    if out:
        rr(d, 10, 196, 300, 38, 10, fill=ON)
        text(d, '输出：开', 160, 215, F24, (255, 255, 255), 'mm')
    else:
        rr(d, 10, 196, 300, 38, 10, fill=BTN)
        text(d, '输出：关', 160, 215, F24, DIM, 'mm')
    return im

def ntc():
    im, d = new()
    status_bar(d, 'NTC 温度')
    rr(d, 8, 30, 304, 34, 9, fill=BTN)
    d.line([(24, 41), (18, 47), (24, 53)], fill=DIM, width=2); d.line([(296, 41), (302, 47), (296, 53)], fill=DIM, width=2)
    text(d, '10k B3435', 160, 47, F24, FG, 'mm')
    text(d, 'R25 10000 Ω    B 3435', 14, 78, F16, DIM, 'lm')
    rr(d, 8, 90, 304, 48, 10, fill=CARD)
    text(d, '45.0', 232, 114, F7, FG, 'rm')
    text(d, '°C', 244, 114, F24, DIM, 'lm')
    for i, s in enumerate(['-1', '-0.1', '+0.1', '+1']):
        rr(d, 8 + i * 77, 144, 73, 32, 9, fill=BTN, outline=ACC if i in (0, 3) else None)
        text(d, s, 8 + i * 77 + 36, 160, F24, FG, 'mm')
    text(d, 'R 4846.9 Ω  已应用', 14, 188, F16, ACC, 'lm')
    rr(d, 8, 200, 304, 34, 10, fill=ON)
    text(d, '输出：开', 160, 217, F24, (255, 255, 255), 'mm')
    return im

shots = [home(), resistor(), resistor(out=True, field=2), ntc()]
sheet = Image.new('RGB', (2 * 320 + 30, 2 * 240 + 30), (40, 40, 40))
for k, im in enumerate(shots):
    sheet.paste(im, (10 + (k % 2) * 330, 10 + (k // 2) * 250))
sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
sheet.save(os.path.join(OUT, 'ui_preview.png'))


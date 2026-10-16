# Generates docs/wiring_pico2.svg — standalone Pico2Arcade default wiring.
# Usage: python tools/mk_wiring_svg.py docs/wiring_pico2.svg
import sys
OUT = sys.argv[1]

L = {1:'GP0',2:'GP1',3:'GND',4:'GP2',5:'GP3',6:'GP4',7:'GP5',8:'GND',9:'GP6',10:'GP7',11:'GP8',12:'GP9',13:'GND',14:'GP10',15:'GP11',16:'GP12',17:'GP13',18:'GND',19:'GP14',20:'GP15'}
R = {40:'VBUS',39:'VSYS',38:'GND',37:'3V3_EN',36:'3V3',35:'ADC_VREF',34:'GP28',33:'AGND',32:'GP27',31:'GP26',30:'RUN',29:'GP22',28:'GND',27:'GP21',26:'GP20',25:'GP19',24:'GP18',23:'GND',22:'GP17',21:'GP16'}
BX0, BX1, Y0, DY = 480, 640, 160, 28
def ly(p): return Y0 + (p-1)*DY
def ry(p): return Y0 + (40-p)*DY
used_l = {3,7,9,10,11,12,14,15,16,17,18,19,20}
used_r = {36,38,40,29,23,22,21}
C_DISP, C_SW, C_AUD, C_PWR, C_GND = '#2563eb', '#16a34a', '#ea580c', '#dc2626', '#374151'
o = []
a = o.append
W, H = 1120, 860

def wire(pts, c, w=2.2):
    a('<polyline points="%s" fill="none" stroke="%s" stroke-width="%s" stroke-linejoin="round"/>'
      % (" ".join("%g,%g" % (x, y) for x, y in pts), c, w))
def dot(x, y, c): a('<circle cx="%g" cy="%g" r="3.6" fill="%s"/>' % (x, y, c))
def text(x, y, s, size=12, anchor='start', c='#111827', w='400'):
    a('<text x="%g" y="%g" font-size="%g" text-anchor="%s" fill="%s" font-weight="%s">%s</text>'
      % (x, y, size, anchor, c, w, s))
def gnd(x, y):
    a('<g stroke="%s" stroke-width="2"><line x1="%g" y1="%g" x2="%g" y2="%g"/>'
      '<line x1="%g" y1="%g" x2="%g" y2="%g"/><line x1="%g" y1="%g" x2="%g" y2="%g"/>'
      '<line x1="%g" y1="%g" x2="%g" y2="%g"/></g>'
      % (C_GND, x, y, x, y+10, x-9, y+10, x+9, y+10, x-6, y+14, x+6, y+14, x-3, y+18, x+3, y+18))
def switch(x, y, label, lx, anchor):
    # push button: terminals at x and x+40, plunger above
    a('<g stroke="%s" stroke-width="2" fill="#fff"><circle cx="%g" cy="%g" r="3.5"/><circle cx="%g" cy="%g" r="3.5"/>'
      '<line x1="%g" y1="%g" x2="%g" y2="%g"/><line x1="%g" y1="%g" x2="%g" y2="%g"/></g>'
      % (C_SW, x, y, x+40, y, x-3, y-9, x+43, y-9, x+20, y-9, x+20, y-16))
    text(lx, y+4, label, 12, anchor, '#065f46', '600')

a('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
  'font-family="Segoe UI, Helvetica, Arial, sans-serif">' % (W, H, W, H))
a('<rect width="%d" height="%d" fill="#ffffff"/>' % (W, H))
text(40, 44, 'Pico2Arcade — standalone Raspberry Pi Pico 2 wiring', 22, 'start', '#111827', '700')
text(40, 68, 'Default pins (buttons and audio can be changed in ARCADE.INI on the ROM drive; display pins are fixed)', 13, 'start', '#4b5563')

# ── Pico 2 board ──
a('<rect x="%d" y="120" width="%d" height="%d" rx="10" fill="#1f6f43" stroke="#14532d" stroke-width="2"/>'
  % (BX0, BX1-BX0, ly(20)-120+40))
cx = (BX0+BX1)/2
a('<rect x="%g" y="104" width="56" height="26" rx="4" fill="#9ca3af" stroke="#4b5563"/>' % (cx-28))
text(cx, 98, 'USB', 11, 'middle', '#4b5563')
a('<rect x="%g" y="380" width="60" height="60" rx="4" fill="#111827"/>' % (cx-30))
text(cx, 414, 'RP2350', 11, 'middle', '#e5e7eb', '600')
text(cx, 464, 'Pico 2', 16, 'middle', '#ffffff', '700')
text(cx, 482, '(top view)', 11, 'middle', '#d1fae5')
for p, n in L.items():
    y = ly(p); u = p in used_l
    a('<circle cx="%d" cy="%d" r="6" fill="%s" stroke="#78350f" stroke-width="1"/>' % (BX0, y, "#fbbf24" if u else "#d1d5db"))
    text(BX0+12, y+4, n, 11, 'start', '#ffffff' if u else '#a7f3d0', '600' if u else '400')
    text(BX0-9, y-7, str(p), 9, 'end', '#6b7280')
for p, n in R.items():
    y = ry(p); u = p in used_r
    a('<circle cx="%d" cy="%d" r="6" fill="%s" stroke="#78350f" stroke-width="1"/>' % (BX1, y, "#fbbf24" if u else "#d1d5db"))
    text(BX1-12, y+4, n, 11, 'end', '#ffffff' if u else '#a7f3d0', '600' if u else '400')
    text(BX1+9, y-7, str(p), 9, 'start', '#6b7280')

# ── Display (ST7789 or ILI9341, 240x320 SPI) ──
DX0, DX1 = 60, 250
dpins = [('BL', None), ('VCC', None), ('GND', 3), ('CS', 7), ('SCL', 9), ('SDA', 10), ('DC', 11), ('RES', 12)]
dy0 = 190
a('<rect x="%d" y="150" width="%d" height="250" rx="8" fill="#1e3a8a" stroke="#1e40af" stroke-width="2"/>' % (DX0, DX1-DX0))
a('<rect x="%d" y="170" width="118" height="160" rx="3" fill="#0b1020" stroke="#93c5fd"/>' % (DX0+18))
text(DX0+77, 245, 'ST7789 / ILI9341', 12, 'middle', '#bfdbfe', '700')
text(DX0+77, 263, '240×320 SPI', 11, 'middle', '#bfdbfe')
text(DX0+77, 380, 'Display', 15, 'middle', '#ffffff', '700')
# lanes chosen so no display wires cross (top display pin -> rightmost lane)
lanes = {'CS': 420, 'SCL': 400, 'SDA': 380, 'DC': 360, 'RES': 340}
for i, (n, pin) in enumerate(dpins):
    y = dy0 + i*24
    a('<circle cx="%d" cy="%d" r="5" fill="#fbbf24" stroke="#78350f"/>' % (DX1, y))
    text(DX1-10, y+4, n, 11, 'end', '#ffffff', '600')
    if n in lanes:
        x = lanes[n]; wire([(DX1, y), (x, y), (x, ly(pin)), (BX0, ly(pin))], C_DISP)
    elif n == 'GND':
        wire([(DX1, y), (450, y), (450, ly(3)), (BX0, ly(3))], C_GND)
    else:
        wire([(DX1, y), (284, y)], C_PWR)
        a('<rect x="284" y="%d" width="118" height="20" rx="10" fill="#fee2e2" stroke="%s"/>' % (y-10, C_PWR))
        text(343, y+4, '3V3 (Pico pin 36)', 10.5, 'middle', '#991b1b', '600')
text(DX0, 424, 'SCL = SCLK, SDA = MOSI, RES = RST.', 11, 'start', '#4b5563')
text(DX0, 440, 'Pin order differs between modules —', 11, 'start', '#4b5563')
text(DX0, 456, 'follow the labels, not the positions.', 11, 'start', '#4b5563')

# 3V3 tag at the Pico
y36 = ry(36)
wire([(BX1, y36), (672, y36)], C_PWR)
a('<rect x="672" y="%d" width="134" height="20" rx="10" fill="#fee2e2" stroke="%s"/>' % (y36-10, C_PWR))
text(739, y36+4, '3V3 → display VCC, BL', 10.5, 'middle', '#991b1b', '600')

# ── Controls, left side (joystick + FIRE + START) ──
BUSL = 270
left_sw = [('UP', 14), ('DOWN', 15), ('LEFT', 16), ('RIGHT', 17), ('FIRE', 19), ('START', 20)]
for n, pin in left_sw:
    y = ly(pin)
    switch(310, y, n, BUSL-10, 'end')
    wire([(BUSL, y), (310-3.5, y)], C_GND)
    wire([(350+3.5, y), (BX0, y)], C_SW)
wire([(BUSL, ly(14)), (BUSL, ly(20))], C_GND)
wire([(BUSL, ly(18)), (BX0, ly(18))], C_GND)
for n, pin in left_sw: dot(BUSL, ly(pin), C_GND)
dot(BUSL, ly(18), C_GND)
a('<rect x="196" y="%d" width="190" height="%d" rx="8" fill="none" stroke="#9ca3af" stroke-dasharray="5,4"/>'
  % (ly(14)-26, ly(17)-ly(14)+44))
text(202, ly(14)-32, 'Joystick (4 switches)', 11, 'start', '#4b5563', '600')

# ── Controls, right side (MENU, COIN) ──
BUSR = 830
for n, pin in [('MENU', 22), ('COIN', 21)]:
    y = ry(pin)
    wire([(BX1, y), (750-3.5, y)], C_SW)
    switch(750, y, n, BUSR+12, 'start')
    wire([(790+3.5, y), (BUSR, y)], C_GND)
    dot(BUSR, y, C_GND)
wire([(BUSR, ry(23)), (BUSR, ry(21))], C_GND)
wire([(BX1, ry(23)), (BUSR, ry(23))], C_GND)
dot(BUSR, ry(23), C_GND)

# ── Amplifier (PAM8302A) + speaker ──
AX0, AX1, AY0 = 870, 990, 150
a('<rect x="%d" y="%d" width="%d" height="150" rx="8" fill="#7c2d12" stroke="#431407" stroke-width="2"/>' % (AX0, AY0, AX1-AX0))
text((AX0+AX1)/2, AY0+128, 'PAM8302A', 12, 'middle', '#ffffff', '700')
text((AX0+AX1)/2, AY0+143, 'class-D amp', 10, 'middle', '#fed7aa')
for n, y in [('VIN', 175), ('GND', 203), ('A+', 231), ('A−', 259)]:
    a('<circle cx="%d" cy="%d" r="5" fill="#fbbf24" stroke="#78350f"/>' % (AX0, y))
    text(AX0+10, y+4, n, 11, 'start', '#ffffff', '600')
# VIN <- VBUS (pin 40)
wire([(BX1, ry(40)), (700, ry(40)), (700, 175), (AX0, 175)], C_PWR)
text(708, 170, 'VBUS 5 V', 10.5, 'start', '#991b1b', '600')
# GND <- pin 38
wire([(BX1, ry(38)), (720, ry(38)), (720, 203), (AX0, 203)], C_GND)
# A- -> GND
wire([(AX0, 259), (852, 259), (852, 272)], C_GND); gnd(852, 272)
# A+ <- GP22 (pin 29), straight in: PAM8302A modules filter the PWM themselves
y22 = ry(29)
wire([(BX1, y22), (840, y22), (840, 231), (AX0, 231)], C_AUD)
text(708, y22-8, 'GP22 audio', 10.5, 'start', '#9a3412', '600')
# outputs -> speaker
for n, y in [('+', 190), ('−', 230)]:
    a('<circle cx="%d" cy="%d" r="5" fill="#fbbf24" stroke="#78350f"/>' % (AX1, y))
    text(AX1-10, y+4, n, 12, 'end', '#ffffff', '700')
wire([(AX1, 190), (1040, 190), (1040, 196)], C_AUD)
wire([(AX1, 230), (1040, 230), (1040, 224)], C_AUD)
a('<g stroke="#111827" stroke-width="2" fill="#e5e7eb"><rect x="1030" y="196" width="20" height="28"/>'
  '<polygon points="1050,196 1074,180 1074,240 1050,224"/></g>')
text(1052, 262, 'Speaker', 12, 'middle', '#111827', '600')
text(1052, 277, '4–8 Ω', 10.5, 'middle', '#4b5563')

# ── Notes + legend ──
ny = 772
a('<line x1="40" y1="%d" x2="%d" y2="%d" stroke="#e5e7eb"/>' % (ny-26, W-40, ny-26))
notes = [
    'Every button is a switch between its GPIO and GND — no resistors needed (the Pico’s internal pull-ups are used).',
    'Hold MENU while powering on for the USB ROM drive (the first boot opens it automatically). The amp’s optional SD (shutdown) pin can stay unconnected.',
    'Not usable for buttons: GPIO 5–9 (display), 23–25 and 29 (Pico 2 internals). Optional in ARCADE.INI: fire2, fire3, nav_up/down, analog stick, amp enable, backlight.',
]
for i, s in enumerate(notes): text(40, ny+i*20, '• ' + s, 12, 'start', '#374151')
x = 40
for c, n in [(C_DISP, 'display'), (C_SW, 'buttons'), (C_AUD, 'audio'), (C_PWR, 'power'), (C_GND, 'ground')]:
    a('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="3"/>' % (x, ny+66, x+26, ny+66, c))
    text(x+32, ny+70, n, 12, 'start', '#374151'); x += 110
a('</svg>')
open(OUT, 'w', encoding='utf-8').write('\n'.join(o))
print('wrote', OUT)

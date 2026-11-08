#!/usr/bin/env python3
"""Generate the point-to-point wiring diagram (wiring.svg). Render with:
    rsvg-convert -b white -z 1 wiring.svg -o wiring.png
"""
from pathlib import Path

W, H = 1800, 1180
out = []
add = out.append
FONT = "DejaVu Sans, Arial, sans-serif"


def text(x, y, s, size=14, fill="#222", anchor="start", weight="normal", extra=""):
    add(f'<text x="{x}" y="{y}" font-family="{FONT}" font-size="{size}" fill="{fill}" '
        f'text-anchor="{anchor}" font-weight="{weight}" {extra}>{s}</text>')


def rect(x, y, w, h, fill, stroke="none", sw=1, rx=0, extra=""):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" '
        f'stroke="{stroke}" stroke-width="{sw}" {extra}/>')


def circle(x, y, r, fill, stroke="none", sw=1):
    add(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>')


# ------------------------------------------------------------------ colours --
C = {
    "5v": "#e53935", "3v3": "#fb8c00", "gnd": "#212121",
    "pres": "#fdd835", "cut": "#8e24aa", "tan": "#00acc1",
    "ledr": "#ff5c8a", "ledg": "#43a047", "ledb": "#1e63d6",
}


def darker(hexc, f=0.55):
    r, g, b = (int(hexc[i:i + 2], 16) for i in (1, 3, 5))
    return "#%02x%02x%02x" % (int(r * f), int(g * f), int(b * f))


wires = []    # drawn last, on top of the components
under = []    # parts of wires that run under the SuperMini (drawn below chip/pads)
joints = []   # solder joints, drawn on top of everything


def path_d(pts):
    return "M " + " L ".join(f"{x},{y}" for x, y in pts)


def wire(color, pts, under_from=None):
    """pts[under_from:] runs under the SuperMini and ends on a pad."""
    if under_from is None:
        wires.append((color, pts))
    else:
        wires.append((color, pts[:under_from + 1]))
        under.append((color, pts[under_from:]))
    joints.append((color, pts[0]))
    joints.append((color, pts[-1]))


def draw_under():
    for color, pts in under:
        add(f'<path d="{path_d(pts)}" fill="none" stroke="#c8ccd2" stroke-width="5" '
            f'stroke-dasharray="9 6" stroke-linejoin="round" opacity="0.9"/>')


def draw_wires():
    for color, pts in wires:
        d = path_d(pts)
        add(f'<path d="{d}" fill="none" stroke="{darker(C[color])}" stroke-width="10" '
            f'stroke-linejoin="round" stroke-linecap="round"/>')
        add(f'<path d="{d}" fill="none" stroke="{C[color]}" stroke-width="6" '
            f'stroke-linejoin="round" stroke-linecap="round"/>')


def draw_joints():
    for color, (x, y) in joints:
        circle(x, y, 5.5, "#c0c0c0", darker(C[color]), 1.5)


add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
rect(0, 0, W, H, "#ffffff")
text(40, 52, "A1 hotend sensor → EBB42 adapter · wiring", 28, weight="bold")
text(40, 82, "Point-to-point, no perfboard. Every coloured line is one wire. "
     "Grey dots are solder joints. All parts shown from the top / component side.", 15, "#555")

# ---------------------------------------------------------- ESP32-C3 SuperMini
BX, BY, BW, BH = 760, 300, 260, 330
PITCH = 34
row = lambda i: BY + 50 + i * PITCH          # pin row y
LX, RX = BX + 22, BX + BW - 22               # pad x, left / right column
LEFT = ["5V", "G", "3.3", "4", "3", "2", "1", "0"]
RIGHT = ["5", "6", "7", "8", "9", "10", "20", "21"]
# USB-C
rect(BX + 80, BY - 28, 100, 48, "#b8bcc2", "#6d7177", 2, rx=10)
rect(BX + 95, BY - 18, 70, 18, "#3a3d42", rx=6)
# PCB
rect(BX, BY, BW, BH, "#1d1f24", "#000", 2, rx=10)
UNDER_MARK = len(out)  # under-board wires get inserted here
# chip
rect(BX + 105, BY + 115, 90, 90, "#2e3136", "#555", 1.5, rx=4)
text(BX + 150, BY + 155, "ESP32-C3", 13, "#bbb", "middle", "bold")
text(BX + 150, BY + 173, "FH4", 11, "#999", "middle")
# crystal, buttons, LED
rect(BX + 125, BY + 222, 50, 22, "#c9ccd1", "#888", 1, rx=3)
for bx_, lbl in ((BX + 82, "BOOT"), (BX + 178, "RST")):
    rect(bx_, BY + 42, 30, 30, "#e8e8e8", "#999", 1, rx=3)
    circle(bx_ + 15, BY + 57, 8, "#3a3d42")
    text(bx_ + 15, BY + 32, lbl, 10, "#ddd", "middle")
rect(BX + 140, BY + 50, 18, 10, "#4f8cff", rx=2)
text(BX + 149, BY + 76, "LED", 9, "#ddd", "middle")
# ceramic antenna at the far end
rect(BX + 90, BY + 280, 120, 34, "#e9e3d3", "#999", 1, rx=3)
text(BX + 150, BY + 302, "antenna", 11, "#666", "middle")
# pads + silkscreen
for i in range(8):
    for x, lbl, anchor, dx in ((LX, LEFT[i], "start", 16), (RX, RIGHT[i], "end", -16)):
        circle(x, row(i), 11, "#d4a73c", "#8c6a1c", 1.5)
        circle(x, row(i), 4.5, "#1d1f24")
        tw = 9 * len(lbl) + 4
        rect(x + dx - (2 if anchor == "start" else tw - 2), row(i) - 9, tw, 18, "#1d1f24")
        text(x + dx, row(i) + 5, lbl, 13, "#fff", anchor, "bold")
text(BX + BW / 2, BY + BH + 75, "ESP32-C3 SuperMini (top view)", 16, anchor="middle", weight="bold")
text(BX + BW / 2, BY + BH + 95, "check pin labels on your clone's silkscreen", 12, "#777", "middle")

# ------------------------------------------------------- FPC breakout board --
FX, FY, FW, FH = 200, 290, 190, 310
fpc_row = lambda k: 330 + k * PITCH          # k = 0..7 top to bottom = pins 8..1
FPX = FX + FW - 22                            # header pin x
rect(FX, FY, FW, FH, "#1f6f3f", "#0e3d22", 2, rx=8)
# ribbon cable coming in from the left
for k in range(9):
    pass
rect(30, 380, 140, 110, "#d89a3a", "#a86f1d", 1.5, rx=2)
for k in range(8):
    rect(30, 388 + k * 12.5, 140, 4, "#b8862e")
text(100, 515, "flat cable 8p 0.5 mm", 12, "#7a5418", "middle")
text(100, 531, "to A1 hotend sensor board", 12, "#7a5418", "middle")
# FPC connector
rect(160, 370, 55, 130, "#222", "#000", 1.5, rx=3)
rect(160, 370, 16, 130, "#7b5a3a", rx=2)
text(188, 440, "FPC", 11, "#bbb", "middle", extra='transform="rotate(-90 188 440)"')
# traces + header pins
labels = ["8 VCC", "7 VCC", "6 TANGLE", "5 n/c", "4 CUTTER", "3 PRESENCE", "2 GND", "1 GND"]
for k in range(8):
    y = fpc_row(k)
    add(f'<line x1="215" y1="{435 + (k - 3.5) * 14}" x2="{FPX - 12}" y2="{y}" '
        f'stroke="#2f9a5c" stroke-width="3"/>')
for k in range(8):
    y = fpc_row(k)
    rect(FPX - 13, y - 13, 26, 26, "#111", rx=2)
    rect(FPX - 7, y - 7, 14, 14, "#d4a73c", "#8c6a1c", 1)
    text(FPX - 22, y + 5, labels[k], 12, "#fff", "end", "bold")
# solder bridges 7-8 and 1-2
for k0 in (0, 6):
    y0, y1 = fpc_row(k0), fpc_row(k0 + 1)
    rect(FPX + 6, y0, 10, y1 - y0, "#c0c0c0", "#777", 1.2, rx=5)
text(FX + FW / 2, FY + FH + 28, "FPC-8P breakout", 16, anchor="middle", weight="bold")
text(FX + FW / 2, FY + FH + 48, "bridge 1+2 and 7+8 (solder blob)", 12, "#777", "middle")
text(FX + FW / 2, FY + FH + 64, "check pin 1 on the cable's printing", 12, "#777", "middle")

# --------------------------------------------------- JST-XH plug for EBB42 --
JX = 1240
jst_y = {5: 282, 4: 316, 3: 350, 2: 384, 1: 418}
rect(JX, 260, 52, 182, "#f3ecd8", "#b9ac87", 2, rx=4)
rect(JX + 52, 265, 10, 172, "#e3d8b8", "#b9ac87", 1.5, rx=2)
for p, y in jst_y.items():
    rect(JX + 14, y - 10, 24, 20, "#d7caa4", "#b9ac87", 1, rx=2)
    text(JX + 26, y + 5, str(p), 12, "#555", "middle", "bold")
jst_lbl = {5: "+5V", 4: "GND", 3: "PB6 · presence", 2: "PB5 · cutter", 1: "PB7 · tangle"}
for p, y in jst_y.items():
    text(JX + 76, y + 5, jst_lbl[p], 13, "#333")
text(JX + 26, 470, "JST-XH 5p plug", 16, anchor="middle", weight="bold")
text(JX + 26, 490, "→ EBB42 Endstop port", 13, "#555", "middle")
text(JX + 26, 506, "check order vs EBB silkscreen", 12, "#777", "middle")

# ------------------------------------------------- RGB LED + resistors ------
# legs point up; seen from the flat side so the order left->right is B G K R
LEDX = {"B": 1060, "G": 1094, "K": 1128, "R": 1162}
LEG_TOP, LEG_BOT = 630, 800
for k, x in LEDX.items():
    top = LEG_TOP
    add(f'<line x1="{x}" y1="{top}" x2="{x}" y2="{LEG_BOT}" stroke="#9ea3a8" stroke-width="4"/>')
# body
add(f'<path d="M 1068 940 L 1068 850 A 43 43 0 0 1 1154 850 L 1154 940 Z" '
    f'fill="#eef2f6" stroke="#9aa4ae" stroke-width="2" opacity="0.95"/>')
rect(1058, 932, 106, 14, "#e2e8ee", "#9aa4ae", 2, rx=2)
rect(1056, 800, 110, 14, "#dfe5ea", "#9aa4ae", 1.5, rx=2)
for c, cx in (("#ff4d4d", 1095), ("#3cc35a", 1111), ("#3d7bff", 1127)):
    circle(cx, 880, 6, c)
text(1111, 975, "5 mm RGB LED, common cathode", 16, anchor="middle", weight="bold")
text(1111, 995, "longest leg = K (cathode)", 12, "#777", "middle")
text(1290, 880, "seen from the back: B G K R", 12, "#777")
text(1290, 897, "(usual order R K G B from the front)", 12, "#777")
text(1290, 914, "check each leg with a 3 V coin cell", 12, "#777")
for k, x in LEDX.items():
    text(x, 828 if k != "K" else 828, k, 12, "#333", "middle", "bold")


def resistor(x, y_top, bands, label):
    rect(x - 8, y_top, 16, 46, "#d8c28e", "#9c8550", 1.2, rx=6)
    for i, bc in enumerate(bands):
        rect(x - 8, y_top + 7 + i * 8, 16, 4, bc)
    lx, ly = x + 17, y_top + 23
    text(lx, ly, label, 11, "#333", "middle", "bold", f'transform="rotate(-90 {lx} {ly})"')


RES_TOP = 660
BAND_220 = ["#d32f2f", "#d32f2f", "#6d4c41", "#c9a227"]
BAND_47 = ["#fbc02d", "#7b1fa2", "#111", "#c9a227"]
resistor(LEDX["R"], RES_TOP, BAND_220, "220Ω")
resistor(LEDX["G"], RES_TOP, BAND_47, "47Ω")
resistor(LEDX["B"], RES_TOP, BAND_47, "47Ω")

# --------------------------------------------------------------- wires ------
# sensor side (FPC breakout -> SuperMini left column)
wire("3v3", [(FPX, fpc_row(1)), (720, fpc_row(1)), (720, row(2)), (LX, row(2))])
wire("tan", [(FPX, fpc_row(2)), (680, fpc_row(2)), (680, row(4)), (LX, row(4))])
wire("cut", [(FPX, fpc_row(4)), (640, fpc_row(4)), (640, row(6)), (LX, row(6))])
wire("pres", [(FPX, fpc_row(5)), (600, fpc_row(5)), (600, row(7)), (LX, row(7))])
# sensor GND: pin 1, down, then under the SuperMini to the G pad
wire("gnd", [(FPX, fpc_row(7)), (430, fpc_row(7)), (430, 655), (830, 655), (830, BY + BH),
             (830, row(1) + 5), (LX, row(1) + 5)], under_from=4)

# EBB side (SuperMini right column -> JST)
wire("pres", [(RX, row(0)), (JX + 14, jst_y[3])])
wire("cut", [(RX, row(1)), (JX + 14, jst_y[2])])
wire("tan", [(RX, row(2)), (JX + 14, jst_y[1])])
# EBB 5V and GND over the top to the left column
wire("5v", [(JX + 14, jst_y[5]), (1170, jst_y[5]), (1170, 200), (745, 200), (745, row(0)),
            (LX, row(0))])
wire("gnd", [(JX + 14, jst_y[4]), (1150, jst_y[4]), (1150, 220), (765, 220), (765, row(1) - 4),
             (LX, row(1) - 4)])

# LED: resistor tops -> GPIO10/20/21, cathode -> G (under the board)
wire("ledr", [(LEDX["R"], LEG_TOP), (LEDX["R"], row(5)), (RX, row(5))])
wire("ledg", [(LEDX["G"], LEG_TOP), (LEDX["G"], row(6)), (RX, row(6))])
wire("ledb", [(LEDX["B"], LEG_TOP), (LEDX["B"], row(7)), (RX, row(7))])
wire("gnd", [(LEDX["K"], LEG_TOP), (LEDX["K"], 470), (BX + BW, 470), (848, 470), (848, row(1) - 5),
             (LX, row(1) - 5)], under_from=2)

# under-board wires go between the PCB fill and the chip/pads
mark = len(out)
draw_under()
seg = out[mark:]
del out[mark:]
out[UNDER_MARK:UNDER_MARK] = seg
draw_wires()
DX = 930
rect(DX, 188, 64, 24, "#1b1b1b", "#000", 1.5, rx=5)
rect(DX + 6, 188, 9, 24, "#cfcfcf")           # cathode band toward the SuperMini
text(DX + 32, 178, "D1 SS14 (optional)", 12, "#333", "middle", "bold")
text(DX + 32, 238 + 4, "band → SuperMini", 11, "#777", "middle")

# wire labels
text(745 - 8, 195 - 10, "+5V", 12, darker(C["5v"]), "end", "bold")
text(1150 + 30, 240, "GND", 12, "#333", "start", "bold")
add(f'<line x1="440" y1="785" x2="490" y2="785" stroke="#9aa0a6" stroke-width="5" stroke-dasharray="9 6"/>')
text(500, 790, "dashed = wire runs under the SuperMini to the G pad.", 13, "#444")
text(500, 808, "The G pad takes 3 wires: sensor GND, EBB GND, LED K.", 13, "#444")
draw_joints()

# --------------------------------------------------------- connection table --
TY = 1035
rect(30, TY - 30, W - 60, 140, "#f6f7f9", "#e1e4e8", 1, rx=8)
rows = [
    ("3v3", "FPC 7 (+8)", "3.3", "sensor supply"),
    ("gnd", "FPC 1 (+2)", "G", "sensor ground"),
    ("pres", "FPC 3", "GPIO0", "presence in"),
    ("cut", "FPC 4", "GPIO1", "cutter in"),
    ("tan", "FPC 6", "GPIO3", "tangle in"),
    ("5v", "JST 5 +5V", "5V", "supply (via D1)"),
    ("gnd", "JST 4 GND", "G", "ground"),
    ("pres", "JST 3 PB6", "GPIO5", "presence out"),
    ("cut", "JST 2 PB5", "GPIO6", "cutter out"),
    ("tan", "JST 1 PB7", "GPIO7", "tangle out"),
    ("ledr", "LED R + 220Ω", "GPIO10", "red"),
    ("ledg", "LED G + 47Ω", "GPIO20", "green"),
    ("ledb", "LED B + 47Ω", "GPIO21", "blue"),
    ("gnd", "LED K", "G", "LED ground"),
]
cols, per_col = 4, 4
cw = (W - 100) / cols
for i, (c, a, b, note) in enumerate(rows):
    cx = 50 + (i // per_col) * cw
    cy = TY + (i % per_col) * 26
    add(f'<line x1="{cx}" y1="{cy - 5}" x2="{cx + 34}" y2="{cy - 5}" stroke="{darker(C[c])}" '
        f'stroke-width="10" stroke-linecap="round"/>')
    add(f'<line x1="{cx}" y1="{cy - 5}" x2="{cx + 34}" y2="{cy - 5}" stroke="{C[c]}" '
        f'stroke-width="6" stroke-linecap="round"/>')
    text(cx + 48, cy, f"{a}  →  <tspan font-weight='bold'>{b}</tspan>", 13)
    text(cx + 250, cy, note, 12, "#777")

add("</svg>")
Path(__file__).with_name("wiring.svg").write_text("\n".join(out))
print("wrote wiring.svg")

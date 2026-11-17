#!/usr/bin/env python3
"""Deterministic benchmark corpus for svg2img.

Generates fixed SVG documents -- no randomness, no clock, no environment --
so timings are comparable between runs and between machines.  Each case has a
declared size and a rough work estimate; the harness runs each one a few times
and reports wall time plus peak RSS.

    python3 bench/gen.py [outdir]        # default: bench/cases
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# name -> (width, height, scale note, body builder)
CASES = {}


def case(name, w, h, note):
    def deco(fn):
        CASES[name] = {"w": w, "h": h, "note": note, "build": fn}
        return fn
    return deco


@case("paths_200x60", 600, 600, "200 cubic paths, 60 units tall")
def _(w, h):
    out = []
    for i in range(200):
        x = i * 3
        out.append(
            '<path d="M%d,%d C%d,%d %d,%d %d,%d" fill="none" '
            'stroke="#123456" stroke-width="1"/>'
            % (x, (i * 7) % h, x + 10, (i * 13) % h,
               x + 20, (i * 29) % h, x + 30, (i * 31) % h))
    return "".join(out)


@case("circles_500", 2000, 1500, "500 large filled circles")
def _(w, h):
    out = []
    for i in range(500):
        out.append('<circle cx="%d" cy="%d" r="30" fill="#%06x"/>'
                   % ((i * 37) % w, (i * 53) % h, (i * 7919) % 0xFFFFFF))
    return "".join(out)


@case("opacity_300", 200, 200, "300 nested opacity groups")
def _(w, h):
    return ('<g opacity="0.9">' * 300 +
            '<rect x="10" y="10" width="180" height="180" fill="red"/>' +
            '</g>' * 300)


@case("grad_400", 800, 600, "400 gradient-filled circles")
def _(w, h):
    out = []
    for i in range(4):
        out.append('<linearGradient id="g%d" x1="0" y1="0" x2="1" y2="1">'
                   '<stop offset="0" stop-color="#ff0000"/>'
                   '<stop offset="1" stop-color="#0000ff"/></linearGradient>' % i)
    for i in range(400):
        out.append('<circle cx="%d" cy="%d" r="10" fill="url(#g%d)"/>'
                   % ((i * 13) % w, (i * 29) % h, i % 4))
    return "".join(out)


@case("text_1500", 1000, 400, "1500 short text nodes")
def _(w, h):
    out = []
    for i in range(1500):
        out.append('<text x="%d" y="%d" font-size="12" fill="#222222">Hello</text>'
                   % ((i * 17) % w, (i * 23) % h))
    return "".join(out)


@case("dash_200", 800, 600, "200 dashed polylines (dash extraction)")
def _(w, h):
    out = []
    for i in range(200):
        pts = " ".join("%d,%d" % ((i * 7 + k * 11) % w, (i * 13 + k * 7) % h)
                       for k in range(8))
        out.append('<polyline points="%s" fill="none" stroke="#000" '
                   'stroke-width="2" stroke-dasharray="6 4"/>' % pts)
    return "".join(out)


@case("clip_300", 600, 600, "300 clipped groups")
def _(w, h):
    out = ['<defs>']
    for i in range(300):
        out.append('<clipPath id="c%d"><circle cx="%d" cy="%d" r="20"/></clipPath>'
                   % (i, (i * 17) % w, (i * 23) % h))
    out.append('</defs>')
    for i in range(300):
        out.append('<g clip-path="url(#c%d)"><rect x="%d" y="%d" width="60" '
                   'height="60" fill="#%06x"/></g>'
                   % (i, (i * 19) % w, (i * 29) % h, (i * 104729) % 0xFFFFFF))
    return "".join(out)



@case("dashboard_1200x800", 1200, 800, "dashboard: cards, bars, gradients, labels")
def _(w, h):
    out = ['<defs>',
           '<linearGradient id="bar" x1="0" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#4f8ef7"/>'
           '<stop offset="1" stop-color="#1c4fb8"/></linearGradient>',
           '<linearGradient id="head" x1="0" y1="0" x2="1" y2="0">'
           '<stop offset="0" stop-color="#0b1f3a"/>'
           '<stop offset="1" stop-color="#1d4f91"/></linearGradient>',
           '<radialGradient id="dot" cx="0.5" cy="0.5" r="0.5">'
           '<stop offset="0" stop-color="#ffd166"/>'
           '<stop offset="1" stop-color="#e08b00"/></radialGradient>',
           '</defs>',
           '<rect width="%d" height="%d" fill="#eef2f7"/>' % (w, h),
           '<rect width="%d" height="72" fill="url(#head)"/>' % w,
           '<text x="32" y="46" font-size="28" fill="#ffffff">Operations</text>']
    for i in range(6):
        x = 32 + i * 190
        out.append('<rect x="%d" y="104" width="166" height="112" rx="10" '
                   'fill="#ffffff" stroke="#d7dee8"/>' % x)
        out.append('<text x="%d" y="134" font-size="12" fill="#667085">Series %d</text>'
                   % (x + 14, i + 1))
        out.append('<text x="%d" y="176" font-size="30" fill="#12233f">%d.%d</text>'
                   % (x + 14, 30 + i * 7, i * 3))
        out.append('<circle cx="%d" cy="194" r="5" fill="url(#dot)"/>' % (x + 18))
    for i in range(18):
        bh = 80 + ((i * 37) % 220)
        x = 56 + i * 62
        out.append('<rect x="%d" y="%d" width="38" height="%d" rx="4" '
                   'fill="url(#bar)"/>' % (x, 700 - bh, bh))
        out.append('<text x="%d" y="726" font-size="10" fill="#667085">%02d</text>'
                   % (x + 4, i + 1))
    out.append('<path d="M56,470 L118,430 L180,455 L242,380 L304,410 L366,330 '
               'L428,360 L490,290 L552,310 L614,250 L676,280 L738,210 L800,240 '
               'L862,180 L924,205 L986,150 L1048,175 L1110,120" fill="none" '
               'stroke="#e5484d" stroke-width="3" stroke-linejoin="round"/>')
    out.append('<rect x="32" y="744" width="1136" height="1" fill="#d7dee8"/>')
    return "".join(out)


@case("map_1000x700", 1000, 700, "map: roads, water, parks, markers, labels")
def _(w, h):
    out = ['<defs>',
           '<linearGradient id="water" x1="0" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#a8d8f0"/>'
           '<stop offset="1" stop-color="#4a9fc8"/></linearGradient>',
           '</defs>',
           '<rect width="%d" height="%d" fill="#f2efe9"/>' % (w, h),
           '<path d="M0,540 C180,500 260,590 400,560 C560,525 660,610 820,570 '
           'C900,550 960,565 1000,545 L1000,700 L0,700 Z" fill="url(#water)"/>',
           '<path d="M60,90 C180,140 240,60 360,120 C500,190 600,80 760,150 '
           'C860,195 930,140 1000,170 L1000,240 C900,210 840,260 740,220 '
           'C600,160 500,250 360,185 C240,130 180,205 60,160 Z" fill="#cfe3c2"/>']
    for i in range(40):
        x0, y0 = (i * 53) % w, (i * 29) % h
        x1, y1 = (x0 + 140 + i * 7) % w, (y0 + 90 + i * 11) % h
        out.append('<path d="M%d,%d C%d,%d %d,%d %d,%d" fill="none" '
                   'stroke="#ffffff" stroke-width="7"/>'
                   % (x0, y0, x0 + 30, y0 - 20, x1 - 30, y1 + 20, x1, y1))
        out.append('<path d="M%d,%d C%d,%d %d,%d %d,%d" fill="none" '
                   'stroke="#e0b25c" stroke-width="3"/>'
                   % (x0, y0, x0 + 30, y0 - 20, x1 - 30, y1 + 20, x1, y1))
    out.append('<path d="M120,650 C220,560 300,630 380,540 C460,450 560,520 '
               '640,430 C720,340 820,400 900,310" fill="none" '
               'stroke="#2f6fed" stroke-width="4" stroke-dasharray="12 8"/>')
    for i in range(12):
        cx, cy = 90 + (i * 77) % (w - 160), 110 + (i * 47) % (h - 220)
        out.append('<circle cx="%d" cy="%d" r="9" fill="#e5484d" '
                   'stroke="#ffffff" stroke-width="3"/>' % (cx, cy))
        out.append('<text x="%d" y="%d" font-size="11" fill="#243b53">%s</text>'
                   % (cx + 14, cy + 4, "Stop %d" % (i + 1)))
    out.append('<text x="40" y="52" font-size="26" fill="#243b53">Transit map</text>')
    return "".join(out)


@case("illustration_800x800", 800, 800, "illustration: mask, pattern, blur, layers")
def _(w, h):
    out = ['<defs>',
           '<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#2b2d6e"/>'
           '<stop offset="0.55" stop-color="#d46a8c"/>'
           '<stop offset="1" stop-color="#ffc46b"/></linearGradient>',
           '<radialGradient id="sun" cx="0.5" cy="0.5" r="0.5">'
           '<stop offset="0" stop-color="#fff4c2"/>'
           '<stop offset="1" stop-color="#ffb31a"/></radialGradient>',
           '<pattern id="dots" width="18" height="18" patternUnits="userSpaceOnUse">'
           '<circle cx="9" cy="9" r="2" fill="#ffffff" opacity="0.35"/>'
           '</pattern>',
           '<mask id="fade"><rect width="800" height="800" fill="white"/>'
           '<ellipse cx="400" cy="650" rx="420" ry="220" fill="black"/></mask>',
           '<filter id="soft"><feGaussianBlur stdDeviation="6"/></filter>',
           '</defs>',
           '<rect width="800" height="800" fill="url(#sky)"/>',
           '<circle cx="590" cy="220" r="86" fill="url(#sun)"/>',
           '<circle cx="590" cy="220" r="130" fill="url(#sun)" opacity="0.18"/>',
           '<rect width="800" height="800" fill="url(#dots)" mask="url(#fade)"/>',
           '<path d="M0,560 L120,430 L230,520 L340,390 L480,545 L620,430 '
           'L800,560 L800,800 L0,800 Z" fill="#1b2340" opacity="0.95"/>',
           '<path d="M0,640 L150,540 L290,620 L430,500 L600,630 L800,540 '
           'L800,800 L0,800 Z" fill="#0e1428"/>']
    for i in range(20):
        x, y = 40 + (i * 97) % 720, 300 + (i * 61) % 260
        out.append('<circle cx="%d" cy="%d" r="2" fill="#fff4c2" opacity="0.8"/>'
                   % (x, y))
    out.append('<g opacity="0.9" filter="url(#soft)">'
               '<rect x="250" y="610" width="300" height="90" rx="18" '
               'fill="#ffcf5c"/></g>')
    out.append('<text x="300" y="668" font-size="34" fill="#2b1d0e" '
               'font-weight="bold">Evening</text>')
    return "".join(out)


@case("icons_1000x1000", 1000, 1000, "icon grid: symbols, use, strokes")
def _(w, h):
    out = ['<defs>',
           '<symbol id="gear" viewBox="0 0 24 24">'
           '<path d="M12 2 L14 5 L18 4 L19 8 L22 9 L20 12 L22 15 L19 16 '
           'L18 20 L14 19 L12 22 L10 19 L6 20 L5 16 L2 15 L4 12 L2 9 '
           'L5 8 L6 4 L10 5 Z" fill="none" stroke="currentColor" '
           'stroke-width="1.6"/><circle cx="12" cy="12" r="3.2" '
           'fill="none" stroke="currentColor" stroke-width="1.6"/></symbol>',
           '<symbol id="heart" viewBox="0 0 24 24">'
           '<path d="M12 21 C4 15 2 10 4.5 6.5 C7 3 11 4 12 7 C13 4 17 3 '
           '19.5 6.5 C22 10 20 15 12 21 Z" fill="none" stroke="currentColor" '
           'stroke-width="1.6"/></symbol>',
           '</defs>',
           '<rect width="%d" height="%d" fill="#101828"/>' % (w, h)]
    colors = ["#7dd3fc", "#fda4af", "#fde68a", "#86efac", "#c4b5fd"]
    for i in range(100):
        x = 30 + (i % 10) * 94
        y = 30 + (i // 10) * 94
        out.append('<rect x="%d" y="%d" width="78" height="78" rx="16" '
                   'fill="#1d2939" stroke="#344054"/>' % (x, y))
        out.append('<g transform="translate(%d,%d) scale(2.2)" color="%s">'
                   '<use href="#%s"/></g>'
                   % (x + 15, y + 15, colors[i % len(colors)],
                      "gear" if i % 2 == 0 else "heart"))
    return "".join(out)


@case("chart_1200x600", 1200, 600, "line/area chart, two series, markers, legend")
def _(w, h):
    out = ['<defs>',
           '<linearGradient id="area" x1="0" y1="0" x2="0" y2="1">'
           '<stop offset="0" stop-color="#3b82f6" stop-opacity="0.55"/>'
           '<stop offset="1" stop-color="#3b82f6" stop-opacity="0.04"/>'
           '</linearGradient>',
           '</defs>',
           '<rect width="%d" height="%d" fill="#ffffff"/>' % (w, h),
           '<text x="48" y="48" font-size="24" fill="#101828">Monthly trend</text>']
    for i in range(7):
        y = 90 + i * 66
        out.append('<line x1="64" y1="%d" x2="1140" y2="%d" stroke="#eaecf0"/>'
                   % (y, y))
        out.append('<text x="32" y="%d" font-size="11" fill="#667085">%d</text>'
                   % (y + 4, (6 - i) * 10))
    pts1, pts2 = [], []
    for i in range(25):
        x = 64 + i * 44
        y1 = 480 - ((i * 37) % 230)
        y2 = 500 - ((i * 23 + 40) % 260)
        pts1.append("%d,%d" % (x, y1))
        pts2.append("%d,%d" % (x, y2))
    out.append('<path d="M64,480 L%s L1140,480 Z" fill="url(#area)"/>'
               % " L".join(pts1))
    out.append('<polyline points="%s" fill="none" stroke="#2563eb" '
               'stroke-width="3"/>' % " ".join(pts1))
    out.append('<polyline points="%s" fill="none" stroke="#e5484d" '
               'stroke-width="2" stroke-dasharray="7 5"/>' % " ".join(pts2))
    for i, p in enumerate(pts1):
        if i % 3: continue
        x, y = p.split(",")
        out.append('<circle cx="%s" cy="%s" r="4" fill="#2563eb" '
                   'stroke="#ffffff" stroke-width="2"/>' % (x, y))
    out.append('<rect x="900" y="24" width="12" height="12" rx="2" fill="#2563eb"/>')
    out.append('<text x="920" y="35" font-size="12" fill="#344054">Revenue</text>')
    out.append('<line x1="1020" y1="30" x2="1050" y2="30" stroke="#e5484d" '
               'stroke-width="2" stroke-dasharray="7 5"/>')
    out.append('<text x="1058" y="35" font-size="12" fill="#344054">Cost</text>')
    for i in range(12):
        out.append('<text x="%d" y="556" font-size="10" fill="#667085">%02d</text>'
                   % (64 + i * 96, i * 2 + 1))
    return "".join(out)

def wrap(w, h, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d">%s</svg>' % (w, h, w, h, body))


def main(argv):
    outdir = argv[0] if argv else os.path.join(HERE, "cases")
    os.makedirs(outdir, exist_ok=True)
    for name, meta in sorted(CASES.items()):
        svg = wrap(meta["w"], meta["h"], meta["build"](meta["w"], meta["h"]))
        path = os.path.join(outdir, name + ".svg")
        with open(path, "w") as f:
            f.write(svg)
        print("%-16s %7d bytes  %dx%d  (%s)"
              % (name, len(svg), meta["w"], meta["h"], meta["note"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
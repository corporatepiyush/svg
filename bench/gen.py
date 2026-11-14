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
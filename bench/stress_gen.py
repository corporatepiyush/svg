#!/usr/bin/env python3
"""Large, feature-dense SVG documents for profiling.

Unlike bench/gen.py (small, single-feature timing cases), each document here
is a big realistic workload that mixes features, so a profile shows where a
heavy real-world render spends its time.  Output is deterministic.

    python3 bench/stress_gen.py [outdir]     # default: bench/stress_cases
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NS = 'xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"'
CASES = {}


def case(name, w, h, note):
    def deco(fn):
        CASES[name] = (w, h, note, fn)
        return fn
    return deco


class Rng:
    """Small LCG so the corpus does not depend on Python's random module."""
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def u(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296.0

    def r(self, a, b):
        return a + (b - a) * self.u()

    def i(self, a, b):
        return int(self.r(a, b + 1))

    def col(self):
        return "#%06x" % self.i(0, 0xFFFFFF)


def f(v):
    return ("%.2f" % v).rstrip("0").rstrip(".")


@case("geo_map", 2400, 1800, "choropleth: 2500 polygons x 200 pts, borders, labels")
def _(w, h):
    rnd = Rng(1)
    out = ['<rect width="100%" height="100%" fill="#cfe8ff"/>']
    for k in range(2500):
        cx, cy, r = rnd.r(0, w), rnd.r(0, h), rnd.r(10, 60)
        pts = []
        for j in range(200):
            a = j / 200 * 2 * math.pi
            rr = r * (0.7 + 0.3 * math.sin(a * 7 + k))
            pts.append("%s,%s" % (f(cx + rr * math.cos(a)), f(cy + rr * math.sin(a))))
        out.append('<polygon points="%s" fill="%s" stroke="#333" stroke-width="0.7" '
                   'fill-rule="evenodd"/>' % (" ".join(pts), rnd.col()))
    for k in range(400):
        out.append('<text x="%s" y="%s" font-size="11" fill="#222">R%d</text>'
                   % (f(rnd.r(0, w)), f(rnd.r(0, h)), k))
    return "".join(out)


@case("timeseries", 2400, 1200, "40 series x 3000 points, area fills, dashed grid")
def _(w, h):
    rnd = Rng(2)
    out = ['<rect width="100%" height="100%" fill="#fff"/>']
    for x in range(0, w, 40):
        out.append('<line x1="%d" y1="0" x2="%d" y2="%d" stroke="#ccc" stroke-dasharray="4 3"/>' % (x, x, h))
    for y in range(0, h, 40):
        out.append('<line x1="0" y1="%d" x2="%d" y2="%d" stroke="#ccc" stroke-dasharray="4 3"/>' % (y, w, y))
    for s in range(40):
        v, d = h / 2, ["M0,%s" % f(h / 2)]
        for i in range(1, 3000):
            v = min(h, max(0, v + rnd.r(-8, 8)))
            d.append("L%s,%s" % (f(i * w / 3000), f(v)))
        path = " ".join(d)
        if s < 8:
            out.append('<path d="%s L%d,%d L0,%d Z" fill="%s" fill-opacity="0.15"/>'
                       % (path, w, h, h, rnd.col()))
        out.append('<path d="%s" fill="none" stroke="%s" stroke-width="1.5" '
                   'stroke-linejoin="round"/>' % (path, rnd.col()))
    return "".join(out)


@case("icon_sheet", 2000, 2000, "6400 <use> of 16 symbols with gradients and clips")
def _(w, h):
    rnd = Rng(3)
    defs = ['<defs>']
    for k in range(16):
        defs.append('<linearGradient id="g%d" x1="0" y1="0" x2="1" y2="1">'
                    '<stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/>'
                    '</linearGradient>' % (k, rnd.col(), rnd.col()))
        defs.append('<clipPath id="c%d"><circle cx="12" cy="12" r="11"/></clipPath>' % k)
        defs.append('<symbol id="s%d" viewBox="0 0 24 24"><g clip-path="url(#c%d)">'
                    '<rect width="24" height="24" fill="url(#g%d)"/>'
                    '<path d="M4 12 L10 18 L20 6" fill="none" stroke="#fff" stroke-width="2.5" '
                    'stroke-linecap="round" stroke-linejoin="round"/>'
                    '<circle cx="%d" cy="%d" r="3" fill="#000" opacity="0.4"/></g></symbol>'
                    % (k, k, k, rnd.i(4, 20), rnd.i(4, 20)))
    defs.append('</defs>')
    out = []
    for i in range(6400):
        out.append('<use href="#s%d" x="%d" y="%d" width="22" height="22"/>'
                   % (i % 16, (i % 80) * 25, (i // 80) * 25))
    return "".join(defs + out)


@case("filter_cards", 1600, 1200, "300 cards with drop shadows, blur, color matrix")
def _(w, h):
    rnd = Rng(4)
    defs = ('<defs><filter id="sh" x="-20%" y="-20%" width="140%" height="140%">'
            '<feGaussianBlur in="SourceAlpha" stdDeviation="4"/><feOffset dx="3" dy="4" result="o"/>'
            '<feFlood flood-color="#000" flood-opacity="0.35"/><feComposite in2="o" operator="in"/>'
            '<feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter>'
            '<filter id="cm"><feColorMatrix type="saturate" values="0.3"/></filter>'
            '<filter id="bl"><feGaussianBlur stdDeviation="8"/></filter></defs>')
    out = [defs, '<rect width="100%" height="100%" fill="#eee"/>']
    for i in range(300):
        x, y = (i % 20) * 80 + 6, (i // 20) * 80 + 6
        flt = ("sh", "cm", "bl")[i % 3]
        out.append('<g filter="url(#%s)"><rect x="%d" y="%d" width="66" height="66" rx="8" fill="%s"/>'
                   '<circle cx="%d" cy="%d" r="18" fill="%s"/></g>'
                   % (flt, x, y, rnd.col(), x + 33, y + 33, rnd.col()))
    return "".join(out)


@case("text_doc", 1600, 2400, "300 lines, 30k glyphs, tspans, CSS classes")
def _(w, h):
    words = ("lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod "
             "tempor incididunt ut labore et dolore magna aliqua").split()
    rnd = Rng(5)
    out = ['<style>.h{font-size:22px;fill:#036}.b{font-size:13px;fill:#222}'
           '.em{fill:#c00}.u{text-decoration:underline}</style>',
           '<rect width="100%" height="100%" fill="#fff"/>']
    y = 30
    for line in range(300):
        cls = "h" if line % 25 == 0 else "b"
        parts = []
        for k in range(14):
            wd = words[rnd.i(0, len(words) - 1)]
            parts.append('<tspan class="em">%s</tspan>' % wd if k % 5 == 0 else wd)
        out.append('<text class="%s" x="20" y="%d">%s</text>' % (cls, y, " ".join(parts)))
        y += 8
    return "".join(out)


@case("gradient_art", 2000, 1500, "3000 transformed shapes, linear/radial, reflect")
def _(w, h):
    rnd = Rng(6)
    defs = ['<defs>']
    for k in range(60):
        if k % 2:
            defs.append('<radialGradient id="g%d" fx="0.3" fy="0.3" spreadMethod="reflect" r="0.4">'
                        '<stop offset="0" stop-color="%s"/><stop offset="0.5" stop-color="%s" stop-opacity="0.6"/>'
                        '<stop offset="1" stop-color="%s"/></radialGradient>'
                        % (k, rnd.col(), rnd.col(), rnd.col()))
        else:
            defs.append('<linearGradient id="g%d" gradientTransform="rotate(%d)" spreadMethod="repeat" x2="0.3">'
                        '<stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>'
                        % (k, rnd.i(0, 90), rnd.col(), rnd.col()))
    defs.append('</defs>')
    out = []
    for i in range(3000):
        tx = 'translate(%s %s) rotate(%d) scale(%s)' % (
            f(rnd.r(0, w)), f(rnd.r(0, h)), rnd.i(0, 360), f(rnd.r(0.5, 2)))
        if i % 2:
            out.append('<ellipse transform="%s" rx="%d" ry="%d" fill="url(#g%d)" opacity="0.85"/>'
                       % (tx, rnd.i(10, 60), rnd.i(10, 40), i % 60))
        else:
            out.append('<rect transform="%s" width="%d" height="%d" rx="6" fill="url(#g%d)"/>'
                       % (tx, rnd.i(20, 90), rnd.i(20, 60), i % 60))
    return "".join(defs + out)


@case("mask_layers", 1600, 1200, "400 masked + clipped groups with nested opacity")
def _(w, h):
    rnd = Rng(7)
    defs = ('<defs><linearGradient id="fade" x2="0" y2="1"><stop offset="0" stop-color="#fff"/>'
            '<stop offset="1" stop-color="#000"/></linearGradient>'
            '<mask id="m" maskContentUnits="objectBoundingBox"><rect width="1" height="1" fill="url(#fade)"/></mask>'
            '<clipPath id="cp" clipPathUnits="objectBoundingBox"><circle cx="0.5" cy="0.5" r="0.5"/></clipPath></defs>')
    out = [defs]
    for i in range(400):
        x, y = rnd.r(0, w - 120), rnd.r(0, h - 120)
        out.append('<g opacity="0.8" mask="url(#m)"><g clip-path="url(#cp)" opacity="0.9">'
                   '<rect x="%s" y="%s" width="120" height="120" fill="%s"/>'
                   '<rect x="%s" y="%s" width="60" height="60" fill="%s"/></g></g>'
                   % (f(x), f(y), rnd.col(), f(x + 30), f(y + 30), rnd.col()))
    return "".join(out)


@case("pattern_fill", 1800, 1400, "1200 shapes with 20 patterns (nested pattern)")
def _(w, h):
    rnd = Rng(8)
    defs = ['<defs><pattern id="dots" width="6" height="6" patternUnits="userSpaceOnUse">'
            '<circle cx="3" cy="3" r="1.5" fill="#333"/></pattern>']
    for k in range(20):
        defs.append('<pattern id="p%d" width="%d" height="%d" patternUnits="userSpaceOnUse" '
                    'patternTransform="rotate(%d)"><rect width="100%%" height="100%%" fill="%s"/>'
                    '<rect width="50%%" height="50%%" fill="url(#dots)"/>'
                    '<path d="M0 0 L%d %d" stroke="%s" stroke-width="2"/></pattern>'
                    % (k, 10 + k, 10 + k, k * 9, rnd.col(), 10 + k, 10 + k, rnd.col()))
    defs.append('</defs>')
    out = []
    for i in range(1200):
        out.append('<circle cx="%s" cy="%s" r="%d" fill="url(#p%d)" stroke="#000"/>'
                   % (f(rnd.r(0, w)), f(rnd.r(0, h)), rnd.i(15, 50), i % 20))
    return "".join(defs + out)


@case("stroke_heavy", 2000, 1600, "3000 thick curved strokes, joins, caps, dashes, arcs")
def _(w, h):
    rnd = Rng(9)
    joins, caps = ("miter", "round", "bevel"), ("butt", "round", "square")
    out = ['<rect width="100%" height="100%" fill="#111"/>']
    for i in range(3000):
        x, y = rnd.r(0, w), rnd.r(0, h)
        d = "M%s,%s C%s,%s %s,%s %s,%s A%d,%d 0 0 1 %s,%s Q%s,%s %s,%s" % tuple(
            [f(x), f(y)] + [f(x + rnd.r(-150, 150)) if j % 2 == 0 else f(y + rnd.r(-150, 150)) for j in range(6)] +
            [rnd.i(10, 60), rnd.i(10, 60), f(x + rnd.r(-80, 80)), f(y + rnd.r(-80, 80))] +
            [f(x + rnd.r(-100, 100)), f(y + rnd.r(-100, 100)), f(x + rnd.r(-100, 100)), f(y + rnd.r(-100, 100))])
        dash = ' stroke-dasharray="%d %d"' % (rnd.i(2, 20), rnd.i(2, 10)) if i % 4 == 0 else ""
        out.append('<path d="%s" fill="none" stroke="%s" stroke-width="%s" stroke-linejoin="%s" '
                   'stroke-linecap="%s"%s stroke-opacity="0.8"/>'
                   % (d, rnd.col(), f(rnd.r(1, 12)), joins[i % 3], caps[i % 3], dash))
    return "".join(out)


@case("css_dashboard", 1920, 1080, "12000 elements styled by 600 CSS rules")
def _(w, h):
    rnd = Rng(10)
    css = ["<style>"]
    for k in range(200):
        css.append(".c%d{fill:%s;stroke:%s;stroke-width:%d}" % (k, rnd.col(), rnd.col(), k % 3))
        css.append("g.row%d rect[data-k='%d']{opacity:0.%d}" % (k % 30, k, 5 + k % 5))
        css.append("#e%d{fill-opacity:0.7}" % (k * 37))
    css.append("rect:first-child{stroke-dasharray:2 1}</style>")
    out = ["".join(css), '<rect width="100%" height="100%" fill="#fafafa"/>']
    n = 0
    for row in range(30):
        out.append('<g class="row%d">' % row)
        for col in range(400):
            out.append('<rect id="e%d" class="c%d" data-k="%d" x="%s" y="%s" width="4" height="%d"/>'
                       % (n, n % 200, n % 200, f(col * 4.8), f(row * 36), rnd.i(4, 34)))
            n += 1
        out.append('</g>')
    return "".join(out)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "stress_cases")
    os.makedirs(outdir, exist_ok=True)
    for name, (w, h, note, fn) in CASES.items():
        body = fn(w, h)
        path = os.path.join(outdir, name + ".svg")
        with open(path, "w") as fh:
            fh.write('<svg %s width="%d" height="%d" viewBox="0 0 %d %d">\n<!-- %s -->\n%s\n</svg>\n'
                     % (NS, w, h, w, h, note, body))
        print("%-14s %5dx%-5d %6.1f MB  %s" % (name, w, h, os.path.getsize(path) / 1e6, note))


if __name__ == "__main__":
    main()

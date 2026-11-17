#!/usr/bin/env python3
"""Complex, hand-shaped SVG documents for correctness and threading tests.

bench/stress_gen.py makes a few huge but repetitive documents (each is one long
line).  These are instead structured like real authored files: one element per
line, 500+ lines each, and 100+ distinct components per document.  A component
is a separately identified piece (a gradient, pattern, filter, mask, clipPath,
marker, symbol, or a widget group with its own id) and every document mixes
many feature kinds.  Output is deterministic.

    python3 bench/complex_gen.py [outdir]     # default: bench/complex_cases
    python3 bench/complex_gen.py --stats      # print lines / components / tags
"""
import base64
import math
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
NS = ('xmlns="http://www.w3.org/2000/svg" '
      'xmlns:xlink="http://www.w3.org/1999/xlink"')


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

    def pick(self, seq):
        return seq[min(int(self.u() * len(seq)), len(seq) - 1)]

    def color(self):
        return '#%02x%02x%02x' % (self.i(0, 255), self.i(0, 255), self.i(0, 255))


def tiny_png(w=24, h=16):
    """A small RGBA gradient PNG for <image> elements."""
    raw = b''
    for y in range(h):
        raw += b'\x00' + b''.join(
            bytes((x * 255 // (w - 1), y * 255 // (h - 1), 160, 255)) for x in range(w))

    def chunk(t, d):
        return (struct.pack('>I', len(d)) + t + d +
                struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return 'data:image/png;base64,' + base64.b64encode(png).decode()


PNG = tiny_png()


class Doc:
    """Collects <defs> and body lines; every component gets a unique id."""
    def __init__(self, w, h, seed, title):
        self.w, self.h = w, h
        self.rng = Rng(seed)
        self.defs = []
        self.body = []
        self.css = []
        self.n = 0
        self.title = title

    def uid(self, kind):
        self.n += 1
        return '%s%d' % (kind, self.n)

    # -- paint servers ----------------------------------------------------
    def lin(self, stops=3, spread=None, units=None):
        r = self.rng
        i = self.uid('lg')
        extra = ''
        if spread:
            extra += ' spreadMethod="%s"' % spread
        if units == 'user':
            extra += (' gradientUnits="userSpaceOnUse" x1="%d" y1="%d" x2="%d" y2="%d"'
                      % (r.i(0, self.w), r.i(0, self.h), r.i(0, self.w), r.i(0, self.h)))
        else:
            extra += ' x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f"' % (
                r.r(0, .3), r.r(0, .3), r.r(.6, 1), r.r(.6, 1))
        self.defs.append('<linearGradient id="%s"%s>' % (i, extra))
        for k in range(stops):
            self.defs.append('  <stop offset="%.2f" stop-color="%s" stop-opacity="%.2f"/>'
                             % (k / max(stops - 1, 1), r.color(), r.r(.5, 1)))
        self.defs.append('</linearGradient>')
        return i

    def rad(self, stops=3, focal=False, spread=None):
        r = self.rng
        i = self.uid('rg')
        extra = ' cx="%.2f" cy="%.2f" r="%.2f"' % (r.r(.3, .7), r.r(.3, .7), r.r(.3, .7))
        if focal:
            extra += ' fx="%.2f" fy="%.2f"' % (r.r(.3, .5), r.r(.3, .5))
        if spread:
            extra += ' spreadMethod="%s"' % spread
        self.defs.append('<radialGradient id="%s"%s>' % (i, extra))
        for k in range(stops):
            self.defs.append('  <stop offset="%.2f" stop-color="%s"/>'
                             % (k / max(stops - 1, 1), r.color()))
        self.defs.append('</radialGradient>')
        return i

    def href_grad(self, base):
        i = self.uid('hg')
        self.defs.append('<linearGradient id="%s" xlink:href="#%s" '
                         'gradientTransform="rotate(%d .5 .5)"/>' % (i, base, self.rng.i(0, 180)))
        return i

    def pattern(self, kind):
        r = self.rng
        i = self.uid('pt')
        s = r.i(6, 18)
        self.defs.append('<pattern id="%s" width="%d" height="%d" patternUnits="userSpaceOnUse"%s>'
                         % (i, s, s, ' patternTransform="rotate(%d)"' % r.i(0, 90)
                            if kind != 'dots' else ''))
        if kind == 'dots':
            self.defs.append('  <circle cx="%g" cy="%g" r="%g" fill="%s"/>' % (s / 2, s / 2, s / 4, r.color()))
        elif kind == 'hatch':
            self.defs.append('  <path d="M0,0 L0,%d" stroke="%s" stroke-width="%g"/>' % (s, r.color(), s / 5))
        else:
            self.defs.append('  <rect width="%g" height="%g" fill="%s"/>' % (s / 2, s / 2, r.color()))
            self.defs.append('  <rect x="%g" y="%g" width="%g" height="%g" fill="%s"/>'
                             % (s / 2, s / 2, s / 2, s / 2, r.color()))
        self.defs.append('</pattern>')
        return i

    # -- effects ------------------------------------------------------------
    def filt(self, kind):
        r = self.rng
        i = self.uid('fx')
        d = self.defs
        d.append('<filter id="%s" x="-20%%" y="-20%%" width="140%%" height="140%%">' % i)
        if kind == 'shadow':
            d.append('  <feGaussianBlur in="SourceAlpha" stdDeviation="%.1f" result="b"/>' % r.r(1, 4))
            d.append('  <feOffset in="b" dx="%d" dy="%d" result="o"/>' % (r.i(1, 5), r.i(1, 5)))
            d.append('  <feFlood flood-color="%s" flood-opacity="0.5"/>' % r.color())
            d.append('  <feComposite in2="o" operator="in" result="s"/>')
            d.append('  <feMerge><feMergeNode in="s"/><feMergeNode in="SourceGraphic"/></feMerge>')
        elif kind == 'blur':
            d.append('  <feGaussianBlur stdDeviation="%.1f %.1f"/>' % (r.r(.5, 3), r.r(.5, 3)))
        elif kind == 'matrix':
            d.append('  <feColorMatrix type="%s" values="%s"/>' % r.pick(
                [('saturate', '%.2f' % r.r(0, 1)), ('hueRotate', '%d' % r.i(0, 360))]))
        elif kind == 'morph':
            d.append('  <feMorphology operator="%s" radius="%d"/>' % (r.pick(['erode', 'dilate']), r.i(1, 2)))
        elif kind == 'transfer':
            d.append('  <feComponentTransfer><feFuncR type="gamma" amplitude="1" exponent="%.2f"/>'
                     '<feFuncG type="linear" slope="%.2f"/><feFuncB type="table" tableValues="0 .5 1"/>'
                     '</feComponentTransfer>' % (r.r(.5, 2), r.r(.5, 1.5)))
        elif kind == 'convolve':
            d.append('  <feConvolveMatrix order="3" kernelMatrix="0 -1 0 -1 5 -1 0 -1 0"/>')
        elif kind == 'turb':
            d.append('  <feTurbulence type="fractalNoise" baseFrequency="%.3f" numOctaves="2" result="t"/>' % r.r(.01, .08))
            d.append('  <feDisplacementMap in="SourceGraphic" in2="t" scale="%d"/>' % r.i(2, 8))
        elif kind == 'light':
            d.append('  <feGaussianBlur in="SourceAlpha" stdDeviation="2" result="b"/>')
            d.append('  <feSpecularLighting in="b" surfaceScale="3" specularConstant=".8" '
                     'specularExponent="15" lighting-color="#fff" result="l">'
                     '<feDistantLight azimuth="%d" elevation="45"/></feSpecularLighting>' % r.i(0, 360))
            d.append('  <feComposite in="l" in2="SourceAlpha" operator="in" result="l2"/>')
            d.append('  <feComposite in="SourceGraphic" in2="l2" operator="arithmetic" k1="0" k2="1" k3="1" k4="0"/>')
        elif kind == 'tile':
            d.append('  <feFlood flood-color="%s" x="0" y="0" width="10" height="10" result="f"/>' % r.color())
            d.append('  <feTile in="f" result="t"/>')
            d.append('  <feBlend in="SourceGraphic" in2="t" mode="%s"/>' % r.pick(['multiply', 'screen', 'darken', 'lighten']))
        d.append('</filter>')
        return i

    def clip(self, kind):
        r = self.rng
        i = self.uid('cp')
        if kind == 'bbox':
            self.defs.append('<clipPath id="%s" clipPathUnits="objectBoundingBox">' % i)
            self.defs.append('  <circle cx=".5" cy=".5" r="%.2f"/>' % r.r(.3, .5))
        else:
            self.defs.append('<clipPath id="%s">' % i)
            self.defs.append('  <path d="M%d,%d l%d,%d l%d,%d z" clip-rule="evenodd"/>'
                             % (r.i(0, self.w), r.i(0, self.h), r.i(50, 300), r.i(-80, 80),
                                r.i(-200, 0), r.i(50, 250)))
        self.defs.append('</clipPath>')
        return i

    def mask(self, kind):
        r = self.rng
        i = self.uid('mk')
        g = self.lin(2)
        if kind == 'bbox':
            self.defs.append('<mask id="%s" maskContentUnits="objectBoundingBox">' % i)
            self.defs.append('  <rect width="1" height="1" fill="url(#%s)"/>' % g)
        else:
            self.defs.append('<mask id="%s" maskUnits="userSpaceOnUse" x="0" y="0" width="%d" height="%d">'
                             % (i, self.w, self.h))
            self.defs.append('  <circle cx="%d" cy="%d" r="%d" fill="url(#%s)"/>'
                             % (r.i(0, self.w), r.i(0, self.h), r.i(80, 300), g))
        self.defs.append('</mask>')
        return i

    def marker(self, shape):
        r = self.rng
        i = self.uid('mr')
        self.defs.append('<marker id="%s" markerWidth="8" markerHeight="8" refX="4" refY="4" '
                         'orient="auto" markerUnits="strokeWidth">' % i)
        if shape == 'arrow':
            self.defs.append('  <path d="M0,0 L8,4 L0,8 z" fill="%s"/>' % r.color())
        else:
            self.defs.append('  <circle cx="4" cy="4" r="3" fill="%s"/>' % r.color())
        self.defs.append('</marker>')
        return i

    def symbol(self):
        r = self.rng
        i = self.uid('sy')
        self.defs.append('<symbol id="%s" viewBox="0 0 24 24">' % i)
        self.defs.append('  <path d="M12,2 L%d,%d L%d,22 L%d,%d z" fill="%s"/>'
                         % (r.i(16, 22), r.i(6, 12), r.i(8, 16), r.i(2, 8), r.i(6, 12), r.color()))
        self.defs.append('  <circle cx="12" cy="12" r="%d" fill="none" stroke="%s" stroke-width="2"/>'
                         % (r.i(3, 8), r.color()))
        self.defs.append('</symbol>')
        return i

    def css_class(self):
        r = self.rng
        i = self.uid('c')
        self.css.append('.%s { fill: %s; stroke: %s; stroke-width: %.1f; }'
                        % (i, r.color(), r.color(), r.r(.5, 3)))
        return i

    # -- body ---------------------------------------------------------------
    def add(self, line, indent=1):
        self.body.append('  ' * indent + line)

    def widget(self, x, y, w, h, fill, extra='', inner=()):
        """A card-like component: its own group id with a few children."""
        i = self.uid('w')
        self.add('<g id="%s" transform="translate(%.1f,%.1f)"%s>' % (i, x, y, extra))
        self.add('<rect width="%.1f" height="%.1f" rx="%d" fill="%s"/>' % (w, h, self.rng.i(0, 12), fill), 2)
        for line in inner:
            self.add(line, 2)
        self.add('</g>')
        return i

    def render(self):
        out = ['<?xml version="1.0" encoding="UTF-8"?>',
               '<svg %s width="%d" height="%d" viewBox="0 0 %d %d">' % (NS, self.w, self.h, self.w, self.h),
               '<title>%s</title>' % self.title]
        if self.css:
            out.append('<style>')
            out += ['  ' + c for c in self.css]
            out.append('</style>')
        out.append('<defs>')
        out += ['  ' + d for d in self.defs]
        out.append('</defs>')
        out += self.body
        out.append('</svg>')
        return '\n'.join(out) + '\n'


def wave(rng, x0, y0, w, h, n):
    pts = []
    for k in range(n):
        pts.append('%.1f,%.1f' % (x0 + w * k / (n - 1), y0 + h * (.5 + .45 * math.sin(k * .35 + rng.r(0, 1)))))
    return ' '.join(pts)


CASES = {}


def case(name):
    def deco(fn):
        CASES[name] = fn
        return fn
    return deco


@case('dashboard')
def dashboard():
    d = Doc(1600, 1500, 11, 'Operations dashboard')
    r = d.rng
    sh = d.filt('shadow')
    d.add('<rect width="1600" height="1500" fill="url(#%s)"/>' % d.lin(3))
    for row in range(7):
        for col in range(6):
            x, y = 30 + col * 260, 30 + row * 160
            g = d.lin(3) if (row + col) % 2 else d.rad(3)
            cls = d.css_class()
            pts = wave(r, 12, 60, 220, 70, 24)
            d.widget(x, y, 240, 140, 'url(#%s)' % g, ' filter="url(#%s)"' % sh if row % 3 == 0 else '', (
                '<text x="12" y="24" font-size="14" font-weight="bold" fill="#123">Metric %d-%d</text>' % (row, col),
                '<text x="12" y="48" font-size="22" fill="#222">%d.%d%%</text>' % (r.i(0, 99), r.i(0, 9)),
                '<polyline class="%s" points="%s" fill="none"/>' % (cls, pts),
                '<circle cx="210" cy="28" r="10" fill="%s" opacity=".8"/>' % r.color(),
                '<path d="M12,132 h216" stroke="#888" stroke-dasharray="4 3"/>',
            ))
    for k in range(24):
        cx, cy = 90 + (k % 8) * 190, 1230 + (k // 8) * 90
        a = r.r(.2, 6.0)
        d.widget(cx - 40, cy - 40, 80, 80, 'none', '', (
            '<circle cx="40" cy="40" r="32" fill="none" stroke="#ddd" stroke-width="10"/>',
            '<path d="M40,8 A32,32 0 %d 1 %.1f,%.1f" fill="none" stroke="url(#%s)" stroke-width="10" '
            'stroke-linecap="round"/>' % (1 if a > math.pi else 0, 40 + 32 * math.sin(a),
                                          40 - 32 * math.cos(a), d.lin(2)),
            '<text x="40" y="45" text-anchor="middle" font-size="14">%d</text>' % int(a * 16),
        ))
    return d


@case('city_map')
def city_map():
    d = Doc(1800, 1400, 23, 'City map')
    r = d.rng
    pats = [d.pattern(k) for k in ('dots', 'hatch', 'checker') * 6]
    mk = [d.marker('arrow'), d.marker('dot')]
    d.add('<rect width="1800" height="1400" fill="#e8eef0"/>')
    for k in range(70):
        x, y = r.i(0, 1700), r.i(0, 1300)
        d.widget(x, y, r.i(40, 160), r.i(40, 160), 'url(#%s)' % r.pick(pats),
                 ' opacity="%.2f"' % r.r(.6, 1), (
                     '<path d="M0,0 l%d,%d" stroke="#555" stroke-width="1.5"/>' % (r.i(10, 40), r.i(10, 40)),))
    for k in range(60):
        pid = d.uid('road')
        d.add('<path id="%s" d="M%d,%d C%d,%d %d,%d %d,%d" fill="none" stroke="%s" stroke-width="%d" '
              'stroke-linecap="round" marker-end="url(#%s)"/>'
              % (pid, r.i(0, 1800), r.i(0, 1400), r.i(0, 1800), r.i(0, 1400), r.i(0, 1800),
                 r.i(0, 1400), r.i(0, 1800), r.i(0, 1400), r.color(), r.i(2, 9), r.pick(mk)))
        if k % 6 == 0:
            d.add('<text font-size="16" fill="#333"><textPath xlink:href="#%s">Street %d</textPath></text>' % (pid, k))
    for k in range(40):
        d.add('<circle cx="%d" cy="%d" r="%d" fill="url(#%s)" stroke="#fff"/>'
              % (r.i(0, 1800), r.i(0, 1400), r.i(6, 30), d.rad(2)))
    return d


@case('infographic')
def infographic():
    d = Doc(1400, 2000, 37, 'Infographic')
    r = d.rng
    fx = [d.filt(k) for k in ('shadow', 'blur', 'matrix', 'morph', 'transfer', 'convolve')]
    masks = [d.mask('bbox') for _ in range(8)] + [d.mask('user') for _ in range(4)]
    for k in range(48):
        x, y = 40 + (k % 4) * 340, 40 + (k // 4) * 160
        d.widget(x, y, 300, 140, 'url(#%s)' % d.lin(4, spread=r.pick(['pad', 'reflect', 'repeat'])),
                 ' mask="url(#%s)"' % r.pick(masks) if k % 3 == 0 else ' filter="url(#%s)"' % r.pick(fx), (
                     '<text x="150" y="70" text-anchor="middle" font-size="28" fill="#fff" '
                     'stroke="#000" stroke-width=".5">%d%%</text>' % r.i(1, 99),
                     '<path d="M20,110 A40,40 0 0 1 %d,110" fill="none" stroke="#fff" stroke-width="6"/>' % r.i(60, 280),
                 ))
    for k in range(30):
        d.add('<ellipse cx="%d" cy="%d" rx="%d" ry="%d" fill="url(#%s)" opacity=".5"/>'
              % (r.i(0, 1400), r.i(0, 2000), r.i(20, 120), r.i(20, 120), d.rad(3, focal=True)))
    return d


@case('ui_kit')
def ui_kit():
    d = Doc(1500, 1100, 41, 'UI kit')
    r = d.rng
    syms = [d.symbol() for _ in range(40)]
    clips = [d.clip('bbox') for _ in range(10)] + [d.clip('user') for _ in range(5)]
    d.add('<rect width="1500" height="1100" fill="#f7f7fb"/>')
    for k, s in enumerate(syms):
        d.add('<use xlink:href="#%s" x="%d" y="%d" width="%d" height="%d"/>'
              % (s, 30 + (k % 10) * 140, 30 + (k // 10) * 120, r.i(32, 96), r.i(32, 96)))
    for k in range(30):
        x, y = 30 + (k % 6) * 240, 520 + (k // 6) * 110
        d.widget(x, y, 220, 90, d.rng.color(), ' clip-path="url(#%s)"' % r.pick(clips), (
            '<svg x="10" y="10" width="60" height="60" viewBox="0 0 10 10">'
            '<circle cx="5" cy="5" r="6" fill="%s"/></svg>' % r.color(),
            '<text x="80" y="40" font-size="16"><tspan font-weight="bold">Button</tspan>'
            '<tspan dx="6" fill="#666">%d</tspan></text>' % k,
            '<rect x="80" y="55" width="%d" height="10" rx="5" fill="url(#%s)"/>' % (r.i(40, 120), d.lin(2)),
        ))
    return d


@case('blueprint')
def blueprint():
    d = Doc(1700, 1200, 53, 'Technical drawing')
    r = d.rng
    hatch = [d.pattern('hatch') for _ in range(12)]
    marks = [d.marker('arrow') for _ in range(10)] + [d.marker('dot') for _ in range(6)]
    d.add('<rect width="1700" height="1200" fill="#0b2a4a"/>')
    for x in range(0, 1700, 50):
        d.add('<line x1="%d" y1="0" x2="%d" y2="1200" stroke="#1d4a7a" stroke-width=".5"/>' % (x, x))
    for k in range(60):
        x, y = r.i(20, 1500), r.i(20, 1000)
        d.widget(x, y, r.i(60, 200), r.i(40, 160), 'url(#%s)' % r.pick(hatch),
                 ' stroke="#9cf"', (
                     '<polyline points="0,0 %d,%d %d,%d" fill="none" stroke="#fff" stroke-dasharray="%d %d" '
                     'stroke-linejoin="%s" marker-start="url(#%s)" marker-mid="url(#%s)" marker-end="url(#%s)"/>'
                     % (r.i(10, 80), r.i(10, 80), r.i(10, 150), r.i(0, 60), r.i(2, 9), r.i(2, 6),
                        r.pick(['miter', 'round', 'bevel']), r.pick(marks), r.pick(marks), r.pick(marks)),
                     '<text x="4" y="-4" font-size="11" fill="#cfe">R%d.%d</text>' % (r.i(1, 99), r.i(0, 9)),
                 ))
    for k in range(30):
        x, y = r.i(20, 1550), r.i(20, 1050)
        d.widget(x, y, 120, 120, 'none', ' clip-path="url(#%s)"' % d.clip('bbox'), (
            '<circle cx="60" cy="60" r="55" fill="none" stroke="#fff" stroke-width="1.5"/>',
            '<path d="M60,5 V115 M5,60 H115" stroke="#9cf" stroke-dasharray="6 2 1 2"/>',
            '<text x="64" y="20" font-size="10" fill="#fff">DETAIL %s</text>' % chr(65 + k % 26),
        ))
    return d


@case('illustration')
def illustration():
    d = Doc(1600, 1200, 67, 'Illustration')
    r = d.rng
    blur = [d.filt('blur') for _ in range(8)] + [d.filt('turb') for _ in range(3)] + [d.filt('light') for _ in range(3)]
    d.add('<rect width="1600" height="1200" fill="url(#%s)"/>' % d.rad(4, focal=True))
    for k in range(80):
        cx, cy = r.i(0, 1600), r.i(0, 1200)
        pts = ' '.join('%.1f,%.1f' % (cx + math.cos(a / 7 * math.pi * 2) * r.i(20, 90),
                                      cy + math.sin(a / 7 * math.pi * 2) * r.i(20, 90)) for a in range(7))
        g = d.rad(3, spread=r.pick([None, 'reflect'])) if k % 2 else d.href_grad(d.lin(3))
        d.add('<g id="%s" opacity="%.2f"%s>' % (d.uid('leaf'), r.r(.5, 1),
                                                 ' filter="url(#%s)"' % r.pick(blur) if k % 4 == 0 else ''))
        d.add('<polygon points="%s" fill="url(#%s)" fill-rule="%s"/>' % (pts, g, r.pick(['nonzero', 'evenodd'])), 2)
        d.add('<path d="M%d,%d q%d,%d %d,%d t%d,%d" fill="none" stroke="%s" stroke-width="2"/>'
              % (cx, cy, r.i(-40, 40), r.i(-40, 40), r.i(-80, 80), r.i(-80, 80), r.i(-60, 60),
                 r.i(-60, 60), r.color()), 2)
        d.add('</g>')
    return d


@case('calendar')
def calendar():
    d = Doc(1400, 1100, 79, 'Calendar')
    r = d.rng
    cls = [d.css_class() for _ in range(30)]
    d.css.append('g.week > rect:first-child { stroke: #333; }')
    d.css.append('#title { font-size: 40px; fill: #234; }')
    d.css.append('text[data-kind="holiday"] { fill: crimson; font-weight: bold; }')
    d.add('<text id="title" x="40" y="60">November</text>')
    for wk in range(6):
        d.add('<g class="week" transform="translate(40,%d)">' % (100 + wk * 160))
        for day in range(7):
            dd = wk * 7 + day
            d.add('<g id="%s">' % d.uid('day'), 2)
            d.add('<rect class="%s" x="%d" y="0" width="180" height="150" rx="6"/>' % (r.pick(cls), day * 190), 2)
            d.add('<text x="%d" y="28" font-size="20"%s>%d</text>'
                  % (day * 190 + 10, ' data-kind="holiday"' if dd % 9 == 0 else '', dd + 1), 2)
            for ev in range(r.i(0, 3)):
                d.add('<rect x="%d" y="%d" width="160" height="22" rx="4" fill="url(#%s)"/>'
                      % (day * 190 + 10, 40 + ev * 30, d.lin(2)), 2)
                d.add('<text x="%d" y="%d" font-size="12" fill="#fff">Event %d</text>'
                      % (day * 190 + 16, 56 + ev * 30, ev), 2)
            d.add('</g>', 2)
        d.add('</g>')
    return d


@case('circuit')
def circuit():
    d = Doc(1600, 1200, 83, 'Circuit board')
    r = d.rng
    parts = [d.symbol() for _ in range(30)]
    pads = [d.marker('dot') for _ in range(8)]
    d.add('<rect width="1600" height="1200" fill="url(#%s)"/>' % d.pattern('dots'))
    for k in range(120):
        d.add('<path d="M%d,%d H%d V%d H%d" fill="none" stroke="%s" stroke-width="3" '
              'stroke-linejoin="round" marker-start="url(#%s)" marker-end="url(#%s)"/>'
              % (r.i(0, 1600), r.i(0, 1200), r.i(0, 1600), r.i(0, 1200), r.i(0, 1600),
                 r.pick(['#c9a227', '#b87333', '#8a8a8a']), r.pick(pads), r.pick(pads)))
    for k in range(80):
        d.add('<use xlink:href="#%s" x="%d" y="%d" width="40" height="40" transform="rotate(%d %d %d)"/>'
              % (r.pick(parts), r.i(0, 1560), r.i(0, 1160), r.pick([0, 90, 180, 270]),
                 r.i(0, 1600), r.i(0, 1200)))
    for k in range(60):
        d.widget(r.i(0, 1480), r.i(0, 1120), 120, 80, 'url(#%s)' % d.lin(2), '', (
            '<rect x="-6" y="10" width="6" height="8" fill="#ccc"/>',
            '<rect x="120" y="10" width="6" height="8" fill="#ccc"/>',
            '<text x="10" y="45" font-size="14" fill="#eee" letter-spacing="2">IC%02d</text>' % k,))
    return d


@case('poster')
def poster():
    d = Doc(1200, 2400, 97, 'Poster with images')
    r = d.rng
    fx = [d.filt(k) for k in ('shadow', 'matrix', 'tile', 'transfer')] * 3
    clips = [d.clip('bbox') for _ in range(10)]
    d.add('<rect width="1200" height="2400" fill="url(#%s)"/>' % d.lin(5))
    for k in range(84):
        x, y = 30 + (k % 6) * 190, 30 + (k // 6) * 165
        par = r.pick(['none', 'xMidYMid meet', 'xMinYMax slice', 'xMaxYMin meet'])
        d.widget(x, y, 170, 150, '#fff', ' filter="url(#%s)"' % r.pick(fx) if k % 2 else
                 ' clip-path="url(#%s)"' % r.pick(clips), (
                     '<image x="10" y="10" width="150" height="100" preserveAspectRatio="%s" '
                     'xlink:href="%s"/>' % (par, PNG),
                     '<rect x="10" y="118" width="150" height="26" fill="url(#%s)"/>' % d.lin(2),
                     '<text x="85" y="135" text-anchor="middle" font-size="14" font-style="italic">Photo %d</text>' % k,
                 ))
    return d


@case('kitchen_sink')
def kitchen_sink():
    d = Doc(1800, 1800, 101, 'Everything')
    r = d.rng
    kinds = ['shadow', 'blur', 'matrix', 'morph', 'transfer', 'convolve', 'turb', 'light', 'tile']
    fx = [d.filt(k) for k in kinds]
    masks = [d.mask(r.pick(['bbox', 'user'])) for _ in range(8)]
    clips = [d.clip(r.pick(['bbox', 'user'])) for _ in range(8)]
    pats = [d.pattern(k) for k in ('dots', 'hatch', 'checker')]
    marks = [d.marker('arrow'), d.marker('dot')]
    syms = [d.symbol() for _ in range(10)]
    cls = [d.css_class() for _ in range(10)]
    d.css.append('g > circle:first-child { stroke-dasharray: 3 2; }')
    for k in range(144):
        x, y = 20 + (k % 12) * 148, 20 + (k // 12) * 148
        style = k % 6
        if style == 0:
            extra = ' filter="url(#%s)"' % r.pick(fx)
        elif style == 1:
            extra = ' mask="url(#%s)"' % r.pick(masks)
        elif style == 2:
            extra = ' clip-path="url(#%s)"' % r.pick(clips)
        elif style == 3:
            extra = ' opacity=".7"'
        else:
            extra = ''
        paint = r.pick(['url(#%s)' % d.lin(3), 'url(#%s)' % d.rad(3), 'url(#%s)' % r.pick(pats)])
        d.widget(x, y, 130, 130, paint, extra, (
            '<circle cx="65" cy="65" r="%d" class="%s"/>' % (r.i(15, 50), r.pick(cls)),
            '<use xlink:href="#%s" x="5" y="5" width="30" height="30"/>' % r.pick(syms),
            '<path d="M10,120 l40,-30 l40,20" fill="none" stroke="#000" marker-end="url(#%s)"/>' % r.pick(marks),
            '<text x="120" y="20" text-anchor="end" font-size="12">#%d</text>' % k,
        ))
    return d


def stats(text):
    lines = text.count('\n')
    ids = re.findall(r'\bid="([^"]+)"', text)
    tags = set(re.findall(r'<([a-zA-Z][\w:]*)', text))
    return lines, len(set(ids)), len(tags)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = args[0] if args else os.path.join(HERE, 'complex_cases')
    os.makedirs(out, exist_ok=True)
    bad = 0
    for name, fn in CASES.items():
        text = fn().render()
        lines, comps, tags = stats(text)
        ok = lines >= 500 and comps >= 100
        bad += not ok
        with open(os.path.join(out, name + '.svg'), 'w') as f:
            f.write(text)
        if '--stats' in sys.argv or not ok:
            print('%-14s %5d lines %4d components %3d element kinds%s'
                  % (name, lines, comps, tags, '' if ok else '  BELOW TARGET'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

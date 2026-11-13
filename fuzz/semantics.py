#!/usr/bin/env python3
"""Shared helpers for semantic (oracle-based) SVG fuzz tests.

The existing fuzzers are crash-only: they check that the process survives.
This module adds the other half: render a document, decode the PNG, and make
assertions about the *pixels*.  It is deliberately dependency-free (stdlib
only) so it can run anywhere the renderer builds.

Coordinate convention: the test SVGs use a viewBox equal to the canvas, so
user units map 1:1 to pixels and assertions can use literal coordinates.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].endswith("svg2img") else os.path.join(ROOT, "svg2img")
# NOTE: argv is consumed by the test runner; individual cases must not treat
# sys.argv[1] as their own.  BIN is resolved once at import time.

TMP = os.environ.get("SVG2IMG_SEM_WORK") or tempfile.mkdtemp(prefix="svg2img_sem_")


class RenderError(RuntimeError):
    pass


def read_png(path):
    """Decode an RGBA8 PNG.  Supports filter types 0..4 (the writer uses 0)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("bad PNG signature")
    i, idat, w, h = 8, b"", 0, 0
    while i < len(data):
        if i + 8 > len(data):
            raise ValueError("truncated chunk header")
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        if len(chunk) != ln:
            raise ValueError("truncated chunk")
        stored = struct.unpack(">I", data[i + 8 + ln:i + 12 + ln])[0]
        if stored != (zlib.crc32(typ + chunk) & 0xFFFFFFFF):
            raise ValueError("CRC mismatch in %r" % typ)
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            if (depth, ctype, comp, filt, inter) != (8, 6, 0, 0, 0):
                raise ValueError("unsupported IHDR %r" % ((depth, ctype, comp, filt, inter),))
        elif typ == b"IDAT":
            idat += chunk
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    if len(raw) != h * (stride + 1):
        raise ValueError("bad decompressed size")
    px = bytearray(w * h * 4)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 0:
            pass
        elif f == 1:
            for x in range(4, stride):
                line[x] = (line[x] + line[x - 4]) & 0xFF
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                b = prev[x]
                c = prev[x - 4] if x >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        else:
            raise ValueError("bad filter %d" % f)
        px[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(px)


def render_svg(svg, args=(), name="case", timeout=20):
    """Render *svg* and return (w, h, rgba_bytes).  Raises RenderError."""
    os.makedirs(TMP, exist_ok=True)
    src = os.path.join(TMP, name + ".svg")
    out = os.path.join(TMP, name + ".png")
    with open(src, "w") as f:
        f.write(svg)
    try:
        if os.path.exists(out):
            os.remove(out)
    except OSError:
        pass
    try:
        r = subprocess.run([BIN, *args, src, out], capture_output=True,
                           text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RenderError("timeout after %ss" % timeout)
    if r.returncode != 0:
        raise RenderError("exit %d: %s" % (r.returncode, r.stderr.strip()[:300]))
    if not os.path.exists(out):
        raise RenderError("no output file")
    return read_png(out)


def at(px, w, x, y):
    if x < 0 or y < 0 or x >= w or y >= (len(px) // (w * 4)):
        raise IndexError("pixel (%d,%d) outside %dx%d image" % (x, y, w, len(px) // (w * 4)))
    o = (y * w + x) * 4
    return (px[o], px[o + 1], px[o + 2], px[o + 3])


def alpha(px, w, x, y):
    return at(px, w, x, y)[3]


def count(px, w, h, pred):
    n = 0
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            if pred(px[o], px[o + 1], px[o + 2], px[o + 3]):
                n += 1
    return n


def count_alpha(px, w, h, lo=8, hi=255):
    return count(px, w, h, lambda r, g, b, a: lo <= a <= hi)


def bbox_alpha(px, w, h, thresh=8):
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            if px[row + x * 4 + 3] > thresh:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return None if x1 < x0 else (x0, y0, x1 + 1, y1 + 1)


def is_red(r, g, b, a=200):
    return a > 200 and r > 200 and g < 60 and b < 60


def is_green(r, g, b, a=200):
    return a > 200 and g > 200 and r < 80 and b < 80


def is_blue(r, g, b, a=200):
    return a > 200 and b > 200 and r < 80 and g < 80


def is_lime(r, g, b, a=200):
    return a > 200 and g > 200 and r < 80 and b < 80


def gradient_t(r, g, b, a=200):
    """Recover t from a red->blue sRGB gradient (r=1-t, b=t)."""
    s = r + b
    return (b / s) if s > 1 else 0.0


def mean_abs_diff(a, b, w, h):
    if len(a) != len(b):
        return 999.0
    return sum(abs(x - y) for x, y in zip(a, b)) / float(len(a))


def svg_doc(body, w=100, h=100, extra=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d"%s>%s</svg>' % (w, h, w, h, extra, body))
#!/usr/bin/env python3
"""Differential test against WebKit's SVG renderer (via qlmanage).

WebKit is an independent, mature SVG implementation, so agreeing with it on
simple, unambiguous geometry is strong evidence of correctness.  Only integer
axis-aligned shapes and interior pixels are compared: antialiasing and gamma
differ between rasterisers, so edge pixels are ignored.

Usage: python3 fuzz/refdiff.py [svg2img]
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

BIN = sys.argv[1] if len(sys.argv) > 1 else "./svg2img"
TMP = tempfile.mkdtemp(prefix="svg2img_ref_")
TOL = 40          # per-channel tolerance on interior pixels


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a png")
    i, idat, w, h, depth, ctype = 8, b"", 0, 0, 0, 0
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        i += 12 + ln
    if ctype != 6 or depth != 8:
        raise ValueError(f"unsupported png {depth}/{ctype}")
    raw = zlib.decompress(idat)
    stride = w * 4
    px = bytearray(w * h * 4)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        if f == 1:
            for x in range(4, stride): line[x] = (line[x] + line[x - 4]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                b = prev[x]; c = prev[x - 4] if x >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        px[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, px


def at_400(svg):
    """Rewrite the root tag to 400x400 with a 200x200 viewBox so both
    renderers draw at 1:1 (QuickLook thumbnails are unreliable at 200px)."""
    m = re.search(r"<svg\b[^>]*>", svg)
    if not m:
        return svg
    tag = re.sub(r'\s(width|height|viewBox)="[^"]*"', "", m.group(0))
    tag = tag[:-1] + ' width="400" height="400" viewBox="0 0 200 200">'
    return svg[:m.start()] + tag + svg[m.end():]


def render_ref(svg_text, size, name):
    src = os.path.join(TMP, name + ".svg")
    outdir = os.path.join(TMP, "ref")
    subprocess.run(["rm", "-rf", outdir])
    os.makedirs(outdir, exist_ok=True)
    open(src, "w").write(svg_text)
    subprocess.run(["qlmanage", "-t", "-s", str(size), "-o", outdir, src],
                   capture_output=True, timeout=60)
    ref = os.path.join(outdir, name + ".svg.png")
    return read_png(ref) if os.path.exists(ref) else (None, None, None)


def render_mine(svg_text, name, aa=4):
    src = os.path.join(TMP, name + ".svg")
    out = os.path.join(TMP, name + ".mine.png")
    open(src, "w").write(svg_text)
    r = subprocess.run([BIN, "-a", str(aa), src, out], capture_output=True, timeout=60)
    if r.returncode != 0:
        return None, None, None, r.returncode
    return (*read_png(out), 0)


def bbox(px, w, h):
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            if px[row + x * 4 + 3] > 8:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return (x0, y0, x1, y1) if x1 >= 0 else None


def bbox_nonwhite(px, w, h):
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            if max(abs(px[o + k] - 255) for k in range(3)) > 8:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return (x0, y0, x1, y1) if x1 >= 0 else None


def compare(name, svg, size=400):
    svg = at_400(svg)
    rw, rh, rpx = render_ref(svg, size, name)
    mw, mh, mpx, rc = render_mine(svg, name)
    if rc != 0:
        return f"{name}: renderer failed rc={rc}"
    if rw is None:
        return f"{name}: reference renderer produced nothing"
    if (rw, rh) != (mw, mh):
        return f"{name}: size mine={mw}x{mh} ref={rw}x{rh}"
    # QuickLook composites on white and is always opaque; flatten my RGBA
    # over the same background before comparing.
    mb = bbox(mpx, mw, mh)
    rb = bbox_nonwhite(rpx, rw, rh)
    if rb is None or mb is None:
        return f"{name}: empty bbox mine={mb} ref={rb}"
    if max(abs(mb[k] - rb[k]) for k in range(4)) > 1:
        return f"{name}: bbox mine={mb} ref={rb}"
    if rb is None:
        return None
    # Compare only pixels whose 5x5 neighbourhood is fully opaque in my
    # render: that excludes every antialiased boundary pixel, which is where
    # two rasterisers legitimately differ, without assuming the shape is a
    # rectangle.
    bad = 0
    total = 0
    for y in range(rb[1], rb[3] + 1):
        for x in range(rb[0], rb[2] + 1):
            amin, amax = 255, 0
            for dy in range(-2, 3):
                yy = y + dy
                if yy < 0 or yy >= mh:
                    amin = amax = 0; break
                for dx in range(-2, 3):
                    xx = x + dx
                    if xx < 0 or xx >= mw:
                        amin = amax = 0; break
                    av = mpx[(yy * mw + xx) * 4 + 3]
                    if av < amin: amin = av
                    if av > amax: amax = av
            if amax == 0 or amax - amin > 4:
                continue          # antialiased boundary or transparent
            # Exclude sharp colour edges (a shape boundary drawn over another
            # shape keeps alpha at 255, so alpha alone cannot detect it).
            cmin = [255, 255, 255]
            cmax = [0, 0, 0]
            for dy in range(-2, 3):
                yy = y + dy
                for dx in range(-2, 3):
                    xx = x + dx
                    if xx < 0 or xx >= mw or yy < 0 or yy >= mh:
                        continue
                    q = (yy * mw + xx) * 4
                    for k in range(3):
                        if mpx[q + k] < cmin[k]: cmin[k] = mpx[q + k]
                        if mpx[q + k] > cmax[k]: cmax[k] = mpx[q + k]
            if max(cmax[k] - cmin[k] for k in range(3)) > 16:
                continue
            o = (y * rw + x) * 4
            a = mpx[o + 3] / 255.0
            mine = [mpx[o + k] * a + 255.0 * (1.0 - a) for k in range(3)]
            total += 1
            if max(abs(rpx[o + k] - mine[k]) for k in range(3)) > TOL:
                bad += 1
                if bad <= 3:
                    return (f"{name}: pixel ({x},{y}) mine~="
                            f"{tuple(round(v) for v in mine)} ref={tuple(rpx[o:o+3])}")
    if total == 0:
        return None          # too thin to compare: skip
    if bad > total * 0.01:
        return f"{name}: {bad}/{total} interior pixels differ"
    return None


CASES = {
    "rect": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
            '<rect x="20" y="20" width="160" height="160" fill="#c0392b"/></svg>',
    "rect_hsl": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
                '<rect x="20" y="20" width="160" height="160" fill="hsl(210,60%,50%)"/></svg>',
    "two_rects": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
                 '<rect x="0" y="0" width="200" height="100" fill="red"/>'
                 '<rect x="0" y="100" width="200" height="100" fill="blue"/></svg>',
    "circle": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
              '<circle cx="100" cy="100" r="80" fill="green"/></svg>',
    "ellipse": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
               '<ellipse cx="100" cy="100" rx="80" ry="50" fill="purple"/></svg>',
    "polygon": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
               '<polygon points="100,20 180,180 20,180" fill="orange"/></svg>',
    "path_lines": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
                  '<path d="M20,20 H180 V180 H20 Z" fill="teal"/></svg>',
    "path_curve": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
                  '<path d="M20,180 C20,20 180,20 180,180 Z" fill="navy"/></svg>',
    "viewbox": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" '
               'viewBox="0 0 100 100"><rect x="10" y="10" width="80" height="80" '
               'fill="maroon"/></svg>',
    "group_opacity": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
                     '<g opacity="0.5"><rect x="20" y="20" width="160" height="160" '
                     'fill="black"/></g></svg>',
    "nested_groups": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
                     '<g transform="translate(20,20)"><g transform="scale(1.5)">'
                     '<rect x="0" y="0" width="60" height="60" fill="olive"/></g></g></svg>',
    "stroke_rect": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
                   '<rect x="40" y="40" width="120" height="120" fill="none" '
                   'stroke="black" stroke-width="8"/></svg>',
    "linear_gradient": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
        '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0">'
        '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
        '</linearGradient></defs>'
        '<rect x="0" y="0" width="200" height="200" fill="url(#g)"/></svg>',
    "radial_gradient": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
        '<defs><radialGradient id="g" cx="0.5" cy="0.5" r="0.5">'
        '<stop offset="0" stop-color="yellow"/><stop offset="1" stop-color="black"/>'
        '</radialGradient></defs>'
        '<rect x="0" y="0" width="200" height="200" fill="url(#g)"/></svg>',
    "clip_circle": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
        '<defs><clipPath id="c"><circle cx="100" cy="100" r="70"/></clipPath></defs>'
        '<rect x="0" y="0" width="200" height="200" fill="green" clip-path="url(#c)"/></svg>',
    "evenodd_hole": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
        '<path fill-rule="evenodd" fill="steelblue" '
        'd="M20,20 H180 V180 H20 Z M60,60 H140 V140 H60 Z"/></svg>',
    "rotate": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
        '<rect x="60" y="60" width="80" height="80" fill="crimson" '
        'transform="rotate(30 100 100)"/></svg>',
    "use_group": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
        '<defs><g id="s"><rect x="0" y="0" width="60" height="60" fill="teal"/>'
        '<circle cx="30" cy="30" r="20" fill="gold"/></g></defs>'
        '<use href="#s" x="20" y="20"/><use href="#s" x="110" y="110"/></svg>',
    "par_none": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" '
        'viewBox="0 0 100 50" preserveAspectRatio="none">'
        '<rect x="0" y="0" width="100" height="50" fill="purple"/></svg>',
    "par_slice": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" '
        'viewBox="0 0 100 50" preserveAspectRatio="xMidYMid slice">'
        '<rect x="0" y="0" width="100" height="50" fill="olive"/></svg>',
    "nested_clip": '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
        '<defs><clipPath id="a"><rect x="0" y="0" width="200" height="200"/></clipPath>'
        '<clipPath id="b"><circle cx="100" cy="100" r="60"/></clipPath></defs>'
        '<g clip-path="url(#a)"><g clip-path="url(#b)">'
        '<rect x="0" y="0" width="200" height="200" fill="navy"/></g></g></svg>',
}



def main():
    bad = 0
    for name, svg in CASES.items():
        msg = compare(name, svg)
        if msg:
            print("FAIL", msg)
            bad += 1
        else:
            print("ok  ", name)
    print(f"\n{len(CASES)} cases, {bad} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
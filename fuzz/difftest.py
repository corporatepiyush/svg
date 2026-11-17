#!/usr/bin/env python3
"""Differential / invariant tests for svg2img.

Since no reference renderer is available, these tests assert properties that
must hold for any correct renderer, plus hand-computed expectations.
"""
import os
import struct
import subprocess
import sys
import zlib

BIN = sys.argv[1] if len(sys.argv) > 1 else "./svg2img"
TMP = "/tmp/rev/out"
fails = []


def run(svg_text, args=(), name="t"):
    os.makedirs(TMP, exist_ok=True)
    src = os.path.join(TMP, name + ".svg")
    out = os.path.join(TMP, name + ".png")
    open(src, "w").write(svg_text)
    if os.path.exists(out):
        os.remove(out)
    try:
        r = subprocess.run([BIN, *args, src, out], capture_output=True,
                           text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT"
    if r.returncode != 0:
        return None, r.stderr.strip()
    return out, None


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    i, idat, w, h = 8, b"", 0, 0
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", chunk[:8])
        elif typ == b"IDAT":
            idat += chunk
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = 1 + w * 4
    bpp = 4
    px = bytearray()
    prev = bytearray(w * 4)
    for y in range(h):
        row = raw[y * stride:(y + 1) * stride]
        f = row[0]
        cur = bytearray(row[1:])
        if f == 0:
            pass
        elif f == 1:  # Sub
            for k in range(bpp, len(cur)):
                cur[k] = (cur[k] + cur[k - bpp]) & 0xFF
        elif f == 2:  # Up
            for k in range(len(cur)):
                cur[k] = (cur[k] + prev[k]) & 0xFF
        elif f == 3:  # Average
            for k in range(len(cur)):
                left = cur[k - bpp] if k >= bpp else 0
                cur[k] = (cur[k] + ((left + prev[k]) >> 1)) & 0xFF
        elif f == 4:  # Paeth
            for k in range(len(cur)):
                a = cur[k - bpp] if k >= bpp else 0
                b = prev[k]
                c = prev[k - bpp] if k >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[k] = (cur[k] + pr) & 0xFF
        else:
            raise AssertionError(f"unexpected filter {f}")
        px += cur
        prev = cur
    return w, h, bytes(px)


def px(buf, w, x, y):
    o = (y * w + x) * 4
    return tuple(buf[o:o + 4])


def ok(cond, label, detail=""):
    if cond:
        print(f"  ok   {label}")
    else:
        print(f"  FAIL {label} {detail}")
        fails.append(label)


def svg(body, w=100, h=100, vb=None):
    """vb defaults to the canvas size; pass a (x,y,w,h) tuple to override."""
    if vb is None:
        vb = f"0 0 {w} {h}"
    else:
        vb = " ".join(str(v) for v in vb)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" '
            f'height="{h}" viewBox="{vb}">{body}</svg>')


print("== A. path grammar ==")
# Unclosed subpath followed by an implicit-lineto pair must not hang.
p, err = run(svg('<path d="M0,0 L10,10 Z 5,5"/>'), name="unclosed")
ok(p is not None and err != "TIMEOUT", "unclosed subpath + trailing pair",
   "" if p else f"err={err}")

# Repeated command letters and stray separators.
p, err = run(svg('<path d="M 10,10 L 20,20 L 30,10 Z M 5,5 L 6,6 Z"/>'),
             name="repeated")
ok(p is not None, "repeated command letters", str(err))

# Implicit repeated lineto after moveto.
p, err = run(svg('<path d="M10,10 20,20 30,10 Z"/>'), name="implicit")
ok(p is not None, "implicit lineto after moveto", str(err))

# Arc flags with no separators: "a1,1 0 011 1,1"
p, err = run(svg('<path d="M10,10 a10,10 0 0110,20"/>'), name="arcflags")
ok(p is not None, "arc flags without separators", str(err))

# Negative numbers directly after flags.
p, err = run(svg('<path d="M10,10 A10 10 0 0 1-5-5"/>'), name="arcneg")
ok(p is not None, "arc with negative coords", str(err))

# Scientific notation and .5 forms.
p, err = run(svg('<path d="M1e1,1E1 L.5.5 L-.5-.5 Z"/>'), name="sci")
ok(p is not None, "scientific notation", str(err))

print("== B. degenerate geometry ==")
for label, body in [
    ("zero-size rect", '<rect x="10" y="10" width="0" height="50" fill="red"/>'),
    ("negative size rect", '<rect x="10" y="10" width="-20" height="50" fill="red"/>'),
    ("zero radius circle", '<circle cx="50" cy="50" r="0" fill="red"/>'),
    ("empty path", '<path d="" fill="red"/>'),
    ("single moveto", '<path d="M10,10" fill="red"/>'),
    ("stroke-only zero width", '<path d="M0,0 L100,100" stroke="black" stroke-width="0"/>'),
    ("polyline one point", '<polyline points="10,10" fill="none" stroke="black" stroke-width="4"/>'),
    ("zero-length dash", '<line x1="0" y1="0" x2="100" y2="0" stroke="black" stroke-width="4" stroke-dasharray="0"/>'),
    ("zero-size viewbox", None),
    ("huge coords", '<path d="M-1e9,-1e9 L1e9,1e9" stroke="black" stroke-width="5"/>'),
    ("tiny coords", '<path d="M1e-9,1e-9 L2e-9,2e-9" stroke="black" stroke-width="0.0001"/>'),
]:
    if body is None:
        continue
    p, err = run(svg(body), name="deg")
    ok(p is not None, label, str(err))

print("== C. stroke joins ==")
# A sharp V with miterlimit 1 should bevel, not spike.
for ml, label in [(1, "miterlimit=1"), (10, "miterlimit=10")]:
    body = (f'<path d="M10,80 L50,20 L90,80" fill="none" stroke="black" '
            f'stroke-width="10" stroke-miterlimit="{ml}"/>')
    p, err = run(svg(body), name="join")
    if p:
        w, h, buf = read_png(p)
        # count opaque pixels; beveled should have fewer than mitered
        n = sum(1 for i in range(3, len(buf), 4) if buf[i] > 0)
        print(f"       {label}: {n} px")
        ok(n > 0, f"stroke join renders ({label})")
    else:
        ok(False, f"stroke join ({label})", str(err))

print("== D. fill rules ==")
# Two nested squares, same winding -> nonzero fills both, evenodd makes a ring.
sq = '<path d="M10,10 H90 V90 H10 Z M30,30 H70 V70 H30 Z" fill="black" fill-rule="%s"/>'
for rule, expect_center in [("nonzero", True), ("evenodd", False)]:
    p, err = run(svg(sq % rule), name="fr_" + rule)
    if p:
        w, h, buf = read_png(p)
        c = px(buf, w, 50, 50)[3]
        got = c > 0
        ok(got == expect_center, f"fill-rule={rule} centre",
           f"alpha={c} expected={'filled' if expect_center else 'hollow'}")
    else:
        ok(False, f"fill-rule={rule}", str(err))

print("== E. opacity compositing ==")
# Two 50% red rects stacked: result alpha = 1-(0.5*0.5)=0.75
body = ('<rect x="0" y="0" width="100" height="100" fill="red" opacity="0.5"/>'
        '<rect x="0" y="0" width="100" height="100" fill="red" opacity="0.5"/>')
p, err = run(svg(body), name="op")
if p:
    w, h, buf = read_png(p)
    a = px(buf, w, 50, 50)[3]
    ok(abs(a - 191) <= 3, "stacked opacity 0.5+0.5 -> 0.75", f"alpha={a}")
else:
    ok(False, "stacked opacity", str(err))

# Group opacity must composite as a layer, not per-shape.
body = ('<g opacity="0.5">'
        '<rect x="0" y="0" width="100" height="100" fill="red"/>'
        '<rect x="0" y="0" width="100" height="100" fill="red"/>'
        '</g>')
p, err = run(svg(body), name="gop")
if p:
    w, h, buf = read_png(p)
    a = px(buf, w, 50, 50)[3]
    # As a layer: opaque red, then 50% -> alpha 128
    ok(abs(a - 128) <= 3, "group opacity composites as layer", f"alpha={a}")
else:
    ok(False, "group opacity", str(err))

print("== F. gradients ==")
# spreadMethod=pad clamps
body = ('<defs><linearGradient id="g"><stop offset="0" stop-color="red"/>'
        '<stop offset="1" stop-color="blue"/></linearGradient></defs>'
        '<rect x="0" y="0" width="100" height="100" fill="url(#g)"/>')
p, err = run(svg(body), name="pad")
if p:
    w, h, buf = read_png(p)
    l = px(buf, w, 0, 50)[:3]
    r = px(buf, w, 99, 50)[:3]
    ok(l[0] > 250 and l[2] < 5, "pad left is red", str(l))
    ok(r[2] > 250 and r[0] < 5, "pad right is blue", str(r))
else:
    ok(False, "spreadMethod pad", str(err))

# gradientTransform
body = ('<defs><linearGradient id="g" gradientTransform="translate(1,0)">'
        '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
        '</linearGradient></defs>'
        '<rect x="0" y="0" width="100" height="100" fill="url(#g)"/>')
p, err = run(svg(body), name="gt")
if p:
    w, h, buf = read_png(p)
    # with translate(1,0), the left edge maps to t=-1 -> clamped red
    ok(px(buf, w, 0, 50)[:3] == (255, 0, 0), "gradientTransform applies",
       str(px(buf, w, 0, 50)))
else:
    ok(False, "gradientTransform", str(err))

print("== G. text ==")
# Anchor middle: text must be centred on x.
p, err = run(svg('<text x="50" y="50" font-size="20" text-anchor="middle" '
                 'fill="black">AB</text>'), name="anchor")
if p:
    w, h, buf = read_png(p)
    xs = [x for x in range(w) for y in range(h) if px(buf, w, x, y)[3] > 0]
    if xs:
        mid = (min(xs) + max(xs)) / 2
        ok(abs(mid - 50) <= 3, "text-anchor=middle centres", f"mid={mid}")
    else:
        ok(False, "text-anchor=middle renders", "no pixels")
else:
    ok(False, "text-anchor=middle", str(err))

print("== H. transforms ==")
# rotate(90) about the centre should move a corner predictably.
body = ('<rect x="0" y="0" width="20" height="20" fill="black" '
        'transform="rotate(90 50 50)"/>')
p, err = run(svg(body), name="rot")
if p:
    w, h, buf = read_png(p)
    # (0,0) -> rotate 90 about (50,50): (x,y)->(50-(y-50), 50+(x-50)) = (100,0)
    # so the rect occupies roughly x 80..100, y 0..20
    n = sum(1 for x in range(75, 100) for y in range(0, 25) if px(buf, w, x, y)[3] > 0)
    ok(n > 50, "rotate(90 50 50) moves rect", f"pixels={n}")
else:
    ok(False, "rotate about point", str(err))

print("== I. viewBox / aspect ratio ==")
# viewBox 0 0 100 50 into 200x200, default xMidYMid meet -> letterboxed
body = ('<rect x="0" y="0" width="100" height="50" fill="black"/>')
p, err = run(svg(body, 200, 200, vb=(0, 0, 100, 50)), name="vb_meet")
if p:
    w, h, buf = read_png(p)
    top = px(buf, w, 100, 25)[3]     # outside content
    mid = px(buf, w, 100, 100)[3]    # inside content
    bot = px(buf, w, 100, 175)[3]    # outside content
    ok(top == 0 and mid > 0 and bot == 0, "preserveAspectRatio meet letterboxes",
       f"top={top} mid={mid} bot={bot}")
else:
    ok(False, "viewBox meet", str(err))

# preserveAspectRatio="none" stretches to fill
body = ('<rect x="0" y="0" width="100" height="50" fill="black"/>')
p, err = run(svg(body, 200, 200, vb=(0, 0, 100, 50)).replace(
    'viewBox="0 0 100 50"', 'viewBox="0 0 100 50" preserveAspectRatio="none"'),
    name="vb_none")
if p:
    w, h, buf = read_png(p)
    top = px(buf, w, 100, 5)[3]
    bot = px(buf, w, 100, 195)[3]
    ok(top > 0 and bot > 0, "preserveAspectRatio=none stretches",
       f"top={top} bot={bot}")
else:
    ok(False, "viewBox none", str(err))

print("== J. clipPath edge cases ==")
# Empty clipPath should hide everything.
body = ('<defs><clipPath id="c"></clipPath></defs>'
        '<rect x="0" y="0" width="100" height="100" fill="red" clip-path="url(#c)"/>')
p, err = run(svg(body), name="emptyclip")
if p:
    w, h, buf = read_png(p)
    n = sum(1 for i in range(3, len(buf), 4) if buf[i] > 0)
    ok(n == 0, "empty clipPath hides content", f"pixels={n}")
else:
    ok(False, "empty clipPath", str(err))

# Self-referencing clipPath must not hang.
body = ('<defs><clipPath id="c"><rect x="0" y="0" width="50" height="50" '
        'clip-path="url(#c)"/></clipPath></defs>'
        '<rect x="0" y="0" width="100" height="100" fill="red" clip-path="url(#c)"/>')
p, err = run(svg(body), name="selfclip")
ok(p is not None and err != "TIMEOUT", "self-referencing clipPath",
   str(err))

print("== K. use/cycles ==")
body = ('<defs><g id="a"><use href="#b"/></g>'
        '<g id="b"><use href="#a"/></g></defs>'
        '<use href="#a"/>')
p, err = run(svg(body), name="usecycle")
ok(p is not None and err != "TIMEOUT", "cyclic use does not hang", str(err))

print()
if fails:
    print(f"{len(fails)} FAILURE(S): {fails}")
    sys.exit(1)
print("all differential checks passed")
#!/usr/bin/env python3
"""Regression tests for svg2img.

Renders each SVG in test/ and asserts pixel values at known coordinates.
Also validates that every produced PNG is a structurally valid RGBA-8 file.

Run:  python3 test/check.py [path-to-svg2img]
"""
import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "svg2img")
TMP = "/tmp/svg2img_test"

failures = []
checks = 0


def read_png(path):
    """Decode a PNG produced by svg2img, validating its structure."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    i, idat, w, h = 8, b"", 0, 0
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        stored = struct.unpack(">I", data[i + 8 + ln:i + 12 + ln])[0]
        assert stored == (zlib.crc32(typ + chunk) & 0xFFFFFFFF), f"CRC in {typ}"
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            assert (depth, ctype, comp, filt, inter) == (8, 6, 0, 0, 0), "bad IHDR"
        elif typ == b"IDAT":
            idat += chunk
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = 1 + w * 4
    assert len(raw) == h * stride, f"raw size {len(raw)} != {h * stride}"
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
            for i in range(bpp, len(cur)):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif f == 2:  # Up
            for i in range(len(cur)):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif f == 3:  # Average
            for i in range(len(cur)):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:  # Paeth
            for i in range(len(cur)):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xFF
        else:
            raise AssertionError(f"unexpected filter {f}")
        px += cur
        prev = cur
    return w, h, bytes(px)


def render(name, args=()):
    src = os.path.join(ROOT, "test", name + ".svg")
    out = os.path.join(TMP, name + ".png")
    r = subprocess.run([BIN, *args, src, out], capture_output=True, text=True)
    assert r.returncode == 0, f"{name}: {r.stderr}"
    return out


def check(label, got, want, tol=24):
    global checks
    checks += 1
    ok = len(got) == len(want) and all(abs(a - b) <= tol for a, b in zip(got, want))
    if not ok:
        failures.append(f"{label}: got {got}, want {want}")
        print(f"  FAIL {label}: got {got}, want {want}")
    return ok


def at(px, w, x, y):
    o = (y * w + x) * 4
    return tuple(px[o:o + 4])


def main():
    os.makedirs(TMP, exist_ok=True)

    print("shapes")
    w, h, px = read_png(render("shapes"))
    check("red rect", at(px, w, 50, 40), (255, 0, 0, 255))
    check("rounded rect", at(px, w, 150, 40), (0, 255, 0, 255))
    check("circle 70%", at(px, w, 250, 40), (0, 0, 255, 179))
    check("ellipse", at(px, w, 350, 40), (255, 165, 0, 255))
    check("line", at(px, w, 80, 125), (0, 0, 0, 255))
    check("polyline", at(px, w, 200, 100), (0, 128, 0, 255))
    check("polygon", at(px, w, 330, 140), (0, 255, 255, 255))
    check("rotated", at(px, w, 45, 205), (255, 0, 255, 255))
    check("group 50%", at(px, w, 150, 200), (255, 255, 0, 128))
    check("transparent bg", at(px, w, 390, 290), (0, 0, 0, 0))

    print("gradients")
    w, h, px = read_png(render("gradients"))
    check("linear left", at(px, w, 12, 70), (255, 0, 0, 255))
    check("linear mid", at(px, w, 70, 70), (255, 255, 0, 255))
    check("linear right", at(px, w, 128, 70), (0, 0, 255, 255))
    check("radial centre", at(px, w, 220, 70), (255, 255, 255, 255))
    check("radial edge", at(px, w, 278, 70), (0, 0, 0, 255))
    check("focal radial", at(px, w, 186, 186), (255, 255, 0, 255), tol=70)

    print("paths")
    w, h, px = read_png(render("paths"))
    check("cubic", at(px, w, 80, 50), (255, 99, 71, 255))
    check("arc", at(px, w, 230, 80), (255, 215, 0, 255))
    check("evenodd hole", at(px, w, 100, 190), (0, 0, 0, 0), tol=20)
    check("evenodd ring", at(px, w, 50, 170), (70, 130, 180, 255))
    check("nonzero", at(px, w, 280, 190), (70, 130, 180, 255))

    print("text")
    w, h, px = read_png(render("text"))
    dark = sum(1 for y in range(10, 45) for x in range(15, 200)
               if at(px, w, x, y)[3] > 200 and at(px, w, x, y)[0] < 60)
    global checks
    checks += 1
    if dark <= 50:
        failures.append(f"text: only {dark} glyph pixels")
        print(f"  FAIL text: only {dark} glyph pixels")
    else:
        print(f"  ok text: {dark} glyph pixels")

    print("complex")
    w, h, px = read_png(render("complex"))
    checks += 1
    if (w, h) != (1200, 900):
        failures.append(f"complex size: got {(w, h)}")
        print(f"  FAIL complex size: got {(w, h)}")
    else:
        print(f"  ok complex size: {(w, h)}")
    # No phantom symbol instance at the origin when <use> has opacity.
    check("no origin badge", at(px, w, 10, 10), (11, 17, 39, 255), tol=8)
    # All three <use> instances land at their own x/y.
    check("badge 1", at(px, w, 860, 600), (255, 208, 102, 255), tol=24)
    check("badge 2", at(px, w, 990, 580), (206, 170, 91, 255), tol=24)
    check("badge 3", at(px, w, 1090, 570), (158, 132, 79, 255), tol=24)
    # Nested viewport background and data-URI image.
    check("nested svg", at(px, w, 830, 705), (30, 41, 59, 255), tol=8)
    check("embedded png", at(px, w, 525, 705), (231, 3, 6, 255), tol=16)
    # Group mask is applied to the flattened group: the purple dome over the
    # warm rectangle must not be double-masked at the overlap.
    check("masked group", at(px, w, 220, 620), (95, 63, 165, 255), tol=24)
    check("masked backdrop", at(px, w, 200, 500), (151, 127, 86, 255), tol=24)

    print("edge")
    w, h, px = read_png(render("edge"))
    check("clip inside", at(px, w, 60, 60), (255, 127, 80, 255))
    check("clip outside", at(px, w, 25, 25), (0, 0, 0, 0), tol=20)
    check("use", at(px, w, 160, 40), (255, 165, 0, 255))
    check("inline style", at(px, w, 300, 260), (255, 105, 180, 255))
    check("dash on", at(px, w, 24, 140), (0, 0, 0, 255))
    check("dash off", at(px, w, 32, 140), (0, 0, 0, 0), tol=20)

    print("features")
    w, h, px = read_png(render("features"))
    check("obb clip centre", at(px, w, 80, 80), (0, 128, 128, 255))
    check("obb clip corner", at(px, w, 25, 25), (0, 0, 0, 0), tol=20)
    check("clip-rule ring", at(px, w, 230, 30), (255, 165, 0, 255))
    check("clip-rule hole", at(px, w, 300, 100), (0, 0, 0, 0), tol=20)

    print("realworld")
    w, h, px = read_png(render("realworld", ("-b", "FFFFFFFF")))
    check("sky top", at(px, w, 10, 10), (77, 146, 218, 255))
    check("sky bottom", at(px, w, 10, 390), (166, 206, 239, 255))
    check("bar", at(px, w, 90, 300), (255, 136, 95, 255))
    check("circle", at(px, w, 480, 120), (246, 176, 86, 255))
    check("white card", at(px, w, 440, 270), (255, 255, 255, 255))

    print("options")
    # scale + explicit size + background
    w, h, px = read_png(render("shapes", ("-s", "2", "-b", "202020FF")))
    global_bin_ok = (w, h) == (800, 600)
    checks += 1
    if not global_bin_ok:
        failures.append(f"scale: got {(w, h)}, want (800, 600)")
        print(f"  FAIL scale: got {(w, h)}")
    else:
        print(f"  ok scale: {(w, h)}")
    check("background", at(px, w, 795, 595), (32, 32, 32, 255), tol=2)

    w, h, _ = read_png(render("shapes", ("-w", "123", "-h", "45")))
    checks += 1
    if (w, h) != (123, 45):
        failures.append(f"size: got {(w, h)}")
        print(f"  FAIL size: got {(w, h)}")
    else:
        print(f"  ok size: {(w, h)}")

    # The options scale the drawing, not just the canvas: shapes' first rect
    # (10,10)-(90,70) covers (150,120) at 2x and (25,20) at -w 200.
    for args, (x, y) in ((("-s", "2"), (150, 120)), (("-w", "200"), (25, 20))):
        w, h, px = read_png(render("shapes", args))
        check("content scaled %s" % (args,), at(px, w, x, y), (255, 0, 0, 255), tol=2)

    # One explicit dimension keeps the intrinsic aspect ratio (400x300).
    for args, want in ((("-w", "200"), (200, 150)), (("-h", "60"), (80, 60))):
        w, h, _ = read_png(render("shapes", args))
        checks += 1
        if (w, h) != want:
            failures.append(f"size {args}: got {(w, h)}, want {want}")
            print(f"  FAIL size {args}: got {(w, h)}")
        else:
            print(f"  ok size {args}: {(w, h)}")

    print()
    if failures:
        print(f"{len(failures)} failure(s) out of {checks} checks")
        return 1
    print(f"all {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
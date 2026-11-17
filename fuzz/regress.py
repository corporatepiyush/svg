#!/usr/bin/env python3
"""Regression driver for the fuzz corpus.

Every SVG in this directory is a shape or document that once crashed, produced
wrong pixels, or hit undefined behaviour.  The driver renders each one and,
where a pixel expectation is listed below, checks it.

Run:  python3 fuzz/regress.py [path-to-svg2img]
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "regress")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "svg2img")
TMP = tempfile.mkdtemp(prefix="svg2img_regress_")


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    i, idat, w, h = 8, b"", 0, 0
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        assert struct.unpack(">I", data[i + 8 + ln:i + 12 + ln])[0] == \
            (zlib.crc32(typ + chunk) & 0xFFFFFFFF), "CRC"
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            assert (depth, ctype, comp, filt, inter) == (8, 6, 0, 0, 0), "bad IHDR"
        elif typ == b"IDAT":
            idat += chunk
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    px = bytearray(w * h * 4)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
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
        px[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, px


def at(px, w, x, y):
    o = (y * w + x) * 4
    return tuple(px[o:o + 4])


def near(a, b, tol=24):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


# name -> (list of (x, y, expected rgba), tolerance)
EXPECT = {
    # The stroke ring must have an empty interior.
    "stroke_closed_ellipse.svg": [((200, 150), (0, 0, 0, 0)),
                                  ((200, 100), (128, 0, 128, 255))],
    # Both subpaths wind the same way: nonzero fills the "hole".
    "nonzero_hole.svg": [((280, 190), (70, 130, 180, 255))],
    # An empty clipPath hides everything.
    "empty_clip.svg": [((100, 100), (0, 0, 0, 0))],
    # use at depth, cloned subtree renders.
    "use_deep.svg": [((20, 20), (255, 0, 0, 255))],
    # Out-of-order gradient stops: the second offset clamps up to the first,
    # so the transition sits at 0.42 (navy before, teal after).
    "gradient_order.svg": [((20, 100), (0, 0, 128, 255)),
                           ((300, 100), (0, 128, 128, 255))],
    # spreadMethod="repeat": two periods across the rect.
    "gradient_repeat.svg": [((25, 50), (127, 0, 128, 255)),
                            ((50, 50), (255, 0, 0, 255))],
    # Radial focal point: t = |FP|/|FQ| = 0.286 at the centre.
    "gradient_focal.svg": [((100, 100), (182, 0, 73, 255))],
    # Equal offsets are a hard stop: the later stop wins at the boundary.
    "gradient_hardstop.svg": [((100, 50), (0, 0, 255, 255))],
    # Group opacity + transform must not double-apply the transform.
    "opacity_transform.svg": [((30, 30), (0, 0, 0, 128)),
                              ((130, 130), (0, 0, 0, 0))],
    # A <use> whose only child is the cloned symbol group must keep the
    # cloning group's own transform (x/y + viewBox) when the opacity layer
    # collapses the single-child group chain.  The old code dropped the last
    # transform and drew the symbol at the origin at its natural size.
    "use_opacity_symbol.svg": [((130, 50), (255, 0, 0, 128)),
                               ((100, 20), (255, 0, 0, 128)),
                               ((159, 79), (255, 0, 0, 128)),
                               ((10, 10), (0, 0, 0, 0))],
    # A mask applies to the group's flattened result, not to each child.
    # Masking the two overlapping rects separately compounded the mask and
    # turned the overlap (blue over red) purple-ish instead of blue.
    "group_mask_overlap.svg": [((100, 70), (0, 0, 128, 255)),
                               ((50, 50), (128, 0, 0, 255)),
                               ((150, 50), (0, 0, 128, 255))],
    # The left half of a symmetric cubic must be present.  A flattening
    # stack that aliases its own slot drops it, so the curve renders as a
    # right-half-only sliver; Chrome agrees with these interior points.
    "cubic_left_half.svg": [((50, 50), (220, 20, 60, 255)),
                            ((16, 50), (220, 20, 60, 255)),
                            ((30, 40), (220, 20, 60, 255)),
                            ((50, 30), (220, 20, 60, 255))],
    # A <use> child carrying an id is discarded when the use expands; the id
    # map must drop it, otherwise a later <use href="#id"> reads freed memory.
    # The reference is dropped safely (nothing renders, no crash).
    "use_child_id.svg": [((50, 50), (0, 0, 0, 0))],
    # Antialiasing: a horizontal run must only cover the pixels its samples
    # fall in.  Spreading a run over whole pixels made a 1-unit stroke at an
    # integer coordinate render two pixels wide (255/255 instead of 128/128).
    "aa_thin_stroke.svg": [((29, 50), (0, 0, 0, 128)),
                           ((30, 50), (0, 0, 0, 128)),
                           ((68, 50), (0, 0, 0, 128)),
                           ((69, 50), (0, 0, 0, 255)),
                           ((70, 50), (0, 0, 0, 255)),
                           ((71, 50), (0, 0, 0, 128))],
    # The same bug over-covered diagonal edges: the hypotenuse pixel is 6/16
    # covered at aa=4 (the true area is 1/2; 4x4 sampling quantises to 0.375).
    # --- expected-empty renders: a NaN/invalid attribute is ignored, not
    # fatal, and an unrenderable tree simply paints nothing.
    "bad_arcs.svg": [((100, 100), (0, 0, 0, 0))],
    "degenerate_paths.svg": [((10, 10), (0, 0, 0, 0))],
    "empty_svg.svg": [((100, 100), (0, 0, 0, 0))],
    "nan_transform.svg": [((30, 30), (0, 0, 0, 0)),
                          ((125, 30), (0, 0, 0, 0))],
    "scale_zero.svg": [((30, 30), (0, 0, 0, 0)),
                       ((90, 30), (0, 0, 0, 0))],
    "huge_numbers.svg": [((20, 20), (0, 0, 0, 0))],
    "deep_nesting.svg": [((200, 150), (0, 0, 0, 0))],
    # --- known renders, sampled away from antialiased edges
    "dup_ids.svg": [((70, 10), (0, 0, 255, 255)),
                    ((190, 10), (0, 0, 255, 255)),
                    ((10, 10), (0, 0, 0, 0))],
    "fill_uninit_gap.svg": [((40, 10), (0, 0, 0, 0)),
                            ((12, 10), (255, 0, 0, 255))],
    "huge_viewbox.svg": [((50, 50), (0, 128, 0, 255)),
                         ((120, 50), (0, 0, 0, 0))],
    "deep_clip.svg": [((20, 20), (0, 0, 0, 0)),
                      ((100, 100), (128, 0, 128, 255)),
                      ((190, 190), (0, 0, 0, 0))],
    "text_only.svg": [((20, 60), (0, 0, 0, 255)),
                      ((100, 70), (0, 0, 0, 255))],
    "huge_stroke.svg": [((20, 100), (0, 0, 0, 255)),
                        ((100, 100), (0, 0, 0, 255))],
    "pattern_eval_cast.svg": [((0, 0), (255, 0, 0, 255))],
    "aa_diagonal.svg": [((78, 20), (0, 0, 0, 255)),
                        ((79, 20), (0, 0, 0, 96)),
                        ((48, 50), (0, 0, 0, 255)),
                        ((49, 50), (0, 0, 0, 96))],
}


def main():
    files = sorted(f for f in os.listdir(HERE) if f.endswith(".svg"))
    bad = 0
    for name in files:
        src = os.path.join(HERE, name)
        out = os.path.join(TMP, name + ".png")
        r = subprocess.run([BIN, "-a", "4", src, out],
                           capture_output=True, timeout=120)
        if r.returncode != 0:
            print(f"FAIL {name}: exit {r.returncode} {r.stderr[:120]!r}")
            bad += 1
            continue
        if name in EXPECT:
            w, h, px = read_png(out)
            for (x, y), want in EXPECT[name]:
                got = at(px, w, x, y)
                if not near(got, want):
                    print(f"FAIL {name} at ({x},{y}): got {got}, want {want}")
                    bad += 1
        print(f"ok   {name}")
    print(f"\n{len(files)} files, {bad} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
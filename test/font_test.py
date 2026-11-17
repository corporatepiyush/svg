#!/usr/bin/env python3
"""Check real-font text rendering when a TrueType font is available.

The library renders text from the built-in 5x7 bitmap font by default.  With
`--font FILE` it parses the font's glyf outlines instead.  This test verifies
that path, and skips (exit 0) when no usable font is installed, so it is safe
to run in CI.

Usage:
    python3 test/font_test.py [path-to-svg2img] [font.ttf]

Set SVG2IMG_TEST_FONT to choose the font.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pngio  # noqa: E402

CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/Library/Fonts/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "C:/Windows/Fonts/arial.ttf",
]

SVG = """<svg xmlns="http://www.w3.org/2000/svg" width="240" height="60"
     viewBox="0 0 240 60">
  <text x="10" y="40" font-size="28" fill="#000000">Hello 42</text>
</svg>
"""


def find_font(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    env = os.environ.get("SVG2IMG_TEST_FONT")
    if env:
        return env if os.path.exists(env) else None
    for c in CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def render(binary, svg, out, font):
    cmd = [binary, "-a", "4", "-w", "240", "-h", "60"]
    if font:
        cmd += ["--font", font]
    cmd += [svg, out]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError("exit %d: %s" % (r.returncode, r.stderr.strip()[:200]))


def dark_pixels(path):
    w, h, px = pngio.read_png(path)
    pts = [(x, y) for y in range(h) for x in range(w)
           if px[(y * w + x) * 4 + 3] > 100 and px[(y * w + x) * 4] < 100]
    return pts


def check_leaks(binary, svg, out, font):
    """Run the macOS leaks tool around a font render; None when unavailable."""
    if not shutil.which("leaks"):
        return None
    cmd = ["leaks", "--atExit", "--", binary, "-a", "2", "--font", font, svg, out]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return False
    text = r.stdout + r.stderr
    if "not supported" in text or "could not" in text:
        return None
    m = re.search(r"total leaked bytes:\s*(\d+)", text)
    if not m:
        return None
    return int(m.group(1)) == 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    binary = argv[0] if argv else os.path.join(ROOT, "svg2img")
    font = find_font(argv[1] if len(argv) > 1 else None)
    if not os.path.exists(binary):
        print("skip: no binary at", binary)
        return 0
    if not font:
        print("skip: no TrueType font found (set SVG2IMG_TEST_FONT)")
        return 0

    tmp = tempfile.mkdtemp(prefix="svg2img_font_")
    svg = os.path.join(tmp, "t.svg")
    with open(svg, "w") as f:
        f.write(SVG)

    bitmap_png = os.path.join(tmp, "bitmap.png")
    font_png = os.path.join(tmp, "font.png")
    render(binary, svg, bitmap_png, None)
    render(binary, svg, font_png, font)

    b = dark_pixels(bitmap_png)
    f = dark_pixels(font_png)
    if not b:
        print("FAIL bitmap text drew nothing")
        return 1
    if not f:
        print("FAIL font text drew nothing")
        return 1

    bb = (min(p[0] for p in b), max(p[0] for p in b),
          min(p[1] for p in b), max(p[1] for p in b))
    fb = (min(p[0] for p in f), max(p[0] for p in f),
          min(p[1] for p in f), max(p[1] for p in f))

    ok = True
    # The outline text must differ from the bitmap text: same ink is a sign
    # the font was ignored.
    if bb == fb:
        print("FAIL outline text matches the bitmap font (font ignored?)")
        ok = False
    # The outline run should still be a single line of text of plausible width.
    width = fb[1] - fb[0]
    if fb[2] < 0 or fb[3] > 60:
        print("FAIL outline text is drawn outside the canvas: %s" % (fb,))
        ok = False
    if not (80 <= width <= 230):
        print("FAIL font run width %d outside 80..230" % width)
        ok = False
    if fb[3] - fb[2] < 10:
        print("FAIL font run is not tall enough (%d px)" % (fb[3] - fb[2]))
        ok = False
    # A real face fills far more pixels than the 5x7 font at the same size.
    if len(f) < 300:
        print("FAIL outline text has too little ink (%d)" % len(f))
        ok = False

    leaks = check_leaks(binary, svg, font_png, font)
    if leaks is False:
        print("FAIL font render leaks memory")
        ok = False
    elif leaks is None:
        print("leaks: unavailable (skipped)")

    print("font %s" % os.path.basename(font))
    print("  bitmap bbox %s ink %d" % (bb, len(b)))
    print("  outline bbox %s ink %d" % (fb, len(f)))
    print("ok" if ok else "failed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
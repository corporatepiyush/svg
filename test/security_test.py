#!/usr/bin/env python3
"""Regression tests for <image> loading hardening.

  * external files stay inside the base directory (absolute paths, "..",
    URL schemes and symlinks pointing outside are refused)
  * a FIFO href cannot hang the renderer
  * nested data:image/svg+xml documents cannot fan out exponentially
  * one embedded image used many times is decoded once

Usage: python3 test/security_test.py ./svg2img
"""
import base64
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import pngio  # noqa: E402

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./svg2img")
SVGNS = 'xmlns="http://www.w3.org/2000/svg"'
failures = 0


def png_bytes(w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def render(svg_path, out, timeout=20):
    t0 = time.time()
    try:
        subprocess.run([BIN, svg_path, out], timeout=timeout,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return None
    return time.time() - t0


def pixel(path, x, y):
    w, _h, px = pngio.read_png(path)
    i = (y * w + x) * 4
    return tuple(px[i:i + 4])


def check(name, ok, detail=""):
    global failures
    print(("ok   " if ok else "FAIL ") + name + (("  " + detail) if detail else ""))
    if not ok:
        failures += 1


with tempfile.TemporaryDirectory() as tmp:
    doc = os.path.join(tmp, "doc")
    secret = os.path.join(tmp, "secret")
    os.makedirs(doc)
    os.makedirs(secret)
    red = png_bytes(4, 4, (255, 0, 0, 255))
    with open(os.path.join(secret, "s.png"), "wb") as f:
        f.write(red)
    with open(os.path.join(doc, "inside.png"), "wb") as f:
        f.write(red)
    os.symlink(os.path.join(secret, "s.png"), os.path.join(doc, "link.png"))
    out = os.path.join(tmp, "out.png")

    # Each href is drawn into its own 10x10 cell.
    cases = [
        ("inside", "inside.png", True),
        ("inside_dot", "./inside.png", True),
        ("absolute", os.path.join(secret, "s.png"), False),
        ("dotdot", "../secret/s.png", False),
        ("file_scheme", "file://" + os.path.join(secret, "s.png"), False),
        ("symlink_out", "link.png", False),
    ]
    body = "".join('<image x="%d" width="10" height="10" href="%s"/>' % (i * 10, h)
                   for i, (_, h, _) in enumerate(cases))
    svg = os.path.join(doc, "files.svg")
    with open(svg, "w") as f:
        f.write('<svg %s width="%d" height="10">%s</svg>' % (SVGNS, 10 * len(cases), body))
    render(svg, out)
    for i, (name, _h, want) in enumerate(cases):
        got = pixel(out, i * 10 + 5, 5)[3] > 0
        check("file_" + name, got == want, "loaded" if got else "refused")

    if hasattr(os, "mkfifo"):
        fifo = os.path.join(doc, "pipe.png")
        os.mkfifo(fifo)
        svg = os.path.join(doc, "fifo.svg")
        with open(svg, "w") as f:
            f.write('<svg %s width="4" height="4"><image width="4" height="4" href="pipe.png"/></svg>' % SVGNS)
        check("fifo_no_hang", render(svg, out, timeout=5) is not None)

    # Six levels of three nested 4000x4000 SVG images: 3^6 renders unbounded.
    s = ('<svg %s width="4000" height="4000"><rect width="10" height="10"/></svg>' % SVGNS).encode()
    for _ in range(6):
        img = "".join('<image width="10" height="10" href="data:image/svg+xml;base64,%s"/>'
                      % base64.b64encode(s).decode() for _ in range(3))
        s = ('<svg %s width="4000" height="4000">%s</svg>' % (SVGNS, img)).encode()
    svg = os.path.join(tmp, "nest.svg")
    with open(svg, "wb") as f:
        f.write(s)
    dt = render(svg, out, timeout=20)
    check("nested_svg_bounded", dt is not None and dt < 5, "%.2fs" % dt if dt else "timeout")

    # Nested SVG images still render.
    inner = '<svg %s width="10" height="10"><rect width="10" height="10" fill="#00f"/></svg>' % SVGNS
    svg = os.path.join(tmp, "nested_ok.svg")
    with open(svg, "w") as f:
        f.write('<svg %s width="10" height="10"><image width="10" height="10" '
                'href="data:image/svg+xml;base64,%s"/></svg>'
                % (SVGNS, base64.b64encode(inner.encode()).decode()))
    render(svg, out)
    check("nested_svg_renders", pixel(out, 5, 5) == (0, 0, 255, 255), str(pixel(out, 5, 5)))

    # One 1024x1024 embedded PNG used 2000 times.
    big = base64.b64encode(png_bytes(1024, 1024, (200, 50, 50, 255))).decode()
    uses = "".join('<use href="#i" x="%d"/>' % (i % 10) for i in range(2000))
    svg = os.path.join(tmp, "use.svg")
    with open(svg, "w") as f:
        f.write('<svg %s width="200" height="200"><defs><image id="i" width="20" height="20" '
                'href="data:image/png;base64,%s"/></defs>%s</svg>' % (SVGNS, big, uses))
    dt = render(svg, out, timeout=20)
    check("use_image_decoded_once", dt is not None and dt < 3, "%.2fs" % dt if dt else "timeout")
    check("use_image_renders", pixel(out, 5, 5)[3] == 255)

    # Decoded images share the max_pixels budget: two 100x100 images fit in
    # 20000 pixels, three do not, and the overrun is reported as a limit.
    small = base64.b64encode(png_bytes(100, 100, (0, 200, 0, 255))).decode()
    env = dict(os.environ, SVG2IMG_LIMIT_MAX_PIXELS="20000")
    for count, want in ((2, 0), (3, 1)):
        imgs = "".join('<image width="10" height="10" href="data:image/png;base64,%s"/>' % small
                       for _ in range(count))
        svg = os.path.join(tmp, "budget%d.svg" % count)
        with open(svg, "w") as f:
            f.write('<svg %s width="10" height="10">%s</svg>' % (SVGNS, imgs))
        rc = subprocess.run([BIN, svg, out], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL).returncode
        check("image_budget_%d" % count, (rc != 0) == bool(want), "exit=%d" % rc)

print("\n%d failure(s)" % failures)
sys.exit(1 if failures else 0)

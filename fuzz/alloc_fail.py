#!/usr/bin/env python3
"""Allocation-failure injection harness for svg2img.

A renderer that leaks or crashes when malloc returns NULL is a security bug:
a hostile document that makes the process allocate a lot, or a host app under
memory pressure, must degrade gracefully rather than corrupt memory.

This harness links the renderer with -DSVG2IMG_ALLOC_HOOKS (so svg_malloc /
svg_realloc route through a test allocator) and a small driver that fails the
N-th allocation for N = 1..K, rendering a corpus of SVGs each time.  For every
run it checks:

  * the process does not crash (no signal, no ASan/UBSan report),
  * the exit status is 0 or a clean non-zero code (never a signal),
  * no bytes are leaked (the driver frees everything itself; ASan/LSan checks
    this too when built with -fsanitize=address).

Build:  make -C fuzz alloc
Run:    python3 fuzz/alloc_fail.py [path-to-svg2img] [--max-failures N]

Exit: 0 if every injected failure was handled, 1 otherwise.
"""
import base64
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].endswith("alloc_run") else \
    os.path.join(ROOT, "fuzz", "alloc_run")
MAX_FAIL = 400
if "--max-failures" in sys.argv:
    MAX_FAIL = int(sys.argv[sys.argv.index("--max-failures") + 1])
TMP = tempfile.mkdtemp(prefix="svg2img_alloc_")


def svg(body, w=60, h=60, extra=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d"%s>%s</svg>' % (w, h, w, h, extra, body))


TINY_PNG = base64.b64encode(
    b"\x89PNG\r\n\x1a\n" +
    (lambda c: c(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)) +
     c(b"IDAT", zlib.compress(b"\0" + b"\xff\0\0\xff" * 2 + b"\0" + b"\0\0\xff\xff" * 2)) +
     c(b"IEND", b""))(
        lambda t, d: struct.pack(">I", len(d)) + t + d +
        struct.pack(">I", zlib.crc32(t + d)))).decode()
NESTED_SVG = base64.b64encode(
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20">'
    b'<rect width="10" height="10" fill="red"/>'
    b'<image width="20" height="20" href="data:image/png;base64,' +
    TINY_PNG.encode() + b'"/></svg>').decode()

CASES = [
    ("simple_rect", svg('<rect x="0" y="0" width="60" height="60" fill="red"/>')),
    ("shapes", svg('<circle cx="30" cy="30" r="20" fill="blue"/>'
                   '<ellipse cx="10" cy="10" rx="8" ry="5" fill="lime"/>'
                   '<line x1="0" y1="0" x2="60" y2="60" stroke="black"/>')),
    ("path_curves", svg('<path d="M0,0 C10,10 20,0 30,10 S50,20 60,0 Z" '
                        'fill="green" stroke="black" stroke-width="2"/>')),
    ("path_arc", svg('<path d="M5,30 A25,25 0 1 1 55,30 Z" fill="purple"/>')),
    ("gradients", svg('<defs>'
                      '<linearGradient id="lg"><stop offset="0" stop-color="red"/>'
                      '<stop offset="1" stop-color="blue"/></linearGradient>'
                      '<radialGradient id="rg"><stop offset="0" stop-color="white"/>'
                      '<stop offset="1" stop-color="black"/></radialGradient>'
                      '</defs>'
                      '<rect width="60" height="30" fill="url(#lg)"/>'
                      '<rect y="30" width="60" height="30" fill="url(#rg)"/>')),
    ("use", svg('<defs><rect id="r" width="20" height="20" fill="red"/>'
                '<g id="g"><use href="#r"/><rect x="20" width="20" height="20" '
                'fill="blue"/></g></defs>'
                '<use href="#g" x="10" y="10"/>')),
    ("clip", svg('<defs><clipPath id="c"><circle cx="30" cy="30" r="25"/>'
                 '</clipPath></defs>'
                 '<rect width="60" height="60" fill="red" clip-path="url(#c)"/>')),
    ("opacity_layers", svg('<g opacity="0.5"><g opacity="0.5">'
                           '<rect x="10" y="10" width="40" height="40" fill="red"/>'
                           '</g></g>')),
    ("text", svg('<text x="5" y="40" font-size="24" fill="black">Hello</text>')),
    ("nested_svg", svg('<svg x="10" y="10" width="40" height="40" '
                       'viewBox="0 0 20 20"><rect width="20" height="20" '
                       'fill="red"/></svg>')),
    ("dash", svg('<path d="M0,30 L60,30" stroke="black" stroke-width="4" '
                 'stroke-dasharray="5 3"/>')),
    ("deep_groups", svg("<g>" * 40 + '<rect width="60" height="60" fill="red"/>' +
                        "</g>" * 40)),
    ("many_shapes", svg("".join('<rect x="%d" y="0" width="5" height="60" '
                                'fill="hsl(%d,80%% ,50%%)"/>' % (i * 5 % 60, i * 7 % 360)
                                for i in range(40)))),
    # Image pixels are reference counted and shared with <use> clones and
    # feImage primitives; embedded SVG images render at build time.
    ("image_png_use", svg('<defs><image id="i" width="20" height="20" '
                          'href="data:image/png;base64,' + TINY_PNG + '"/></defs>'
                          '<use href="#i"/><use href="#i" x="20"/>')),
    ("image_svg_nested", svg('<image width="40" height="40" '
                             'href="data:image/svg+xml;base64,' + NESTED_SVG + '"/>')),
    ("fe_image", svg('<defs><filter id="f"><feImage href="data:image/png;base64,' +
                     TINY_PNG + '"/></filter></defs>'
                     '<g filter="url(#f)"><rect width="60" height="60"/></g>'
                     '<use href="#r"/><rect id="r" filter="url(#f)" width="5" height="5"/>')),
    # clipPath with its own clip-path and a clipped child (separate canvases),
    # primitive subregions, image preserveAspectRatio, paint fallback and the
    # marker/font shorthands.
    ("spec_fixes", svg('<style>.m{marker:url(#mk)} text{font:bold 20px serif}</style>'
                       '<marker id="mk" markerWidth="4" markerHeight="4"><rect width="4" height="4"/></marker>'
                       '<clipPath id="i"><rect width="30" height="60"/></clipPath>'
                       '<clipPath id="j"><rect x="20" width="40" height="60" clip-path="url(#i)"/></clipPath>'
                       '<clipPath id="o" clip-path="url(#i)"><circle cx="30" cy="30" r="25"/></clipPath>'
                       '<filter id="f"><feFlood flood-color="red" x="10" width="20"/>'
                       '<feConvolveMatrix order="3" kernelMatrix="0 0 0 0 1 0 0 0 0" preserveAlpha="true"/></filter>'
                       '<rect width="60" height="60" fill="url(#none) blue" clip-path="url(#o)"/>'
                       '<rect width="60" height="60" fill="green" clip-path="url(#j)" filter="url(#f)"/>'
                       '<image width="60" height="30" preserveAspectRatio="xMaxYMax slice" '
                       'href="data:image/png;base64,' + TINY_PNG + '"/>'
                       '<path class="m" d="M5,5 L30,30 L55,5" fill="none"/><text y="50">Hi</text>')),
    # Enough rules to build the CSS rule index (id/tag/class buckets plus
    # attribute selectors with pre-parsed values).
    ("css_index", svg('<style>' + ''.join(
        '.c%d{fill:#%02x0000} #i%d{stroke:blue} rect[data-k="%d"]{opacity:.5} '
        'g > .c%d:first-child{stroke-width:2} ' % (k, k * 7 % 256, k, k, k)
        for k in range(40)) + '[fill=red]{stroke:green} *{stroke-opacity:.9}</style>'
        + ''.join('<g><rect id="i%d" class="c%d x" data-k="%d" width="5" height="5" '
                  'x="%d"/></g>' % (k, k, k, k) for k in range(0, 40, 3)))),
    # Overflow every stack buffer onto the heap: >16 attributes on one
    # element, >256 flattened points and >64 raw segments in one path, a
    # >256-point polyline, dashed strokes that outgrow their inline subpath
    # array, and a transformed path too big for the scratch copy.
    ("spill_buffers", svg('<rect width="9" height="9" ' + ' '.join(
        'data-a%d="v%d &amp; x"' % (k, k) for k in range(24)) + '/>'
        + '<polyline fill="none" stroke="red" stroke-dasharray="3 1" points="'
        + ' '.join('%d,%d' % (k % 97, (k * 7) % 89) for k in range(600)) + '"/>'
        + '<path transform="rotate(5)" stroke="blue" d="M5,5 ' + ' '.join(
            'C%d,%d %d,%d %d,%d' % (k % 90, k % 70, (k * 3) % 90, (k * 5) % 70,
                                     (k * 7) % 90, (k * 11) % 70) for k in range(90))
        + ' Z"/>')),
    ("bad_xml", "<svg><rect></svg>"),
    ("empty", ""),
]


def build():
    libdir = os.path.join(ROOT, "libs")
    srcs = sorted(
        os.path.join(libdir, name)
        for name in os.listdir(libdir)
        if name.endswith(".c") and name not in ("cli.c", "main.c")
    )
    cmd = [
        "cc", "-std=c17", "-g", "-O1", "-DSVG2IMG_ALLOC_HOOKS",
        "-I" + libdir,
        "-o", os.path.join(ROOT, "fuzz", "alloc_run"),
        os.path.join(ROOT, "fuzz", "alloc_run.c"),
    ] + srcs + ["-lm", "-lpthread"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("build failed:\n" + r.stderr)
        return False
    return True


def main():
    if not os.path.exists(BIN):
        if not build():
            return 1
    failures = []
    runs = 0
    for name, text in CASES:
        src = os.path.join(TMP, name + ".svg")
        out = os.path.join(TMP, name + ".png")
        with open(src, "w") as f:
            f.write(text)
        for n in range(0, MAX_FAIL + 1):
            if os.path.exists(out):
                os.remove(out)
            r = subprocess.run([BIN, str(n), src, out],
                               capture_output=True, timeout=60)
            runs += 1
            if r.returncode < 0:
                failures.append((name, n, "killed by signal %d" % -r.returncode))
                print("FAIL  %-16s alloc#%d: signal %d" % (name, n, -r.returncode))
                if len(failures) > 20:
                    break
                continue
            if r.returncode > 2:
                failures.append((name, n, "exit %d: %s" %
                                 (r.returncode, r.stderr.decode("utf-8", "replace")[:120])))
                print("FAIL  %-16s alloc#%d: exit %d" % (name, n, r.returncode))
                if len(failures) > 20:
                    break
        print("ok    %-16s (%d allocations injected)" % (name, MAX_FAIL + 1))
    print("\n%d runs, %d failure(s)" % (runs, len(failures)))
    for name, n, why in failures:
        print("  %s alloc#%d: %s" % (name, n, why))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
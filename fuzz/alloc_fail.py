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
import os
import subprocess
import sys
import tempfile

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
    ("bad_xml", "<svg><rect></svg>"),
    ("empty", ""),
]


def build():
    src = [
        "xml_parse.c", "xml_dom.c", "path_parse.c", "path_util.c", "color.c",
        "length.c", "svg_idmap.c", "svg_transform.c", "svg_geometry.c",
        "svg_gradient.c", "svg_text.c", "svg_style.c", "svg_build.c",
        "svg_refs.c", "svg_viewport.c", "svg_api.c", "svg_alloc.c",
        "raster_util.c", "raster_canvas.c", "raster_blend.c", "raster_mask.c",
        "raster_gradient.c", "raster_fill.c", "raster_stroke.c",
        "raster_shape.c", "raster_clip.c", "raster_text.c", "raster_node.c",
        "png_deflate.c", "png_write.c", "font.c",
    ]
    srcs = " ".join(os.path.join(ROOT, s) for s in src)
    cmd = ("cc -std=c17 -g -O1 -DSVG2IMG_ALLOC_HOOKS -I%s -o %s "
           "%s %s -lm" % (ROOT, os.path.join(ROOT, "fuzz", "alloc_run"),
                          os.path.join(ROOT, "fuzz", "alloc_run.c"), srcs))
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
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
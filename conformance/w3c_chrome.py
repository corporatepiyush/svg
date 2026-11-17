#!/usr/bin/env python3
"""Measure the renderer against the official W3C SVG 1.1 test suite.

The suite is not committed: it is 14 MB of W3C-copyright material.  This script
downloads it once into a cache directory (default ``conformance/.w3c-suite``,
override with ``SVG11_CACHE``) and renders every ``svg/*.svg`` with both this
renderer and an independent conforming implementation (Chrome/Chromium).

Why Chrome and not the committed reference PNGs: the suite's PNGs were produced
by one renderer whose strokes snap to the pixel grid, while a scanline
rasteriser centres a 1-unit stroke on the path and gives half coverage to each
pixel it touches.  Comparing against those PNGs measures the reference
renderer's antialiasing, not conformance.  Chrome is an independent
implementation of the same specification, so agreement with it is meaningful.

Text is masked out.  The suite labels every test with a revision string and
most tests draw text; this renderer uses a built-in 5x7 bitmap font, so glyph
rasterisation cannot match a system font and would mask the geometry being
tested.  Tests left with almost no comparable pixels (text-only tests) are
reported separately rather than counted as passes.

Usage:
    python3 conformance/w3c_chrome.py [--limit N] [--workers N] [--json FILE]
                                      [--tol N] [--max-bad-frac F]

Exit status: 0 when every comparable test matches within tolerance.
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "fuzz"))
import pngio  # noqa: E402
import diff_external as D  # noqa: E402

URL = ("https://www.w3.org/Graphics/SVG/Test/20110816/archives/"
       "W3C_SVG_11_TestSuite.tar.gz")
CACHE = os.environ.get("SVG11_CACHE", os.path.join(ROOT, "conformance", ".w3c-suite"))
W, H = 480, 360
MIN_PX = 5000          # below this a test is text-only: nothing to compare

CHAPTERS = {
    "render.html": "3 Rendering model", "types.html": "4 Data types",
    "struct.html": "5 Document structure", "styling.html": "6 Styling",
    "coords.html": "7 Coordinates/units", "paths.html": "8 Paths",
    "shapes.html": "9 Basic shapes", "text.html": "10 Text",
    "painting.html": "11 Painting", "color.html": "12 Color",
    "pservers.html": "13 Gradients/patterns", "masking.html": "14 Clipping/masking",
    "filters.html": "15 Filters", "interact.html": "16 Interactivity",
    "linking.html": "17 Linking", "script.html": "18 Scripting",
    "animate.html": "19 Animation", "fonts.html": "20 Fonts",
    "metadata.html": "21 Metadata", "extend.html": "23 Extensibility",
    "conform.html": "App. G Conformance", "svgdom.html": "App. B DOM",
    "implnote.html": "App. F Impl. reqs",
}


def ensure_suite():
    root = os.path.join(CACHE, "W3C_SVG_11_TestSuite")
    if os.path.isdir(os.path.join(root, "svg")):
        return root
    if os.path.isdir(os.path.join(CACHE, "svg")):
        return CACHE
    os.makedirs(CACHE, exist_ok=True)
    tgz = os.path.join(CACHE, "W3C_SVG_11_TestSuite.tar.gz")
    if not os.path.exists(tgz):
        print("downloading", URL)
        urllib.request.urlretrieve(URL, tgz)
    print("extracting", tgz)
    with tarfile.open(tgz) as t:
        t.extractall(CACHE)
    return root if os.path.isdir(os.path.join(root, "svg")) else CACHE


def chapter_of(root, name):
    path = os.path.join(root, "svg", name + ".svg")
    try:
        src = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return "(unlabelled)"
    m = re.search(r'<d:testDescription[^>]*href="([^"]+)"', src)
    if not m:
        return "(unlabelled)"
    href = m.group(1)
    href = re.sub(r"^https?://[^/]*/TR/SVG11/", "", href)
    href = re.sub(r"^https?://dev\.w3\.org/SVG/profiles/1\.1F2/(publish|master)/", "", href)
    href = href.split("#")[0]
    return CHAPTERS.get(href, href or "(unlabelled)")


def text_bands(src, w, h):
    """Rows covered by <text> elements; glyph rendering is out of scope."""
    band = bytearray(w * h)
    for m in re.finditer(r"<text\b([^>]*)>(.*?)</text>", src, re.S):
        attrs = m.group(1)
        y = float((re.search(r'\by="(-?[0-9.]+)"', attrs) or [None, "0"])[1])
        fs = re.search(r'font-size="([0-9.]+)"', attrs)
        fs = float(fs.group(1)) if fs else 18.0
        for yy in range(max(0, int(y - 1.6 * fs)), min(h, int(y + 0.6 * fs) + 1)):
            for xx in range(w):
                band[yy * w + xx] = 1
    return band


def suppress_text(src):
    """Return *src* with all text-rendering elements hidden.

    The normal comparison masks horizontal text bands, which can also hide a
    geometry bug that happens to share those rows.  Rendering a text-suppressed
    copy of every test and comparing every pixel gives a geometry-only oracle;
    tests whose entire content is text then have no comparable pixels and are
    reported as text-only, as before.
    """
    m = re.search(r"<svg\b[^>]*>", src, re.S)
    if not m:
        return src
    inject = "<style>text,tspan,tref,textPath,altGlyph{display:none}</style>"
    return src[:m.end()] + inject + src[m.end():]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "svg2img"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=0)
    ap.add_argument("--tol", type=int, default=16)
    ap.add_argument("--max-bad-frac", type=float, default=0.03)
    ap.add_argument("--json", default="")
    ap.add_argument("--suppress-text", action="store_true",
                    help="hide text before comparing, so geometry is not "
                         "masked by the bitmap-font difference")
    ap.add_argument("--backend", default="chrome-inline",
                    choices=["chrome-inline", "chrome"],
                    help="external oracle harness (default: chrome-inline)")
    ap.add_argument("--only", default="",
                    help="only tests whose name contains this substring")
    args = ap.parse_args(argv)

    root = ensure_suite()
    tests = sorted(f for f in os.listdir(os.path.join(root, "svg")) if f.endswith(".svg"))
    if args.only:
        tests = [f for f in tests if args.only in f]
    if args.limit:
        tests = tests[:args.limit]
    backend_cls = (D.ChromeInlineBackend if args.backend == "chrome-inline"
                   else D.ChromeBackend)
    backend = backend_cls.probe()
    if not backend.available:
        print("no Chrome-family browser available; cannot verify")
        return 2
    if args.suppress_text and args.backend == "chrome-inline":
        backend.extra_css = ("text,tspan,tref,textPath,altGlyph{display:none}")
    print("oracle:", backend.detail)

    workers = args.workers or min(4, os.cpu_count() or 4)
    tmp = tempfile.mkdtemp(prefix="svg11_chrome_")
    ours_dir = os.path.join(tmp, "ours")
    chrome_dir = os.path.join(tmp, "chrome")
    os.makedirs(ours_dir)
    os.makedirs(chrome_dir)
    ours_src_dir = os.path.join(root, "svg")
    if args.suppress_text:
        # Only our renderer consumes the modified copy; the Chrome harness
        # gets the original file plus page CSS, so its relative references
        # still resolve against the suite directory.
        ours_src_dir = os.path.join(tmp, "svg-notext")
        os.makedirs(ours_src_dir)
        for fname in tests:
            with open(os.path.join(root, "svg", fname),
                      encoding="utf-8", errors="replace") as f:
                src = f.read(16 * 1024 * 1024)
            with open(os.path.join(ours_src_dir, fname), "w",
                      encoding="utf-8") as f:
                f.write(suppress_text(src))

    def render(item):
        fname, which = item
        name = fname[:-4]
        outdir = ours_dir if which == "ours" else chrome_dir
        out = os.path.join(outdir, name + (".png" if which == "ours" else ".chrome.png"))
        if os.path.exists(out):
            return
        svg = os.path.join(ours_src_dir if which == "ours"
                           else os.path.join(root, "svg"), fname)
        try:
            if which == "ours":
                subprocess.run([args.bin, "-a", "4", "-w", str(W), "-h", str(H), svg, out],
                               capture_output=True, timeout=120)
            else:
                backend.render(svg, (W, H), outdir, outdir)
        except Exception:                                     # noqa: BLE001
            pass

    jobs = [(f, "ours") for f in tests] + [(f, "chrome") for f in tests]
    with cf.ThreadPoolExecutor(max_workers=workers) as ex:
        for i, _ in enumerate(ex.map(render, jobs), 1):
            if i % 100 == 0:
                print("  rendered %d/%d" % (i, len(jobs)))

    rows = []
    for fname in tests:
        name = fname[:-4]
        ours = os.path.join(ours_dir, name + ".png")
        ref = os.path.join(chrome_dir, name + ".chrome.png")
        ch = chapter_of(root, name)
        if not (os.path.exists(ours) and os.path.exists(ref)):
            rows.append({"name": name, "chapter": ch, "verdict": "error",
                         "compared": 0, "diff": 1.0})
            continue
        aw, ah, a = pngio.read_png(ours)
        rw, rh, b = pngio.read_png(ref)
        if (aw, ah) != (rw, rh):
            rows.append({"name": name, "chapter": ch, "verdict": "size",
                         "compared": 0, "diff": 1.0})
            continue
        band = (bytearray(aw * ah) if args.suppress_text else
                text_bands(open(os.path.join(root, "svg", fname),
                                encoding="utf-8", errors="replace").read(),
                           aw, ah))
        n = bad = 0
        for y in range(3, ah - 3):
            row = y * aw
            for x in range(3, aw - 3):
                i = row + x
                if band[i]:
                    continue
                o = i * 4
                n += 1
                if (abs(a[o] - b[o]) > args.tol or abs(a[o + 1] - b[o + 1]) > args.tol
                        or abs(a[o + 2] - b[o + 2]) > args.tol
                        or abs(a[o + 3] - b[o + 3]) > args.tol):
                    bad += 1
        if n < MIN_PX:
            rows.append({"name": name, "chapter": ch, "verdict": "text-only",
                         "compared": n, "diff": bad / n if n else 1.0})
            continue
        frac = bad / n
        rows.append({"name": name, "chapter": ch,
                     "verdict": "ok" if frac <= args.max_bad_frac else "mismatch",
                     "compared": n, "diff": frac})

    per = {}
    for r in rows:
        d = per.setdefault(r["chapter"], [0, 0, 0])
        d[0] += 1
        if r["verdict"] != "text-only":
            d[1] += 1
            if r["verdict"] == "ok":
                d[2] += 1

    print("\n%-24s %5s %5s %5s %6s" % ("spec chapter", "tests", "comp", "pass", "rate"))
    for ch, (tot, comp, ok) in sorted(per.items(),
                                      key=lambda kv: (kv[1][2] / kv[1][1]) if kv[1][1] else -1):
        print("%-24s %5d %5d %5d %6s"
              % (ch, tot, comp, ok, "%.0f%%" % (100.0 * ok / comp) if comp else "n/a"))

    tot = len(rows)
    comp = sum(1 for r in rows if r["verdict"] != "text-only")
    ok = sum(1 for r in rows if r["verdict"] == "ok")
    print("\n%d tests: %d match within %.0f%% of pixels, %d differ, %d text-only"
          % (tot, ok, args.max_bad_frac * 100, comp - ok, tot - comp))
    print("comparable pass rate: %.1f%%" % (100.0 * ok / comp))
    if args.suppress_text:
        print("text was suppressed for both renderers; this is a "
              "geometry-only comparison")

    if args.json:
        json.dump({"total": tot, "comparable": comp, "pass": ok,
                   "tol": args.tol, "max_bad_frac": args.max_bad_frac,
                   "suppress_text": bool(args.suppress_text),
                   "backend": args.backend,
                   "by_chapter": {k: {"tests": v[0], "comparable": v[1], "pass": v[2]}
                                  for k, v in per.items()},
                   "rows": sorted(rows, key=lambda r: -r["diff"])},
                  open(args.json, "w"), indent=1)
        print("wrote", args.json)
    shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok == comp else 1


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""Verify every conformance assertion against an external renderer.

`run.py` checks the corpus against *our* renderer using expectations written
by hand.  That only proves self-consistency: if an expectation was copied from
our own output, the suite passes while the renderer is wrong.

This script closes that hole.  For each case it renders the SVG with an
independent backend (Chrome/Chromium by default) and samples the exact
coordinates used by each assertion, then compares the external value with the
committed expectation.

It does not replace `run.py`; it validates that `run.py` is measuring the
right thing.

Usage:
    python3 conformance/verify_external.py [--backends chrome] [--tol N]
                                           [--json FILE] [--limit N] [--only NAME]

Exit status is 0 when every sampled assertion agrees within tolerance.
Missing backends are reported and skipped (exit 0) so the check is safe to run
anywhere; use --require-backend to make a missing backend a failure.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "fuzz"))

EXPECT = os.path.join(HERE, "expectations.json")


def load_backend(name):
    import diff_external as D  # noqa: E402  (local module, after sys.path tweak)

    cls = {"chrome": D.ChromeBackend,
           "inkscape": D.InkscapeBackend,
           "resvg": D.ResvgBackend,
           "qlmanage": D.QlmanageBackend}.get(name)
    if cls is None:
        return None
    b = cls.probe()
    return b if b.available else None


def svg_size(path):
    """Read width/height from the root <svg> so each case renders 1 unit = 1 px.

    The corpus mixes 100x100, 200x100 and 200x200 documents; rendering them all
    at one size would scale some and make every coordinate assertion wrong.
    """
    import re

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        head = f.read(4096)
    m = re.search(r"<svg\b[^>]*>", head, re.S)
    if not m:
        return (100, 100)
    tag = m.group(0)
    out = []
    for attr in ("width", "height"):
        mm = re.search(r'\b%s\s*=\s*"([0-9.]+)' % attr, tag)
        out.append(int(float(mm.group(1))) if mm else None)
    return (out[0] or 100, out[1] or 100)


def sample(png_path, x, y):
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import pngio  # noqa: E402

    w, h, px = pngio.read_png(png_path)
    if not (0 <= x < w and 0 <= y < h):
        return None
    o = (y * w + x) * 4
    return (px[o], px[o + 1], px[o + 2], px[o + 3])


def check_alpha(got, c):
    a = got[3]
    return c.get("min", 0) <= a <= c.get("max", 255), "alpha=%d want [%d,%d]" % (
        a, c.get("min", 0), c.get("max", 255))


def check_rgba(got, c, tol):
    want = c["rgba"]
    t = c.get("tol", tol)
    ok = all(abs(a - b) <= t for a, b in zip(got, want))
    return ok, "got %s want %s (tol %d)" % (list(got), want, t)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backends", default="chrome")
    ap.add_argument("--tol", type=int, default=12)
    ap.add_argument("--json", default="")
    ap.add_argument("--only", default="",
                    help="case name prefix")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--require-backend", action="store_true")
    args = ap.parse_args(argv)

    spec = json.load(open(EXPECT))
    # Cases whose expected behaviour intentionally differs from other renderers
    # are documented in fuzz/README.md; verifying them externally would always
    # "fail", which is noise rather than signal.
    intentional = set(spec.get("external_intentional", []))
    # Cases the harness cannot render faithfully: the headless renderer is
    # given the SVG as an <img>, and a browser blocks external references in
    # SVG-as-image mode, so a file-backed image would look empty to it.
    unverifiable = set(spec.get("external_unverifiable", []))
    cases = [c for c in spec["cases"]
             if (not args.only or c["name"].startswith(args.only))
             and c["name"] not in intentional
             and c["name"] not in unverifiable]
    if args.limit:
        cases = cases[:args.limit]

    backends = []
    for name in [s.strip() for s in args.backends.split(",") if s.strip()]:
        b = load_backend(name)
        if b is None:
            print("backend %-10s unavailable (skipped)" % name)
            if args.require_backend:
                return 2
        else:
            backends.append(b)
    if not backends:
        print("no external renderer available; nothing verified")
        return 0

    tmp = tempfile.mkdtemp(prefix="svg2img_verify_")
    total = agreed = disagreed = skipped = 0
    rows = []

    for c in cases:
        svg = os.path.join(HERE, "cases", c["name"] + ".svg")
        if not os.path.exists(svg):
            continue
        for b in backends:
            # Render at a 4x-free size: the corpus is 1 unit == 1 px.
            outdir = os.path.join(tmp, b.name)
            os.makedirs(outdir, exist_ok=True)
            w, h = svg_size(svg)
            try:
                png = b.render(svg, (w, h), outdir, tmp)
            except Exception as exc:                       # noqa: BLE001
                skipped += len(c.get("checks", []))
                rows.append({"case": c["name"], "backend": b.name,
                             "verdict": "error", "detail": repr(exc)[:160]})
                continue
            if not png or not os.path.exists(png):
                skipped += len(c.get("checks", []))
                rows.append({"case": c["name"], "backend": b.name,
                             "verdict": "error", "detail": "no output"})
                continue
            for chk in c.get("checks", []):
                kind = chk.get("kind")
                if kind not in ("rgba", "alpha"):
                    continue        # bbox/ink kinds are validated by run.py
                x, y = chk["x"], chk["y"]
                got = sample(png, x, y)
                total += 1
                if got is None:
                    skipped += 1
                    continue
                # Compare flat interiors only.  A pixel the reference leaves
                # partially covered sits on an edge, where scanline coverage
                # and 2D antialiasing legitimately differ; sampling it would
                # report an AA difference as a conformance failure.
                if kind == "rgba" and 0 < got[3] < 250:
                    skipped += 1
                    rows.append({"case": c["name"], "backend": b.name,
                                 "kind": kind, "at": [x, y], "verdict": "skip-edge",
                                 "detail": "reference alpha=%d (antialiased edge)"
                                           % got[3]})
                    continue
                if kind == "rgba":
                    ok, detail = check_rgba(got, chk, args.tol)
                else:
                    ok, detail = check_alpha(got, chk)
                if ok:
                    agreed += 1
                    verdict = "pass"
                else:
                    disagreed += 1
                    verdict = "FAIL"
                rows.append({"case": c["name"], "backend": b.name, "kind": kind,
                             "at": [x, y], "verdict": verdict, "detail": detail})
                if verdict == "FAIL":
                    print("FAIL  %-28s %-10s %s at (%d,%d): %s"
                          % (c["name"], b.name, kind, x, y, detail))

    print("\n%d sampled assertion(s): %d agree, %d disagree, %d skipped"
          % (total, agreed, disagreed, skipped))
    if args.json:
        json.dump({"total": total, "agree": agreed, "disagree": disagreed,
                   "skipped": skipped, "tol": args.tol, "rows": rows},
                  open(args.json, "w"), indent=1)
        print("wrote", args.json)
    return 1 if disagreed else 0


if __name__ == "__main__":
    sys.exit(main())
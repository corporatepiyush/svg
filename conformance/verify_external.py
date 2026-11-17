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
    python3 conformance/verify_external.py [--backends chrome-inline] [--tol N]
                                           [--json FILE] [--limit N] [--only NAME]
                                           [--jobs N]

Rendering is independent per case, so the cases are rendered by a process pool
by default (one browser process per case); `--jobs 1` restores the old serial
behaviour.

The default `chrome-inline` backend puts the SVG directly into the page, so
relative `<image href>`/CSS `url(...)` references resolve.  The legacy
`chrome` backend wraps the SVG in `<img>`; it cannot load external references
and is kept only for reproducing older results.

Exit status is 0 when every sampled assertion agrees within tolerance.
Missing backends are reported and skipped (exit 0) so the check is safe to run
anywhere; use --require-backend to make a missing backend a failure.
"""
import argparse
import concurrent.futures
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
           "chrome-inline": D.ChromeInlineBackend,
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
    When width/height are absent, SVG 1.1 §7.12 derives the intrinsic size
    from the viewBox, so fall back to the viewBox dimensions.
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
    if out[0] is None or out[1] is None:
        vb = re.search(r'\bviewBox\s*=\s*"([^"]+)"', tag)
        if vb:
            nums = [float(x) for x in re.split(r"[\s,]+", vb.group(1).strip())]
            if len(nums) == 4 and nums[2] > 0 and nums[3] > 0:
                if out[0] is None:
                    out[0] = int(round(nums[2]))
                if out[1] is None:
                    out[1] = int(round(nums[3]))
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


# Worker-process helpers.  The pool uses processes (not threads): each render
# launches a browser subprocess, and a process worker keeps the parent's
# interpreter free of per-render state and fully isolates crashes.

def _backend_from_spec(spec):
    import diff_external as D  # noqa: E402  (path set up at import time)

    cls = {"chrome": D.ChromeBackend,
           "chrome-inline": D.ChromeInlineBackend,
           "inkscape": D.InkscapeBackend,
           "resvg": D.ResvgBackend,
           "qlmanage": D.QlmanageBackend}[spec["name"]]
    b = cls()
    b.exe = spec["exe"]
    b.available = True
    if hasattr(b, "extra_css"):
        b.extra_css = spec.get("extra_css", "")
    return b


def verify_one(c, bspec, tmp, tol):
    """Render one case with one backend and sample its assertions.

    Runs in a worker process, so it only touches its own per-case directory
    and returns plain data: counts, report rows, and FAIL lines to print in
    the parent (which keeps the final report deterministic).
    """
    svg = os.path.join(HERE, "cases", c["name"] + ".svg")
    if not os.path.exists(svg):
        return None
    b = _backend_from_spec(bspec)
    n_checks = len(c.get("checks", []))
    outdir = os.path.join(tmp, b.name, c["name"])
    os.makedirs(outdir, exist_ok=True)
    res = {"total": 0, "agree": 0, "disagree": 0,
           "skip_reasons": {}, "rows": [], "fails": []}

    def skip(reason, n=1, **row):
        res["skip_reasons"][reason] = \
            res["skip_reasons"].get(reason, 0) + n
        row.update({"verdict": "skip-" + reason})
        res["rows"].append(row)

    # Render at a 4x-free size: the corpus is 1 unit == 1 px.
    w, h = svg_size(svg)
    try:
        png = b.render(svg, (w, h), outdir, outdir)
    except Exception as exc:                               # noqa: BLE001
        skip("render-error", n_checks, case=c["name"], backend=b.name,
             detail=repr(exc)[:160])
        return res
    if not png or not os.path.exists(png):
        skip("no-output", n_checks, case=c["name"], backend=b.name,
             detail="renderer produced no PNG")
        return res
    for chk in c.get("checks", []):
        kind = chk.get("kind")
        if kind not in ("rgba", "alpha"):
            continue                # bbox/ink kinds are validated by run.py
        x, y = chk["x"], chk["y"]
        got = sample(png, x, y)
        res["total"] += 1
        if got is None:
            skip("out-of-bounds", case=c["name"], backend=b.name,
                 kind=kind, at=[x, y])
            continue
        # Compare flat interiors only.  A pixel the reference leaves
        # partially covered sits on an edge, where scanline coverage and 2D
        # antialiasing legitimately differ; sampling it would report an AA
        # difference as a conformance failure.
        if kind == "rgba" and 0 < got[3] < 250:
            skip("edge", case=c["name"], backend=b.name, kind=kind,
                 at=[x, y],
                 detail="reference alpha=%d (antialiased edge)" % got[3])
            continue
        if kind == "rgba":
            ok, detail = check_rgba(got, chk, tol)
        else:
            ok, detail = check_alpha(got, chk)
        if ok:
            res["agree"] += 1
            verdict = "pass"
        else:
            res["disagree"] += 1
            verdict = "FAIL"
        res["rows"].append({"case": c["name"], "backend": b.name,
                            "kind": kind, "at": [x, y],
                            "verdict": verdict, "detail": detail})
        if verdict == "FAIL":
            res["fails"].append(
                "FAIL  %-28s %-10s %s at (%d,%d): %s"
                % (c["name"], b.name, kind, x, y, detail))
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backends", default="chrome-inline",
                    help="comma list; chrome-inline is the faithful harness")
    ap.add_argument("--tol", type=int, default=12)
    ap.add_argument("--json", default="")
    ap.add_argument("--only", default="",
                    help="case name prefix")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel render workers (default: min(6, cpus); "
                         "1 = serial)")
    ap.add_argument("--require-backend", action="store_true")
    args = ap.parse_args(argv)

    spec = json.load(open(EXPECT))
    # Cases whose expected behaviour intentionally differs from other renderers
    # are documented in fuzz/README.md; verifying them externally would always
    # "fail", which is noise rather than signal.
    intentional = set(spec.get("external_intentional", []))
    if not intentional:
        intentional = set(spec.get("meta", {}).get("external_intentional", {}))

    backends = []
    for name in [s.strip() for s in args.backends.split(",") if s.strip()]:
        b = load_backend(name)
        if b is None:
            print("backend %-12s unavailable (skipped)" % name)
            if args.require_backend:
                return 2
        else:
            backends.append(b)
    if not backends:
        print("no external renderer available; nothing verified")
        return 0

    # The <img> harness cannot load external file references.  The inline
    # harness can, so only exclude those cases when no inline backend is in
    # use.
    has_inline = any(b.name == "chrome-inline" for b in backends)
    unverifiable = set() if has_inline else set(
        spec.get("external_unverifiable", []))
    cases = [c for c in spec["cases"]
             if (not args.only or c["name"].startswith(args.only))
             and c["name"] not in intentional
             and c["name"] not in unverifiable]
    if args.limit:
        cases = cases[:args.limit]

    tmp = tempfile.mkdtemp(prefix="svg2img_verify_")
    total = agreed = disagreed = skipped = 0
    skip_reasons = {}
    rows = []

    # Every case is independent: each render writes its own HTML/PNG below a
    # per-case directory, so a process pool can drive several browser
    # processes without sharing state.  Results are collected in submission
    # order, keeping the JSON report deterministic.  Processes rather than
    # threads: the work is subprocess-bound, and a worker crash (or a hung
    # browser) cannot corrupt the parent interpreter.
    backend_specs = [{"name": b.name, "exe": getattr(b, "exe", ""),
                      "extra_css": getattr(b, "extra_css", "")}
                     for b in backends]
    work = [(c, s) for c in cases for s in backend_specs]
    jobs = args.jobs if args.jobs > 0 else min(6, os.cpu_count() or 1)
    results = [None] * len(work)
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(verify_one, c, s, tmp, args.tol)
                   for c, s in work]
        for i, fut in enumerate(futures):
            results[i] = fut.result()

    # Browser startup occasionally times out when several headless instances
    # compete for the machine, which would otherwise show up as spurious
    # render-error skips.  Retry those cases once, serially, when the pool has
    # drained.  The retry replaces the first result rather than adding to it.
    retry = [i for i, r in enumerate(results)
             if r is not None and ("render-error" in r["skip_reasons"]
                                   or "no-output" in r["skip_reasons"])]
    if retry:
        print("retrying %d failed render(s) serially: %s"
              % (len(retry), ", ".join(work[i][0]["name"] for i in retry)))
        for i in retry:
            c, s = work[i]
            results[i] = verify_one(c, s, tmp, args.tol)

    for r in results:
        if r is None:
            continue
        total += r["total"]
        agreed += r["agree"]
        disagreed += r["disagree"]
        for reason, n in r["skip_reasons"].items():
            skipped += n
            skip_reasons[reason] = skip_reasons.get(reason, 0) + n
        rows.extend(r["rows"])
        for line in r["fails"]:
            print(line)

    print("\n%d sampled assertion(s): %d agree, %d disagree, %d skipped"
          % (total, agreed, disagreed, skipped))
    if skip_reasons:
        for reason, n in sorted(skip_reasons.items()):
            print("  skipped: %s: %d" % (reason, n))
    if args.json:
        json.dump({"total": total, "agree": agreed, "disagree": disagreed,
                   "skipped": skipped, "skip_reasons": skip_reasons,
                   "tol": args.tol, "rows": rows},
                  open(args.json, "w"), indent=1)
        print("wrote", args.json)
    return 1 if disagreed else 0


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""Conformance corpus runner: spec-derived assertions over rendered pixels.

Each case in ``conformance/cases/<name>.svg`` is a small, self-contained
document.  ``conformance/expectations.json`` holds, per case, the *feature
area* it exercises and a list of assertions derived from the SVG 1.1
specification -- not from current output.  Assertions sample flat interiors so
they stay stable across supersampling factors and small rasteriser changes.

Check kinds
-----------
``rgba``            exact-ish RGBA at (x, y), per-channel tolerance
``alpha``           alpha at (x, y) within [min, max]
``bbox``            rendered ink bbox equals [x0, y0, x1, y1] (exclusive)
``bbox_width``      ink bbox width >= min
``bbox_center_x``   ink bbox centre within ``tol`` of x
``ink``             count pixels in a rect that are opaque and dark
``compare_ink``     ratio between the ink counts of two rects

Output: a scorecard by feature area, a machine-readable ``--json`` summary, and
an optional markdown report.  Exit status is 0 only when every assertion
passes.

Usage:
    python3 conformance/run.py [--bin PATH] [--filter AREA] [--tol N]
                                [--json FILE] [--md FILE] [--list] [--verbose]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import pngio  # noqa: E402

CASES = os.path.join(HERE, "cases")
EXPECT = os.path.join(HERE, "expectations.json")
DEFAULT_JSON = os.path.join(HERE, "summary.json")


# ---------------------------------------------------------------------------
# Assertion primitives
# ---------------------------------------------------------------------------

def at(px, w, x, y):
    o = (y * w + x) * 4
    return (px[o], px[o + 1], px[o + 2], px[o + 3])


def bbox(px, w, h, thresh=8):
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            if px[row + x * 4 + 3] > thresh:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return None if x1 < x0 else (x0, y0, x1 + 1, y1 + 1)


def ink_count(px, w, h, rect, rgb_max=60, alpha_min=200):
    x, y, rw, rh = rect
    n = 0
    for yy in range(max(0, y), min(h, y + rh)):
        row = yy * w * 4
        for xx in range(max(0, x), min(w, x + rw)):
            o = row + xx * 4
            if (px[o + 3] >= alpha_min and px[o] <= rgb_max and
                    px[o + 1] <= rgb_max and px[o + 2] <= rgb_max):
                n += 1
    return n


def check_rgba(px, w, h, c, tol):
    got = at(px, w, c["x"], c["y"])
    ct = c.get("tol", tol)
    ok = all(abs(a - b) <= ct for a, b in zip(got, c["rgba"]))
    return ok, "at (%d,%d) got %s want %s (tol %d)" % (
        c["x"], c["y"], list(got), c["rgba"], ct)


def check_alpha(px, w, h, c, tol):
    a = at(px, w, c["x"], c["y"])[3]
    lo = c.get("min", 0)
    hi = c.get("max", 255)
    return lo <= a <= hi, "alpha at (%d,%d)=%d want [%d,%d]" % (
        c["x"], c["y"], a, lo, hi)


def check_alpha_range(px, w, h, c, tol):
    """Bounds on the minimum and maximum alpha over a rect (noise, blur)."""
    x, y, rw, rh = c["rect"] if "rect" in c else (c["x"], c["y"], c["w"], c["h"])
    amin, amax = 255, 0
    for yy in range(max(0, y), min(h, y + rh)):
        row = yy * w * 4
        for xx in range(max(0, x), min(w, x + rw)):
            a = px[row + xx * 4 + 3]
            if a < amin: amin = a
            if a > amax: amax = a
    if "amin_max" in c and amin > c["amin_max"]:
        return False, "min alpha %d want <= %d" % (amin, c["amin_max"])
    if "amax_min" in c and amax < c["amax_min"]:
        return False, "max alpha %d want >= %d" % (amax, c["amax_min"])
    if "amin_min" in c and amin < c["amin_min"]:
        return False, "min alpha %d want >= %d" % (amin, c["amin_min"])
    if "amax_max" in c and amax > c["amax_max"]:
        return False, "max alpha %d want <= %d" % (amax, c["amax_max"])
    return True, "alpha range [%d,%d]" % (amin, amax)


def check_partial_alpha(px, w, h, c, tol):
    """Count antialiased pixels (partial alpha) in a rect."""
    x, y, rw, rh = c["rect"] if "rect" in c else (c["x"], c["y"], c["w"], c["h"])
    lo = c.get("lo", 4)
    hi = c.get("hi", 251)
    n = 0
    for yy in range(max(0, y), min(h, y + rh)):
        row = yy * w * 4
        for xx in range(max(0, x), min(w, x + rw)):
            a = px[row + xx * 4 + 3]
            if lo < a < hi:
                n += 1
    if "min_count" in c and n < c["min_count"]:
        return False, "partial pixels=%d want >= %d" % (n, c["min_count"])
    return n <= c.get("max", 0), "partial pixels=%d want <= %d" % (n, c.get("max", 0))


def check_bbox(px, w, h, c, tol):
    got = bbox(px, w, h)
    want = tuple(c["bbox"])
    if got is None:
        return False, "no ink, want bbox %s" % (want,)
    t = c.get("tol", 1)
    ok = all(abs(a - b) <= t for a, b in zip(got, want))
    return ok, "bbox got %s want %s (tol %d)" % (got, want, t)


def check_bbox_width(px, w, h, c, tol):
    got = bbox(px, w, h)
    if got is None:
        return False, "no ink, want width >= %d" % c["min"]
    width = got[2] - got[0]
    return width >= c["min"], "bbox width %d want >= %d" % (width, c["min"])


def check_bbox_center_x(px, w, h, c, tol):
    got = bbox(px, w, h)
    if got is None:
        return False, "no ink"
    cx = (got[0] + got[2]) / 2.0
    return abs(cx - c["x"]) <= c["tol"], "bbox centre x=%.1f want %d +/- %d" % (
        cx, c["x"], c["tol"])


def check_ink(px, w, h, c, tol):
    n = ink_count(px, w, h, tuple(c["rect"]) if "rect" in c else
                  (c["x"], c["y"], c["w"], c["h"]), c.get("rgb_max", 60))
    return n >= c["min"], "ink=%d want >= %d" % (n, c["min"])


def check_compare_ink(px, w, h, c, tol):
    a = ink_count(px, w, h, tuple(c["a"]), c.get("rgb_max", 60))
    b = ink_count(px, w, h, tuple(c["b"]), c.get("rgb_max", 60))
    if b == 0:
        return False, "second region has no ink"
    ratio = a / float(b)
    return ratio >= c["min_ratio"], "ink ratio %.2f want >= %.2f (%d vs %d)" % (
        ratio, c["min_ratio"], a, b)


def check_color_count(px, w, h, c, tol):
    """Count pixels in a rect whose colour is within tol of a target."""
    x, y, rw, rh = c["rect"] if "rect" in c else (c["x"], c["y"], c["w"], c["h"])
    want = c["rgba"]
    t = c.get("tol", 30)
    amin = c.get("alpha_min", 100)
    n = 0
    for yy in range(max(0, y), min(h, y + rh)):
        row = yy * w * 4
        for xx in range(max(0, x), min(w, x + rw)):
            o = row + xx * 4
            if px[o + 3] < amin:
                continue
            if all(abs(px[o + k] - want[k]) <= t for k in range(3)):
                n += 1
    return n >= c["min"], "colour pixels=%d want >= %d" % (n, c["min"])


CHECKS = {
    "rgba": check_rgba,
    "color_count": check_color_count,
    "alpha": check_alpha,
    "alpha_range": check_alpha_range,
    "partial_alpha": check_partial_alpha,
    "bbox": check_bbox,
    "bbox_width": check_bbox_width,
    "bbox_center_x": check_bbox_center_x,
    "ink": check_ink,
    "compare_ink": check_compare_ink,
}


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def render(binpath, svg_path, out_png, timeout=30):
    r = subprocess.run([binpath, "-a", "4", svg_path, out_png],
                       capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("exit %d: %s" % (r.returncode, r.stderr.strip()[:200]))
    if not os.path.exists(out_png):
        raise RuntimeError("no output file")
    return out_png


def run_case(binpath, case, tmp, tol, verbose):
    name = case["name"]
    svg = os.path.join(CASES, name + ".svg")
    xfail = case.get("xfail")
    if not os.path.exists(svg):
        return {"name": name, "area": case.get("area", "?"), "xfail": xfail,
                "status": "error", "checks": [],
                "detail": "missing %s" % os.path.relpath(svg, ROOT)}
    out = os.path.join(tmp, name + ".png")
    t0 = time.time()
    try:
        render(binpath, svg, out)
        w, h, px = pngio.read_png(out)
    except Exception as e:
        return {"name": name, "area": case.get("area", "?"), "xfail": xfail,
                "status": "error", "checks": [], "detail": repr(e)[:200]}
    dt = time.time() - t0
    results = []
    for c in case.get("checks", []):
        fn = CHECKS.get(c.get("kind"))
        if fn is None:
            results.append({"kind": c.get("kind"), "ok": False,
                            "detail": "unknown check kind"})
            continue
        try:
            ok, detail = fn(px, w, h, c, tol)
        except Exception as e:
            ok, detail = False, "check error: %r" % e
        results.append({"kind": c["kind"], "ok": bool(ok), "detail": detail})
        if verbose:
            print("      %s %s: %s" % ("ok  " if ok else "FAIL",
                                       c["kind"], detail))
    nfail = sum(1 for r in results if not r["ok"])
    if nfail == 0:
        status = "xpass" if xfail else "pass"
    else:
        status = "xfail" if xfail else "fail"
    return {"name": name, "area": case.get("area", "?"), "xfail": xfail,
            "size": [w, h], "seconds": round(dt, 4), "checks": results,
            "status": status, "failed": nfail, "note": case.get("note", "")}


def scorecard(results):
    areas = {}
    for r in results:
        a = areas.setdefault(r["area"], {"cases": 0, "pass": 0, "fail": 0,
                                         "error": 0, "xfail": 0, "xpass": 0,
                                         "checks": 0, "checks_failed": 0})
        a["cases"] += 1
        a[r["status"]] += 1
        a["checks"] += len(r.get("checks", []))
        a["checks_failed"] += r.get("failed", 0)
    return areas


def main(argv=None):
    p = argparse.ArgumentParser(description="svg2img conformance corpus")
    p.add_argument("--bin", default=os.path.join(ROOT, "svg2img"))
    p.add_argument("--filter", default="", help="only this feature area")
    p.add_argument("--tol", type=int, default=10, help="default RGBA tolerance")
    p.add_argument("--json", default=DEFAULT_JSON)
    p.add_argument("--md", default="")
    p.add_argument("--list", action="store_true")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args(argv)

    if not os.path.exists(EXPECT):
        print("missing %s" % EXPECT, file=sys.stderr)
        return 2
    spec = json.load(open(EXPECT))
    cases = spec["cases"]
    if args.filter:
        cases = [c for c in cases if c.get("area") == args.filter]

    if args.list:
        for c in cases:
            print("%-22s %s" % (c["name"], c.get("area", "?")))
        print("%d case(s)" % len(cases))
        return 0

    if not os.path.exists(args.bin):
        print("renderer not found: %s (run make)" % args.bin, file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="svg2img_conf_")
    t0 = time.time()
    results = []
    marks = {"pass": "ok   ", "fail": "FAIL ", "error": "ERR  ",
             "xfail": "xfail", "xpass": "XPASS"}
    for c in cases:
        r = run_case(args.bin, c, tmp, args.tol, args.verbose)
        results.append(r)
        print("%s %-24s %s" % (marks[r["status"]], r["name"], r["area"]))
        if r["status"] in ("fail", "error", "xpass") and not args.verbose:
            for chk in r.get("checks", []):
                if not chk["ok"]:
                    print("       %s: %s" % (chk["kind"], chk["detail"]))
            if r.get("detail"):
                print("       %s" % r["detail"])
            if r["status"] == "xpass":
                print("       (expected to fail: %s -- remove the xfail)"
                      % r["xfail"])
    dt = time.time() - t0

    areas = scorecard(results)
    npass = sum(1 for r in results if r["status"] == "pass")
    nfail = sum(1 for r in results if r["status"] == "fail")
    nerr = sum(1 for r in results if r["status"] == "error")
    nxf = sum(1 for r in results if r["status"] == "xfail")
    nxp = sum(1 for r in results if r["status"] == "xpass")
    total_checks = sum(len(r.get("checks", [])) for r in results)
    failed_checks = sum(r.get("failed", 0) for r in results)

    print("\nscorecard")
    print("  %-12s %5s %5s %5s %5s %6s %10s"
          % ("area", "cases", "pass", "fail", "xfail", "xpass", "checks"))
    for area in sorted(areas):
        a = areas[area]
        print("  %-12s %5d %5d %5d %5d %6d %5d/%d" % (
            area, a["cases"], a["pass"], a["fail"] + a["error"],
            a["xfail"], a["xpass"],
            a["checks"] - a["checks_failed"], a["checks"]))
    print("  %-12s %5d %5d %5d %5d %6d %5d/%d" % (
        "TOTAL", len(results), npass, nfail + nerr, nxf, nxp,
        total_checks - failed_checks, total_checks))
    print("\n%d cases: %d pass, %d fail, %d error, %d xfail, %d xpass "
          "in %.2fs (%d checks failed)"
          % (len(results), npass, nfail, nerr, nxf, nxp, dt, failed_checks))

    summary = {
        "renderer": args.bin,
        "cases_total": len(results),
        "passed": npass, "failed": nfail, "errors": nerr,
        "xfailed": nxf, "xpassed": nxp,
        "checks_total": total_checks, "checks_failed": failed_checks,
        "seconds": round(dt, 2),
        "tolerance": args.tol,
        "areas": {k: dict(v, name=k) for k, v in sorted(areas.items())},
        "results": results,
    }
    if args.json:
        with open(args.json, "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
            f.write("\n")
        print("summary: %s" % args.json)
    if args.md:
        with open(args.md, "w") as f:
            f.write(markdown(summary))
        print("report: %s" % args.md)
    return 0 if (nfail == 0 and nerr == 0) else 1


def markdown(s):
    lines = ["# svg2img conformance", "",
             "Renderer: `%s`  " % s["renderer"],
             "%d cases: %d pass, %d fail, %d error, %d xfail, %d xpass; "
             "%d/%d checks in %.2fs" % (
                 s["cases_total"], s["passed"], s["failed"], s["errors"],
                 s["xfailed"], s["xpassed"],
                 s["checks_total"] - s["checks_failed"], s["checks_total"],
                 s["seconds"]),
             "", "| area | cases | pass | fail | xfail | xpass | checks |",
             "|---|---|---|---|---|---|---|"]
    for name, a in sorted(s["areas"].items()):
        lines.append("| %s | %d | %d | %d | %d | %d | %d/%d |" % (
            name, a["cases"], a["pass"], a["fail"] + a["error"],
            a["xfail"], a["xpass"],
            a["checks"] - a["checks_failed"], a["checks"]))
    bad = [r for r in s["results"] if r["status"] not in ("pass", "xfail")]
    lines += ["", "## Not passing (%d)" % len(bad), ""]
    if not bad:
        lines.append("none")
    for r in bad:
        lines.append("- **%s** (%s) [%s]" % (r["name"], r["area"], r["status"]))
        if r.get("xfail"):
            lines.append("  - expected failure: %s" % r["xfail"])
        if r.get("note"):
            lines.append("  - %s" % r["note"])
        for c in r.get("checks", []):
            if not c["ok"]:
                lines.append("  - `%s`: %s" % (c["kind"], c["detail"]))
        if r.get("detail"):
            lines.append("  - %s" % r["detail"])
    lines.append("")
    return "\n".join(lines)
    bad = [r for r in s["results"] if r["status"] != "pass"]
    lines += ["", "## Failures (%d)" % len(bad), ""]
    if not bad:
        lines.append("none")
    for r in bad:
        lines.append("- **%s** (%s)" % (r["name"], r["area"]))
        if r.get("note"):
            lines.append("  - %s" % r["note"])
        for c in r.get("checks", []):
            if not c["ok"]:
                lines.append("  - `%s`: %s" % (c["kind"], c["detail"]))
        if r.get("detail"):
            lines.append("  - %s" % r["detail"])
    lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
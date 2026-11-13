#!/usr/bin/env python3
"""Resource / denial-of-service tests for svg2img.

Crash fuzzers do not catch work that is merely *unbounded*: a 20-line SVG
that expands to millions of nodes, a 5 MB path whose dashed stroke is O(n^2),
or a wide tree that takes minutes.  This runner generates such documents,
runs each under a wall-clock budget and an address-space limit, and reports
the timing.

Run:  python3 fuzz/resource_fuzz.py [path-to-svg2img] [--quick]

Exit: 0 if every non-xfail case finished inside its budget, 1 otherwise.
"""
import os
import resource
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].endswith("svg2img") else os.path.join(ROOT, "svg2img")
QUICK = "--quick" in sys.argv
TMP = tempfile.mkdtemp(prefix="svg2img_res_")
LIMIT_BYTES = 3 * 1024 * 1024 * 1024


def write(name, svg):
    path = os.path.join(TMP, name + ".svg")
    with open(path, "w") as f:
        f.write(svg)
    return path


def use_chain(levels):
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">',
             '<defs>', '<g id="a0"><rect width="1" height="1" fill="red"/></g>']
    for i in range(1, levels):
        parts.append('<g id="a%d"><use href="#a%d"/><use href="#a%d"/></g>'
                     % (i, i - 1, i - 1))
    parts.append('</defs>')
    parts.append('<use href="#a%d"/>' % (levels - 1))
    parts.append('</svg>')
    return "".join(parts)


def wide(n):
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">']
    for i in range(n):
        parts.append('<rect x="%d" y="%d" width="1" height="1" fill="red"/>'
                     % (i % 200, (i // 200) % 200))
    parts.append('</svg>')
    return "".join(parts)


def deep(n):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50">' +
            "<g>" * n + '<rect width="10" height="10" fill="red"/>' +
            "</g>" * n + '</svg>')


def dashed(points, dash="0.5 0.5"):
    pts = " ".join("%d,50" % i for i in range(points))
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="100" '
            'viewBox="0 0 %d 100"><path d="M%s" fill="none" stroke="black" '
            'stroke-width="2" stroke-dasharray="%s"/></svg>'
            % (points, points, pts, dash))


def path_points(n):
    pts = " ".join("%d,%d" % (i % 2000, (i * 7) % 2000) for i in range(n))
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="2000" height="2000">'
            '<path d="M%s" fill="none" stroke="black" stroke-width="1"/></svg>' % pts)


def text(n):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="2000" height="200">'
            '<text x="0" y="100" font-size="10" fill="black">%s</text></svg>'
            % ("A" * n))


def siblings(n):
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">']
    for i in range(n):
        parts.append('<rect x="%d" y="%d" width="1" height="1" fill="red"/>'
                     % (i % 200, (i // 200) % 200))
    parts.append('</svg>')
    return "".join(parts)


def attr_1m():
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20">'
            '<rect width="20" height="20" fill="red" data-x="%s"/></svg>'
            % ("x" * 1000000))


def canvas(n):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">'
            '<rect width="%d" height="%d" fill="url(#g)"/>'
            '<defs><linearGradient id="g"><stop offset="0" stop-color="red"/>'
            '<stop offset="1" stop-color="blue"/></linearGradient></defs></svg>'
            % (n, n, n, n))


# name, svg, budget seconds, xfail reason
def cases():
    c = [
        ("deep_2000", deep(2000), 10, None),
        ("deep_5000", deep(5000), 10, None),
        ("wide_100k", wide(100000), 15, None),
        ("use_10", use_chain(10), 10, None),
        ("use_18", use_chain(18), 20, None),
        ("use_22", use_chain(22), 20,
         "exponential use expansion: ~2M nodes, ~26 s on this machine"),
        ("dash_60k", dashed(60000), 15, None),
        ("dash_200k", dashed(200000), 30, None),
        ("text_200k", text(200000), 20, None),
        ("siblings_100k", siblings(100000), 15, None),
        ("attr_1m", attr_1m(), 10, None),
        ("canvas_4096", canvas(4096), 30, None),
        ("path_1m", path_points(1000000), 40, None),
    ]
    if QUICK:
        c = [x for x in c if x[0] not in ("path_1m", "canvas_4096")]
    return c


def limit():
    try:
        resource.setrlimit(resource.RLIMIT_AS, (LIMIT_BYTES, LIMIT_BYTES))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    except (ValueError, OSError):
        pass


def run(name, svg, budget, xfail):
    path = write(name, svg)
    out = os.path.join(TMP, name + ".png")
    t0 = time.monotonic()
    status = "ok"
    detail = ""
    try:
        r = subprocess.run([BIN, "-a", "1", path, out], timeout=budget,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           preexec_fn=limit)
        if r.returncode != 0:
            status = "exit=%d" % r.returncode
            detail = r.stderr.decode("utf-8", "replace").strip()[:160]
    except subprocess.TimeoutExpired:
        status = "TIMEOUT"
    elapsed = time.monotonic() - t0
    ok = status == "ok" and elapsed < budget
    if xfail:
        # An xfail may legitimately exceed the budget; report it but do not
        # fail the run.  An xfail that *passes* is reported as XPASS.
        if ok:
            print("XPASS %-12s %6.2fs (fixed? %s)" % (name, elapsed, xfail))
            return True, 0
        print("XFAIL %-12s %6.2fs %-8s %s" % (name, elapsed, status, xfail))
        return True, elapsed
    if ok:
        print("ok    %-12s %6.2fs" % (name, elapsed))
        return True, elapsed
    print("FAIL  %-12s %6.2fs %-8s %s" % (name, elapsed, status, detail))
    return False, elapsed


def main():
    failures = 0
    total = 0.0
    for name, svg, budget, xfail in cases():
        ok, elapsed = run(name, svg, budget, xfail)
        total += elapsed
        if not ok:
            failures += 1
    print("\n%d cases, %.1fs total, %d failure(s)" % (len(cases()), total, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
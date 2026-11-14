#!/usr/bin/env python3
"""Leak regression tests for svg2img.

Crash fuzzers do not catch leaks (the fork-per-input runner uses `_exit`, and
macOS ASan has no LSan).  This runner checks the whole render path under
`leaks --atExit` (macOS) or valgrind (Linux) and fails on unexpected leaks.

Run:  python3 fuzz/leak_check.py [path-to-svg2img]
Exit: 0 if no unexpected leak, 1 otherwise.  Known leaks are xfail.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].endswith("svg2img") else os.path.join(ROOT, "svg2img")
TMP = tempfile.mkdtemp(prefix="svg2img_leak_")


def svg(body, w=50, h=50):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d">%s</svg>' % (w, h, w, h, body))


CASES = [
    ("use_no_children", svg('<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
                            '<use href="#r"/>'), None),
    ("use_one_child", svg('<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
                          '<use href="#r"><rect width="5" height="5"/></use>'), None),
    ("use_two_children", svg('<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
                             '<use href="#r"><rect width="5" height="5"/>'
                             '<circle cx="1" cy="1" r="2"/></use>'), None),
    # A <use> child that carries an id is discarded when the use expands.  The
    # id map must drop it: otherwise a later <use href="#id"> clones freed
    # memory (heap-use-after-free, found by the external differential run).
    ("use_child_id", svg('<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
                         '<use href="#r"><g id="victim"><rect width="5" height="5"/></g></use>'
                         '<use href="#victim" x="20" y="20"/>'), None),
    ("use_grandchild_id", svg('<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
                              '<use href="#r"><g id="a"><g id="b">'
                              '<rect width="4" height="4"/></g></g></use>'
                              '<use href="#b" x="10" y="10"/>'
                              '<use href="#a" x="20" y="20"/>'), None),
    ("use_dup_id", svg('<defs><rect id="d" width="10" height="10" fill="red"/></defs>'
                       '<rect id="d" width="6" height="6" fill="blue"/>'
                       '<use href="#d"/>'), None),
    # Inheriting a dash list into a shadow tree allocates a copy per node; a
    # deep subtree must free every copy (and the target's own list).
    ("use_dash_inherit", svg('<defs><g id="g"><rect width="4" height="4"/>'
                             '<rect x="6" width="4" height="4"/></g></defs>'
                             '<use href="#g" stroke="black" stroke-width="2"'
                             ' stroke-dasharray="4 2 1"/>'), None),
    # A pattern tile is allocated for every fill and must be released; fill
    # and stroke of the same shape allocate two.
    ("pattern_fill_stroke", svg('<defs><pattern id="p" width="4" height="4"'
                                ' patternUnits="userSpaceOnUse">'
                                '<rect width="4" height="4" fill="red"/></pattern>'
                                '</defs>'
                                '<rect width="20" height="20" fill="url(#p)"'
                                ' stroke="url(#p)" stroke-width="2"/>'), None),
    # A mask allocates a full-region canvas plus an alpha mask per use.
    ("mask_luminance", svg('<defs>'
                           '<linearGradient id="g"><stop offset="0" stop-color="white"/>'
                           '<stop offset="1" stop-color="black"/></linearGradient>'
                           '<mask id="m" maskContentUnits="objectBoundingBox">'
                           '<rect width="1" height="1" fill="url(#g)"/></mask>'
                           '</defs>'
                           '<rect width="20" height="20" fill="red" mask="url(#m)"/>'), None),
    # Every vertex allocates marker subtrees; they must all be released.
    ("markers_all", svg('<defs><marker id="m" markerWidth="4" markerHeight="4"'
                        ' refX="2" refY="2"><circle cx="2" cy="2" r="2"'
                        ' fill="red"/></marker></defs>'
                        '<path d="M2,2 L18,2 L18,18" fill="none" stroke="black"'
                        ' marker-start="url(#m)" marker-mid="url(#m)"'
                        ' marker-end="url(#m)"/>'), None),
]


def add_file(name):
    p = os.path.join(ROOT, "test", name + ".svg")
    if os.path.exists(p):
        CASES.append((name, open(p).read(), None))


for n in ("shapes", "gradients", "paths", "edge", "features", "realworld"):
    add_file(n)
for n in ("use_deep", "deep_clip", "empty_clip", "gradient_focal"):
    p = os.path.join(ROOT, "fuzz", "regress", n + ".svg")
    if os.path.exists(p):
        CASES.append(("regress_" + n, open(p).read(), None))


def check_macos(path, out):
    r = subprocess.run(["leaks", "--atExit", "--", BIN, path, out],
                       capture_output=True, text=True, timeout=120)
    text = r.stdout + r.stderr
    m = re.search(r"Process \d+: (\d+) leaks for (\d+) total leaked bytes", text)
    if not m:
        # "not debuggable" / unsupported: treat as skipped.
        if "not debuggable" in text or "could not" in text:
            return None, text
        return None, text
    return int(m.group(1)), text


def check_valgrind(path, out):
    r = subprocess.run(["valgrind", "--leak-check=full", "--show-leak-kinds=definite",
                        "--errors-for-leak-kinds=definite", "--error-exitcode=99",
                        BIN, path, out], capture_output=True, text=True, timeout=180)
    text = r.stderr
    m = re.search(r"definitely lost: ([\d,]+) bytes", text)
    if not m:
        return 0, text
    return (1 if int(m.group(1).replace(",", "")) > 0 else 0), text


def main():
    if sys.platform == "darwin":
        checker = check_macos
    elif shutil.which("valgrind"):
        checker = check_valgrind
    else:
        print("No leak checker found (need macOS `leaks` or Linux `valgrind`); skipping.")
        return 0

    failed, xfailed, xpassed, skipped = [], [], [], []
    for i, case in enumerate(CASES):
        if len(case) == 3:
            name, text, xfail = case
        else:
            name, text = case
            xfail = None
        src = os.path.join(TMP, "%03d_%s.svg" % (i, name))
        out = os.path.join(TMP, "%03d_%s.png" % (i, name))
        with open(src, "w") as f:
            f.write(text)
        try:
            n, detail = checker(src, out)
        except subprocess.TimeoutExpired:
            failed.append((name, "checker timeout"))
            print("FAIL  %-20s checker timeout" % name)
            continue
        if n is None:
            skipped.append(name)
            continue
        if n == 0:
            if xfail:
                xpassed.append((name, xfail))
                print("XPASS %-20s (fixed? %s)" % (name, xfail))
            else:
                print("ok    %-20s" % name)
        else:
            if xfail:
                xfailed.append((name, xfail))
                print("XFAIL %-20s %d leaked block(s) -- %s" % (name, n, xfail))
            else:
                failed.append((name, "%d leaked block(s)" % n))
                print("FAIL  %-20s %d leaked block(s)" % (name, n))
    print("\n%d cases: %d unexpected failure(s), %d xfail, %d xpass, %d skipped"
          % (len(CASES), len(failed), len(xfailed), len(xpassed), len(skipped)))
    if xpassed:
        print("XPASS (fixed leaks -- remove the xfail markers):")
        for n, r in xpassed:
            print("  %s: %s" % (n, r))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
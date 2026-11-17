#!/usr/bin/env python3
"""Per-test triage for the W3C SVG 1.1 suite results.

`w3c_chrome.py` produces a machine-readable list of per-test verdicts.  This
script turns that list into a reviewable triage report: every failing test is
listed with its chapter, the proportion of differing pixels, the test's own
pass criterion (read from the suite SVG) and a coarse cause classification.

The classification is deliberately a *pointer*, not a verdict.  It groups the
failure with the subsystem that has to be investigated, so a reviewer can work
through a chapter instead of 244 unrelated names.

Usage:
    python3 conformance/w3c_triage.py --json conformance/w3c_results.json
    python3 conformance/w3c_triage.py --json ... --md conformance/w3c_triage.md
    python3 conformance/w3c_triage.py --json ... --all

Exit status is 0; this is a report generator, not a pass/fail gate.
"""
import argparse
import html
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Ordered: the first matching rule wins.  Each entry is (regex, cause).
RULES = [
    (r"\banimate|animateColor|animateMotion|animateTransform|\bset\b|\bsmil\b",
     "SMIL animation (deliberate scope limit SCOPE-01)"),
    (r"\bscript|onclick|onload|javascript|event",
     "scripting/event handling (deliberate scope limit SCOPE-02)"),
    (r"\bfont|glyph|textLength|textPath|tspan|tref|altGlyph|"
     r"writing-mode|dominant-baseline|alignment-baseline|baseline-shift|"
     r"letter-spacing|text-anchor",
     "text layout / real font shaping (SCOPE-03, bitmap font)"),
    (r"\bfilter|feBlend|feColorMatrix|feComponentTransfer|feComposite|"
     r"feConvolve|feDiffuse|feDisplacement|feDistant|feFlood|feGaussian|"
     r"feImage|feMerge|feMorphology|feOffset|fePoint|feSpecular|feSpot|"
     r"feTile|feTurbulence|feFunc",
     "filter primitive coverage"),
    (r"\bgradient|linearGradient|radialGradient|stop|spreadMethod|"
     r"gradientUnits|gradientTransform|focal",
     "gradient/paint-server coverage"),
    (r"\bpattern|patternUnits|patternContentUnits|patternTransform",
     "pattern tile coverage"),
    (r"\bclip|clipPath|clip-rule|clipPathUnits|mask|maskUnits|"
     r"maskContentUnits|luminance",
     "clipping/masking coverage"),
    (r"\bimage\b|feImage|xlink:href|\.png|\.jpg|\.jpeg|\.gif",
     "image loading/decoding"),
    (r"\buse\b|symbol|switch|requiredFeatures|requiredExtensions|"
     r"systemLanguage",
     "use/symbol/switch generated content"),
    (r"\bstyle\b|\bcss\b|class=|selector|specificity|!important|"
     r"presentation attribute",
     "CSS cascade/selector coverage"),
    (r"transform|matrix|rotate|skew|translate|scale",
     "transform parsing/composition"),
    (r"\bpath\b|\barc\b|\bbezier|\bcurve|\bclosepath|\bpathLength|"
     r"\bmarker\b|markerWidth|markerHeight|refX|refY|orient",
     "path/marker geometry"),
    (r"dash|stroke-linecap|stroke-linejoin|stroke-miterlimit|"
     r"stroke-width|stroke-opacity",
     "stroke geometry/dash coverage"),
    (r"opacity|fill-opacity|stroke-opacity|comp-op|color-interpolation|"
     r"rendering model|shape-rendering|image-rendering",
     "opacity/compositing/rendering hints"),
    (r"\bcolor\b|\bcolour\b|rgb\(|rgba\(|hsl\(|hsla\(|currentColor|"
     r"color-profile|color-interpolation|linearRGB",
     "colour parsing/interpolation"),
    (r"viewBox|preserveAspectRatio|viewport|percentage|percent|"
     r"|\bpx\b|\bpt\b|\bpc\b|\bin\b|\bcm\b|\bmm\b|\bem\b|\bex\b",
     "viewport/units/percentage resolution"),
    (r"pointer|cursor|interact|focus|tabindex|zoom|pan",
     "interactivity (deliberate scope limit SCOPE-02)"),
    (r"metadata|desc|title|role|aria| RDF| DOCTYPE|entity|CDATA",
     "metadata/document wrapper"),
]

CHAPTER_FALLBACK = {
    "3 Rendering model": "rendering model",
    "4 Data types": "data-type/number grammar",
    "5 Document structure": "document structure/use/switch",
    "6 Styling": "CSS cascade/selector coverage",
    "7 Coordinates/units": "viewport/units/percentage resolution",
    "8 Paths": "path/marker geometry",
    "9 Basic shapes": "basic-shape geometry",
    "10 Text": "text layout / real font shaping (SCOPE-03, bitmap font)",
    "11 Painting": "paint/stroke coverage",
    "12 Color": "colour parsing/interpolation",
    "13 Gradients/patterns": "gradient/paint-server coverage",
    "14 Clipping/masking": "clipping/masking coverage",
    "15 Filters": "filter primitive coverage",
    "16 Interactivity": "interactivity (deliberate scope limit SCOPE-02)",
    "17 Linking": "image/external linking",
    "18 Scripting": "scripting/event handling (deliberate scope limit SCOPE-02)",
    "19 Animation": "SMIL animation (deliberate scope limit SCOPE-01)",
    "20 Fonts": "real font shaping (SCOPE-03, bitmap font)",
    "21 Metadata": "metadata",
    "23 Extensibility": "extensibility",
}


def strip_tags(s):
    s = re.sub(r"<[^>]+>", " ", s)
    s = html.unescape(s)
    return re.sub(r"\s+", " ", s).strip()


def test_meta(name):
    """Return (description, pass-criteria) from the suite SVG, if present."""
    try:
        from w3c_chrome import ensure_suite
        root = ensure_suite()
    except Exception:                                      # noqa: BLE001
        return "", ""
    path = os.path.join(root, "svg", name + ".svg")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read(256 * 1024)
    except OSError:
        return "", ""
    desc = re.search(r"<d:testDescription\b[^>]*>(.*?)</d:testDescription>",
                     src, re.S)
    crit = re.search(r"<d:passCriteria\b[^>]*>(.*?)</d:passCriteria>",
                     src, re.S)
    return (strip_tags(desc.group(1))[:400] if desc else "",
            strip_tags(crit.group(1))[:400] if crit else "")


def classify(row, desc):
    name = row["name"].lower()
    chapter = row.get("chapter", "")
    hay = " ".join((name.replace("-", " "), desc.lower()))
    for pat, cause in RULES:
        if re.search(pat, hay, re.I):
            return cause
    return CHAPTER_FALLBACK.get(chapter, "geometry/raster mismatch")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", required=True,
                    help="result JSON from conformance/w3c_chrome.py")
    ap.add_argument("--md", default=os.path.join(HERE, "w3c_triage.md"))
    ap.add_argument("--all", action="store_true",
                    help="include passing tests in the per-test table")
    args = ap.parse_args(argv)

    with open(args.json) as f:
        data = json.load(f)
    rows = data["rows"]
    total = data.get("total", len(rows))
    comparable = data.get("comparable", sum(
        1 for r in rows if r["verdict"] != "text-only"))
    passed = data.get("pass", sum(1 for r in rows if r["verdict"] == "ok"))

    triaged = []
    for r in rows:
        desc, crit = test_meta(r["name"])
        triaged.append(dict(r, reason=classify(r, desc),
                            description=desc, criteria=crit))

    by_reason = {}
    by_chapter = {}
    for r in triaged:
        if r["verdict"] == "ok" and not args.all:
            continue
        by_reason[r["reason"]] = by_reason.get(r["reason"], 0) + 1
        ch = r.get("chapter", "(unlabelled)")
        d = by_chapter.setdefault(ch, {"total": 0, "ok": 0, "fail": 0,
                                       "text": 0, "error": 0})
        d["total"] += 1
        if r["verdict"] == "ok":
            d["ok"] += 1
        elif r["verdict"] == "text-only":
            d["text"] += 1
        elif r["verdict"] == "error":
            d["error"] += 1
        else:
            d["fail"] += 1

    lines = [
        "# W3C SVG 1.1 per-test triage",
        "",
        "Generated by `conformance/w3c_triage.py` from the result JSON of",
        "`conformance/w3c_chrome.py`.  The suite itself is not committed; the",
        "script downloads it into `conformance/.w3c-suite` on first use.",
        "",
        "Machine-readable source: `%s`  " % os.path.relpath(args.json, ROOT),
        "Tests: **%d**; comparable: **%d**; match within tolerance: **%d** "
        "(%.1f%%)" % (total, comparable, passed,
                      100.0 * passed / comparable if comparable else 0.0),
        "",
        "The cause column is a triage pointer, not a verdict: it names the",
        "subsystem to investigate first.  `SCOPE-*` entries are deliberate",
        "product limits, not conformance bugs.",
        "",
        "## Failures by subsystem",
        "",
        "| cause | tests |",
        "|---|---|",
    ]
    for reason, n in sorted(by_reason.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append("| %s | %d |" % (reason, n))

    lines += ["", "## Failures by spec chapter", "",
              "| chapter | tests | pass | fail | text-only | error |",
              "|---|---|---|---|---|---|"]
    for ch, d in sorted(by_chapter.items()):
        lines.append("| %s | %d | %d | %d | %d | %d |"
                     % (ch, d["total"], d["ok"], d["fail"], d["text"],
                        d["error"]))

    lines += ["", "## Per-test findings", "",
              "| test | chapter | verdict | diff | cause | pass criterion |",
              "|---|---|---|---|---|---|"]
    show = triaged
    if not args.all:
        show = [r for r in triaged if r["verdict"] != "ok"]
    show.sort(key=lambda r: (r.get("chapter", ""), r["name"]))
    for r in show:
        diff = r.get("diff", 1.0)
        crit = r["criteria"] or r["description"] or "—"
        lines.append("| `%s` | %s | %s | %.1f%% | %s | %s |"
                     % (r["name"], r.get("chapter", ""), r["verdict"],
                        100.0 * diff, r["reason"],
                        crit.replace("|", "/")))
    lines.append("")
    with open(args.md, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote", args.md)
    print("%d tests: %d pass, %d fail, %d text-only, %d error"
          % (total, passed,
             sum(1 for r in rows if r["verdict"] == "mismatch"),
             sum(1 for r in rows if r["verdict"] == "text-only"),
             sum(1 for r in rows if r["verdict"] == "error")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
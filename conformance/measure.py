#!/usr/bin/env python3
"""Measure the conformance corpus and emit per-case pixel expectations.

This is a *development* helper, not part of the test run.  It renders every
case with svg2img, samples a list of probe points per case (declared in
``PROBES``), and writes ``conformance/expectations.json``.

The checked-in expectations file is what ``conformance/run.py`` compares
against, so re-run this only when a case intentionally changes:

    python3 conformance/measure.py > conformance/expectations.json

Every probe point must be *interior* (away from antialiased edges) so the
expectation is stable across supersampling factors.
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "fuzz"))

import semantics as S  # noqa: E402

S.BIN = os.path.join(ROOT, "svg2img")

# Points chosen to sit inside flat fills, away from edges.  Values are
# measured, then frozen; tolerance is applied by the runner.
PROBES = {
    "shapes_rect": [(50, 40), (12, 22), (89, 59)],
    "shapes_rrect": [(50, 40), (12, 12), (50, 15)],
    "shapes_circle": [(50, 50), (50, 22), (26, 50)],
    "shapes_ellipse": [(50, 50), (14, 50), (50, 32)],
    "shapes_line": [(50, 50), (30, 65)],
    "shapes_polyline": [(50, 42), (30, 62)],
    "shapes_polygon": [(50, 40), (50, 75)],
    "path_lines": [(50, 50), (50, 15)],
    "path_cubic": [(50, 70), (50, 40)],
    "path_quad": [(50, 75), (50, 55)],
    "path_arc": [(50, 45), (50, 20)],
    "path_relative": [(50, 50), (15, 15)],
    "path_evenodd": [(50, 50), (50, 15)],
    "path_nonzero": [(50, 50), (50, 15)],
    "path_stroke": [(50, 50)],
    "transform_translate": [(50, 50), (45, 35)],
    "transform_rotate": [(50, 50), (50, 38)],
    "transform_scale": [(20, 20), (60, 60)],
    "transform_matrix": [(20, 20), (50, 22)],
    "transform_nested": [(25, 10), (55, 35)],
    "viewbox_scale": [(30, 30), (70, 70), (15, 15)],
    "par_meet": [(100, 50), (25, 50)],
    "par_none": [(100, 50), (25, 50)],
    "par_slice": [(100, 50), (25, 50)],
    "grad_linear": [(5, 50), (95, 50), (50, 50)],
    "grad_radial": [(50, 50), (95, 50), (20, 20)],
    "grad_focal": [(25, 50), (5, 50), (95, 50)],
    "grad_spread": [(50, 50), (75, 50), (95, 50)],
    "grad_stop_opacity": [(5, 50), (50, 50), (95, 50)],
    "grad_user_space": [(5, 50), (95, 50)],
    "clip_rect": [(50, 50), (10, 10)],
    "clip_obb": [(50, 50), (12, 12)],
    "clip_rule": [(50, 15), (50, 50)],
    "clip_nested": [(50, 50), (15, 15)],
    "opacity_group": [(50, 50)],
    "opacity_fill": [(20, 50), (50, 50)],
    "opacity_nested": [(50, 50)],
    "use_basic": [(60, 60), (10, 10)],
    "use_transform": [(70, 30), (10, 10)],
    "use_symbol": [(20, 20), (50, 50)],
    "text_basic": [(20, 30)],
    "text_anchor": [(40, 30), (60, 30)],
    "text_spacing": [(12, 40)],
    "text_fontsize": [(12, 36), (12, 80)],
    "style_inline_fill": [(50, 50)],
    "style_inline_opacity": [(50, 50)],
    "style_element": [(50, 50)],
    "units_absolute": [(50, 50), (10, 10)],
    "units_cm": [(20, 20)],
    "units_percent": [(20, 20)],
    "units_em": [(20, 20)],
    "colors_solid": [(5, 5), (37, 5), (72, 5), (5, 37), (37, 37), (72, 37),
                     (5, 72), (37, 72), (72, 72)],
    "stroke_dash": [(14, 50), (22, 50)],
    "stroke_dashoffset": [(14, 50), (22, 50)],
    "stroke_cap_round": [(50, 17), (50, 50)],
    "stroke_cap_butt": [(50, 22), (50, 50)],
    "stroke_join_miter": [(50, 16), (50, 50)],
    "stroke_join_round": [(50, 20), (50, 50)],
    "stroke_miterlimit": [(50, 16), (50, 50)],
    "stroke_width": [(50, 50), (50, 46)],
}


def main():
    out = {}
    for path in sorted(glob.glob(os.path.join(HERE, "cases", "*.svg"))):
        name = os.path.splitext(os.path.basename(path))[0]
        svg = open(path).read()
        try:
            w, h, px = S.render_svg(svg, name="conf_" + name)
        except S.RenderError as e:
            print("skip %s: %s" % (name, e), file=sys.stderr)
            continue
        samples = []
        for (x, y) in PROBES.get(name, []):
            samples.append({"xy": [x, y], "rgba": list(S.at(px, w, x, y))})
        entry = {
            "file": os.path.relpath(path, ROOT),
            "size": [w, h],
            "bbox": list(S.bbox_alpha(px, w, h) or ()),
            "pixels": samples,
        }
        out[name] = entry
    json.dump(out, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
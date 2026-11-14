#!/usr/bin/env python3
"""Oracle-based semantic tests for svg2img.

Unlike the crash fuzzers, these assert *what the image should look like*.
Each case is a small SVG plus one or more pixel predicates.  Cases marked
``xfail`` document a confirmed conformance bug; they are expected to fail
today and will report ``XPASS`` when the bug is fixed (remove the xfail then).

Run:  python3 fuzz/semantic_cases.py [path-to-svg2img]
Exit: 0 if no unexpected failures, 1 otherwise.
"""
import os
import sys

import semantics as S

# ---------------------------------------------------------------------------
# Case helpers
# ---------------------------------------------------------------------------

def case(name, body, checks, w=100, h=100, xfail=None):
    return {"name": name, "svg": S.svg_doc(body, w, h),
            "checks": checks, "xfail": xfail}


def expect_rgba(x, y, want, tol=20):
    def check(px, w, h):
        got = S.at(px, w, x, y)
        ok = all(abs(a - b) <= tol for a, b in zip(got, want))
        return ok, "got %r want %r" % (got, want)
    return check


def expect_alpha_at(x, y, lo, hi):
    def check(px, w, h):
        a = S.alpha(px, w, x, y)
        return lo <= a <= hi, "alpha=%d want [%d,%d]" % (a, lo, hi)
    return check


def expect_transparent(x, y, tol=8):
    def check(px, w, h):
        a = S.alpha(px, w, x, y)
        return a <= tol, "alpha=%d want <=%d" % (a, tol)
    return check


def expect_red_at(x, y):
    def check(px, w, h):
        r, g, b, a = S.at(px, w, x, y)
        return S.is_red(r, g, b, a), "got %r" % ((r, g, b, a),)
    return check


def expect_blue_at(x, y):
    def check(px, w, h):
        r, g, b, a = S.at(px, w, x, y)
        return S.is_blue(r, g, b, a), "got %r" % ((r, g, b, a),)
    return check


def expect_lime_at(x, y):
    def check(px, w, h):
        r, g, b, a = S.at(px, w, x, y)
        return S.is_lime(r, g, b, a), "got %r" % ((r, g, b, a),)
    return check


def expect_count(pred, lo, hi, label):
    def check(px, w, h):
        n = S.count(px, w, h, pred)
        return lo <= n <= hi, "%s count=%d want [%d,%d]" % (label, n, lo, hi)
    return check


def expect_gradient_t(x, y, want, tol=0.08):
    def check(px, w, h):
        r, g, b, a = S.at(px, w, x, y)
        if a < 200:
            return False, "transparent at (%d,%d)" % (x, y)
        t = S.gradient_t(r, g, b, a)
        return abs(t - want) <= tol, "t=%.3f want %.2f (rgba=%r)" % (t, want, (r, g, b, a))
    return check


# ---------------------------------------------------------------------------
# Conformance cases that used to fail.  Every one of these was an xfail; they
# are kept here as ordinary cases so a regression is caught immediately.
# ---------------------------------------------------------------------------

FIXED_CASES = [
    # --- open subpaths are filled as if closed (SVG 1.1 §11.4) ---
    case("open_polyline_fill",
         '<polyline points="10,10 90,10 50,90" fill="red"/>',
         [expect_red_at(50, 40)]),
    case("open_path_fill",
         '<path d="M10,10 L90,10 L50,90" fill="red"/>',
         [expect_red_at(50, 40)]),
    # --- Z followed by numbers starts a new subpath (SVG 1.1 §8.3) ---
    case("z_then_implicit_lineto",
         '<path d="M10,10 L60,10 L60,60 Z 100,10 L150,10 L150,60" fill="red"/>',
         [expect_red_at(45, 35), expect_red_at(135, 35)],
         w=200, h=100),
    # --- use shadow-tree style inheritance (SVG 1.1 §5.6) ---
    case("use_style_inherit",
         '<defs><rect id="r" x="0" y="0" width="40" height="40"/></defs>'
         '<use href="#r" x="0" y="0" fill="red"/>',
         [expect_red_at(20, 20)]),
    # --- forward / nested use references ---
    case("use_forward_nested",
         '<use href="#g1" x="5" y="5"/>'
         '<defs><rect id="r" x="0" y="0" width="40" height="40" fill="red"/>'
         '<g id="g1"><use href="#r"/>'
         '<rect x="50" y="0" width="40" height="40" fill="blue"/></g></defs>',
         [expect_red_at(15, 15), expect_blue_at(70, 15)]),
    # --- chained gradient href, forward reference (SVG 1.1 §13.2) ---
    case("gradient_href_chain_forward",
         '<defs>'
         '<linearGradient id="g3" href="#g2"/>'
         '<linearGradient id="g2" href="#g1"/>'
         '<linearGradient id="g1" x1="0" y1="0" x2="1" y2="0">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient></defs>'
         '<rect x="0" y="0" width="100" height="100" fill="url(#g1)"/>'
         '<rect x="100" y="0" width="100" height="100" fill="url(#g3)"/>',
         [expect_red_at(10, 50), expect_blue_at(190, 50)],
         w=200, h=100),
    # --- symbol must not render at the top level (SVG 1.1 §5.7) ---
    case("symbol_not_rendered",
         '<symbol id="s"><rect x="0" y="0" width="100" height="100" fill="green"/></symbol>',
         [expect_transparent(50, 50)]),
    # --- nested <svg> establishes a new viewport (SVG 1.1 §5.1) ---
    case("nested_svg_viewport",
         '<svg x="50" y="50" width="100" height="100" viewBox="0 0 50 50">'
         '<rect x="0" y="0" width="50" height="50" fill="red"/></svg>',
         [expect_red_at(100, 100), expect_transparent(25, 25)],
         w=200, h=200),
    # --- visibility is inherited but can be overridden (SVG 1.1 §11.4) ---
    case("visibility_override",
         '<g visibility="hidden">'
         '<rect x="10" y="10" width="80" height="80" fill="red" visibility="visible"/>'
         '</g>',
         [expect_red_at(50, 50)]),
    # --- stroke must not be clipped by an opacity layer bbox ---
    case("stroke_in_opacity_layer",
         '<g opacity="0.5" transform="scale(4)">'
         '<rect x="40" y="40" width="20" height="20" fill="none" '
         'stroke="black" stroke-width="5"/></g>',
         [expect_alpha_at(152, 200, 20, 255), expect_transparent(200, 200)],
         w=400, h=400),
    # --- userSpaceOnUse gradient under a non-uniform CTM ---
    case("user_space_gradient_nonuniform",
         '<defs><linearGradient id="g" gradientUnits="userSpaceOnUse" '
         'x1="0" y1="0" x2="100" y2="100">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient></defs>'
         '<g transform="scale(2,1)">'
         '<rect x="0" y="0" width="100" height="200" fill="url(#g)"/></g>',
         [expect_gradient_t(100, 0, 0.25), expect_gradient_t(0, 100, 0.50)],
         w=200, h=200),
    # --- stop-color inherits from the gradient element (SVG 1.1 §13.2) ---
    case("stop_color_inherit",
         '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0" color="red">'
         '<stop offset="0" stop-opacity="1"/>'
         '<stop offset="1" stop-color="blue"/></linearGradient></defs>'
         '<rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_red_at(10, 50), expect_blue_at(190, 50)],
         w=200, h=100),
]

# Any remaining known conformance gaps would be listed here with an `xfail`
# reason.  There are none: the twelve that used to be xfail now live in
# FIXED_CASES and pass.
CASES = []

# ---------------------------------------------------------------------------
# Positive controls: behaviour that must keep working
# ---------------------------------------------------------------------------

CONTROLS = [
    case("closed_path_fill",
         '<path d="M10,10 L60,10 L60,60 Z" fill="red"/>',
         [expect_red_at(45, 35)]),
    case("polygon_fill",
         '<polygon points="10,10 90,10 50,90" fill="red"/>',
         [expect_red_at(50, 40)]),
    case("clip_inside_outside",
         '<defs><clipPath id="c"><rect x="20" y="20" width="60" height="60"/></clipPath></defs>'
         '<rect x="0" y="0" width="100" height="100" fill="red" clip-path="url(#c)"/>',
         [expect_red_at(50, 50), expect_transparent(10, 10)]),
    case("linear_gradient",
         '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient></defs>'
         '<rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_red_at(10, 50), expect_blue_at(190, 50)],
         w=200, h=100),
    case("group_opacity",
         '<g opacity="0.5"><rect x="0" y="0" width="100" height="100" fill="red"/></g>',
         [expect_alpha_at(50, 50, 108, 148)]),
    case("use_basic",
         '<defs><rect id="r" x="0" y="0" width="40" height="40" fill="red"/></defs>'
         '<use href="#r" x="10" y="10"/>',
         [expect_red_at(15, 15)]),
    case("symbol_use_basic",
         '<defs><symbol id="s"><rect x="0" y="0" width="40" height="40" fill="red"/></symbol></defs>'
         '<use href="#s" x="10" y="10"/>',
         [expect_red_at(15, 15)]),
    case("text_basic",
         '<text x="10" y="60" font-size="48" fill="black">A</text>',
         [expect_count(lambda r, g, b, a: a > 200 and r < 60, 50, 100000, "glyph pixels")]),
    case("style_element_applies",
         '<style>.x { fill: red; }</style>'
         '<rect class="x" x="0" y="0" width="100" height="100" fill="black"/>',
         [expect_rgba(50, 50, (255, 0, 0, 255))]),
    case("inline_style_overrides",
         '<rect x="0" y="0" width="100" height="100" fill="black" '
         'style="fill: lime;"/>',
         [expect_lime_at(50, 50)]),
    case("font_size_percentage_attr",
         '<text x="10" y="50" font-size="150%" fill="red">ABC</text>',
         [expect_count(S.is_red, 1, 100000, "red glyph pixels")]),
    case("stop_inline_style",
         '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0">'
         '<stop offset="0" style="stop-color:red"/>'
         '<stop offset="1" style="stop-color:blue"/></linearGradient></defs>'
         '<rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_red_at(10, 50), expect_blue_at(190, 50)], w=200, h=100),
    case("opacity_inline_style",
         '<rect x="0" y="0" width="100" height="100" fill="red" style="opacity:0.5"/>',
         [expect_alpha_at(50, 50, 108, 148)]),
    case("clip_path_inline_style",
         '<defs><clipPath id="c"><rect x="0" y="0" width="50" height="50"/></clipPath></defs>'
         '<rect x="0" y="0" width="100" height="100" fill="red" '
         'style="clip-path:url(#c)"/>',
         [expect_red_at(25, 25), expect_transparent(75, 75)]),
    # --- regressions for the 12 fixes above, exercised from another angle ---
    # Open subpath with an even-odd hole still opens correctly.
    case("open_subpath_evenodd",
         '<path d="M10,10 L90,10 L90,90 L10,90 M30,30 L70,30 L70,70 L30,70" '
         'fill="red" fill-rule="evenodd"/>',
         [expect_red_at(20, 20), expect_transparent(50, 50)]),
    # Z then an implicit lineto with a *relative* command stays relative to
    # the new subpath's start point, not the old subpath's.
    case("z_then_relative_lineto",
         '<path d="M10,10 L40,10 L40,40 Z 100,10 l30,0 l0,30" fill="red"/>',
         [expect_red_at(35, 25), expect_red_at(125, 25)],
         w=200, h=100),
    # <use> of a <symbol> supplies the use site's fill to a symbol child that
    # did not set one.
    case("use_symbol_inherit",
         '<defs><symbol id="s"><rect x="0" y="0" width="40" height="40"/></symbol></defs>'
         '<use href="#s" x="10" y="10" fill="lime"/>',
         [expect_lime_at(20, 20)]),
    # A <use> of a group containing a nested <use> resolves both levels.
    case("use_nested_use",
         '<defs><rect id="q" x="0" y="0" width="20" height="20" fill="red"/>'
         '<g id="outer"><use href="#q"/></g></defs>'
         '<use href="#outer" x="40" y="40"/>',
         [expect_red_at(50, 50)]),
    # A <use> cycle must terminate (no hang, no crash) and render nothing.
    case("use_cycle",
         '<defs><g id="a"><use href="#b"/></g><g id="b"><use href="#a"/></g></defs>'
         '<use href="#a"/>',
         [expect_transparent(50, 50)]),
    # Deeply nested groups are depth-limited but still render the top ones.
    case("deep_group_chain",
         '<g opacity="0.9"><g opacity="0.9"><rect x="10" y="10" '
         'width="30" height="30" fill="red"/></g></g>',
         [expect_red_at(25, 25)]),
    # Nested <svg> without a viewBox: the viewport is just x/y/w/h.
    case("nested_svg_no_viewbox",
         '<svg x="20" y="20" width="60" height="60">'
         '<rect x="0" y="0" width="60" height="60" fill="red"/></svg>',
         [expect_red_at(50, 50), expect_transparent(10, 10)]),
    # Nested <svg> clips to its viewport.
    case("nested_svg_clips",
         '<svg x="20" y="20" width="40" height="40">'
         '<rect x="-20" y="-20" width="80" height="80" fill="red"/></svg>',
         [expect_red_at(40, 40), expect_transparent(80, 80)]),
    # display:none removes the whole subtree, including children that set
    # their own visibility.
    case("display_none_subtree",
         '<g display="none">'
         '<rect x="0" y="0" width="100" height="100" fill="red" visibility="visible"/>'
         '</g>',
         [expect_transparent(50, 50)]),
    # An opacity layer must not clip the stroke that extends past the fill.
    case("opacity_layer_stroke_bbox",
         '<g opacity="0.75">'
         '<rect x="40" y="40" width="20" height="20" fill="none" '
         'stroke="black" stroke-width="4"/></g>',
         [expect_alpha_at(40, 50, 20, 255)]),
    # userSpaceOnUse radial under a non-uniform CTM stays circular in user
    # space (an ellipse in device space).
    case("user_space_radial_nonuniform",
         '<defs><radialGradient id="g" gradientUnits="userSpaceOnUse" '
         'cx="50" cy="50" r="50">'
         '<stop offset="0" stop-color="red"/>'
         '<stop offset="1" stop-color="blue"/></radialGradient></defs>'
         '<g transform="scale(2,1)">'
         '<rect x="0" y="0" width="100" height="100" fill="url(#g)"/></g>',
         [expect_red_at(100, 50)],
         w=200, h=100),
    # An opacity layer over an existing clip keeps both.
    case("opacity_layer_with_clip",
         '<defs><clipPath id="c"><rect x="0" y="0" width="50" height="50"/>'
         '</clipPath></defs>'
         '<g clip-path="url(#c)"><g opacity="0.5">'
         '<rect x="0" y="0" width="100" height="100" fill="red"/></g></g>',
         [expect_alpha_at(25, 25, 108, 148), expect_transparent(75, 75)]),

    # --- unsupported-feature policy: silently ignored, never an error ---
    # <style> stays opaque: a class rule must not recolour anything.
    case("style_class_applies",
         '<style>.x { fill: lime; }</style>'
         '<rect class="x" x="0" y="0" width="100" height="100"/>',
         [expect_rgba(50, 50, (0, 255, 0, 255))]),
    # <style> with a type attribute is still opaque.
    case("style_type_css",
         '<style type="text/css"><![CDATA['
         'rect { fill: lime; }]]></style>'
         '<rect x="0" y="0" width="100" height="100" fill="black"/>',
         [expect_rgba(50, 50, (0, 255, 0, 255))]),
    # Unknown elements are ignored, their known children still render.
    case("unknown_element_ignored",
         '<foo><rect x="0" y="0" width="100" height="100" fill="red"/></foo>',
         [expect_red_at(50, 50)]),
    # Unknown presentation attributes are ignored, not fatal.
    case("unknown_attribute_ignored",
         '<rect x="0" y="0" width="100" height="100" fill="red" '
         'data-x="1" requiredExtensions="http://example.com/"/>',
         [expect_red_at(50, 50)]),
    # <script> is never executed and never rendered.
    case("script_ignored",
         '<script>alert(1)</script>'
         '<rect x="0" y="0" width="100" height="100" fill="red"/>',
         [expect_red_at(50, 50)]),
    # <foreignObject> and <image> are not supported and render nothing.
    case("foreignobject_image_ignored",
         '<foreignObject x="0" y="0" width="40" height="40">'
         '<div xmlns="http://www.w3.org/1999/xhtml">x</div></foreignObject>'
         '<image x="0" y="0" width="40" height="40" href="x.png"/>'
         '<rect x="50" y="0" width="50" height="100" fill="red"/>',
         [expect_red_at(75, 50), expect_transparent(20, 20)]),
]

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def run(cases):
    failed = []
    xfailed = []
    xpassed = []
    for c in cases:
        name = c["name"]
        try:
            w, h, px = S.render_svg(c["svg"], name=name)
        except S.RenderError as e:
            if c["xfail"]:
                xfailed.append((name, "render: " + str(e)))
                print("XFAIL %-32s render: %s" % (name, str(e)[:100]))
            else:
                failed.append((name, "render: " + str(e)))
                print("FAIL  %-32s render: %s" % (name, str(e)[:100]))
            continue
        bad = []
        for chk in c["checks"]:
            try:
                ok, detail = chk(px, w, h)
            except Exception as e:  # a broken check is a failure, not a crash
                ok, detail = False, "check error: %r" % e
            if not ok:
                bad.append(detail)
        if bad:
            if c["xfail"]:
                xfailed.append((name, "; ".join(bad)))
                print("XFAIL %-32s %s" % (name, "; ".join(bad)[:140]))
            else:
                failed.append((name, "; ".join(bad)))
                print("FAIL  %-32s %s" % (name, "; ".join(bad)[:140]))
        else:
            if c["xfail"]:
                xpassed.append((name, c["xfail"]))
                print("XPASS %-32s (fixed? remove xfail: %s)" % (name, c["xfail"]))
            else:
                print("ok    %s" % name)
    return failed, xfailed, xpassed


def main():
    print("== fixed conformance cases (%d) ==" % len(FIXED_CASES))
    ff, _, _ = run(FIXED_CASES)
    print("\n== positive controls (%d) ==" % len(CONTROLS))
    cf, _, _ = run(CONTROLS)
    print("\n== known conformance gaps (%d, xfail) ==" % len(CASES))
    f, xf, xp = run(CASES)
    failed = ff + cf + f
    print("\n%d fixed, %d controls, %d gap cases: %d unexpected failure(s), "
          "%d xfail, %d xpass" % (len(FIXED_CASES), len(CONTROLS), len(CASES),
                                   len(failed), len(xf), len(xp)))
    if xp:
        print("\nXPASS (fixed bugs -- remove the xfail markers):")
        for name, reason in xp:
            print("  %s: %s" % (name, reason))
    if failed:
        print("\nUnexpected failures:")
        for name, detail in failed:
            print("  %s: %s" % (name, detail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
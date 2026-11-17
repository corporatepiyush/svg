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

def case(name, body, checks, w=100, h=100, xfail=None, viewbox=None):
    if viewbox is None:
        svg = S.svg_doc(body, w, h)
    else:
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="%s">%s</svg>' % (w, h, viewbox, body))
    return {"name": name, "svg": svg, "checks": checks, "xfail": xfail}


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


def expect_ink_left(lo, hi):
    def check(px, w, h):
        b = S.bbox_alpha(px, w, h)
        if b is None:
            return False, "no ink"
        return lo <= b[0] <= hi, "ink left=%d want [%d,%d] (bbox %r)" % (b[0], lo, hi, b)
    return check


def expect_ink_right(lo, hi):
    def check(px, w, h):
        b = S.bbox_alpha(px, w, h)
        if b is None:
            return False, "no ink"
        return lo <= b[2] - 1 <= hi, "ink right=%d want [%d,%d] (bbox %r)" % (b[2] - 1, lo, hi, b)
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
    # --- stop-color is not inherited from the gradient element; its initial
    # value is black (SVG 1.1 §13.2.4).  `color` only feeds currentColor. ---
    case("stop_color_not_inherited",
         '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0" color="red">'
         '<stop offset="0" stop-opacity="1"/>'
         '<stop offset="1" stop-color="blue"/></linearGradient></defs>'
         '<rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_rgba(10, 50, (0, 0, 0, 255)), expect_blue_at(190, 50)],
         w=200, h=100),
    # --- VIEW-01: percentages use the viewport in user units, not pixels ---
    case("pct_viewbox",
         '<rect x="0" y="0" width="50%" height="50%" fill="red"/>',
         [expect_red_at(25, 25), expect_transparent(75, 75)],
         viewbox="0 0 50 50"),
    # --- VIEW-02: nested viewport percentages use the nested viewBox ---
    case("nested_pct_viewport",
         '<svg x="0" y="0" width="100" height="100" viewBox="0 0 200 200">'
         '<rect x="0" y="0" width="50%" height="50%" fill="red"/></svg>',
         [expect_red_at(25, 25), expect_transparent(75, 75)]),
    # --- SHAPE-01: negative rect dimensions disable rendering ---
    case("rect_negative_width",
         '<rect x="10" y="10" width="-50" height="80" fill="red"/>',
         [expect_transparent(20, 50), expect_transparent(50, 50)]),
    case("rect_negative_height",
         '<rect x="10" y="10" width="80" height="-50" fill="red"/>',
         [expect_transparent(50, 20), expect_transparent(50, 50)]),
    # --- SHAPE-02: an explicit zero corner radius stays zero ---
    case("rect_rx0_ry30",
         '<rect x="10" y="10" width="80" height="80" rx="0" ry="30" fill="red"/>',
         [expect_red_at(12, 12), expect_red_at(50, 50), expect_transparent(5, 5)]),
    case("rect_rx30_ry0",
         '<rect x="10" y="10" width="80" height="80" rx="30" ry="0" fill="blue"/>',
         [expect_blue_at(12, 12), expect_blue_at(50, 50), expect_transparent(5, 5)]),
    # --- STRUCT-04: a zero-sized nested viewport disables rendering ---
    case("nested_svg_zero",
         '<svg x="0" y="0" width="0" height="0">'
         '<rect x="0" y="0" width="100" height="100" fill="red"/></svg>',
         [expect_transparent(50, 50)]),
    # --- STYLE-01: an invalid paint value is ignored, so fill inherits ---
    case("invalid_fill_inherits",
         '<g fill="red"><rect x="0" y="0" width="100" height="100" fill="bogus"/></g>',
         [expect_red_at(50, 50)]),
    # --- STYLE-02: the CSS-wide inherit keyword ---
    case("fill_inherit_keyword",
         '<g fill="red"><rect x="0" y="0" width="100" height="100" fill="inherit"/></g>',
         [expect_red_at(50, 50)]),
    case("stroke_inherit_keyword",
         '<g stroke="red" stroke-width="10" fill="none">'
         '<line x1="10" y1="50" x2="90" y2="50" stroke="inherit"/></g>',
         [expect_red_at(50, 50)]),
    # --- STYLE-03: a later declaration wins over an earlier display:none ---
    case("display_reenable_inline",
         '<rect x="0" y="0" width="100" height="100" fill="red" '
         'display="none" style="display:inline"/>',
         [expect_red_at(50, 50)]),
    # --- STROKE-01: a zero-sum dasharray behaves like solid ---
    case("dasharray_zero_sum",
         '<line x1="10" y1="50" x2="90" y2="50" stroke="red" '
         'stroke-width="10" stroke-dasharray="0 0"/>',
         [expect_red_at(50, 50)]),
    # --- STROKE-02/03: negative lengths are invalid and ignored ---
    case("negative_stroke_width",
         '<g stroke="red" stroke-width="10"><line x1="10" y1="50" x2="90" y2="50" '
         'stroke-width="-5"/></g>',
         [expect_red_at(50, 50)]),
    case("negative_dasharray",
         '<line x1="10" y1="50" x2="90" y2="50" stroke="red" '
         'stroke-width="10" stroke-dasharray="-5"/>',
         [expect_red_at(50, 50)]),
    # --- STYLE-07: currentColor resolves against the gradient's color ---
    case("stop_currentcolor",
         '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0" color="red">'
         '<stop offset="0" stop-color="currentColor"/>'
         '<stop offset="1" stop-color="blue"/></linearGradient></defs>'
         '<rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_red_at(10, 50), expect_blue_at(190, 50)],
         w=200, h=100),
    # --- PAINT-01: href inherits only attributes not defined locally ---
    case("gradient_href_local_coords",
         '<defs>'
         '<linearGradient id="base" x1="0" y1="0" x2="1" y2="0">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient>'
         '<linearGradient id="g" href="#base" x1="0" y1="0" x2="0" y2="1"/>'
         '</defs><rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_red_at(190, 10), expect_blue_at(10, 90)],
         w=200, h=100),
    case("gradient_href_local_stops",
         '<defs>'
         '<linearGradient id="base" x1="0" y1="0" x2="1" y2="0">'
         '<stop offset="0" stop-color="green"/><stop offset="1" stop-color="yellow"/>'
         '</linearGradient>'
         '<linearGradient id="g" href="#base">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient></defs>'
         '<rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_red_at(10, 50), expect_blue_at(190, 50)],
         w=200, h=100),
    # --- PATH-01: after Z, L starts a new subpath at the closed start ---
    case("z_then_lineto",
         '<path d="M10 10 L50 10 Z L90 90" fill="none" stroke="red" '
         'stroke-width="4"/>',
         [expect_red_at(30, 30), expect_red_at(50, 50),
          expect_transparent(70, 40), expect_transparent(70, 30)]),
    # --- PATH-02: pathLength applies to basic shapes too ---
    case("pathlength_rect_dash",
         '<rect x="10" y="40" width="80" height="20" fill="none" stroke="red" '
         'stroke-width="6" stroke-dasharray="10 10" pathLength="20"/>',
         [expect_red_at(42, 40), expect_red_at(45, 40)]),
    # --- PAINT-02: an inherited patternTransform rotates the tiling ---
    case("pattern_href_transform",
         '<defs>'
         '<pattern id="base" width="20" height="20" patternUnits="userSpaceOnUse" '
         'patternTransform="translate(10,0)">'
         '<rect width="20" height="20" fill="#ff0000"/>'
         '<rect width="10" height="20" fill="#0000ff"/></pattern>'
         '<pattern id="p" href="#base"/></defs>'
         '<rect width="100" height="40" fill="url(#p)"/>',
         [expect_red_at(5, 10), expect_blue_at(15, 10),
          expect_red_at(25, 10), expect_blue_at(35, 10)],
         w=100, h=40),
    # --- PAINT-02: an inherited empty pattern uses the referenced children ---
    case("pattern_href_children",
         '<defs>'
         '<pattern id="base" width="20" height="20" patternUnits="userSpaceOnUse">'
         '<rect width="20" height="20" fill="red"/></pattern>'
         '<pattern id="p" href="#base"/></defs>'
         '<rect width="100" height="100" fill="url(#p)"/>',
         [expect_red_at(5, 5), expect_red_at(25, 5)]),
    # --- PAINT-02: local children replace the referenced pattern content ---
    case("pattern_href_local_children",
         '<defs>'
         '<pattern id="base" width="20" height="20" patternUnits="userSpaceOnUse">'
         '<rect width="20" height="20" fill="red"/></pattern>'
         '<pattern id="p" href="#base">'
         '<rect width="10" height="10" fill="blue"/></pattern></defs>'
         '<rect width="100" height="100" fill="url(#p)"/>',
         [expect_blue_at(5, 5), expect_transparent(15, 5)]),
    # --- PAINT-02: inherited percentages are reinterpreted under the local
    # patternUnits (50% of the viewport is 100 user units, but 0.5 of the
    # bounding box is 50 user units). ---
    case("pattern_href_units_pct",
         '<defs>'
         '<pattern id="base" patternUnits="userSpaceOnUse" '
         'width="50%" height="50%">'
         '<rect width="100" height="100" fill="red"/>'
         '<rect width="10" height="10" fill="blue"/></pattern>'
         '<pattern id="p" href="#base" patternUnits="objectBoundingBox"/></defs>'
         '<rect width="100" height="100" fill="url(#p)"/>',
         [expect_blue_at(5, 5), expect_blue_at(55, 5), expect_red_at(30, 5)],
         w=200, h=100),
    # --- PAINT-01: an inherited gradientTransform orients the gradient ---
    case("gradient_href_transform",
         '<defs>'
         '<linearGradient id="base" gradientUnits="userSpaceOnUse" '
         'x1="10" y1="50" x2="90" y2="50" '
         'gradientTransform="rotate(90 50 50)">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient>'
         '<linearGradient id="g" href="#base"/></defs>'
         '<rect width="100" height="100" fill="url(#g)"/>',
         [expect_red_at(50, 10), expect_blue_at(50, 90)]),
    # --- PAINT-01: a radialGradient may reference a linearGradient for the
    # common attributes and stops; type-specific coordinates are not copied. ---
    case("gradient_href_cross_type",
         '<defs>'
         '<linearGradient id="base" x1="0" y1="0" x2="0" y2="1">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient>'
         '<radialGradient id="g" href="#base" gradientUnits="userSpaceOnUse" '
         'cx="50" cy="50" r="40"/></defs>'
         '<rect width="100" height="100" fill="url(#g)"/>',
         [expect_red_at(50, 50), expect_blue_at(85, 50)]),
    # --- PAINT-01: gradientUnits override reinterprets inherited percentages.
    # The referencing gradient uses objectBoundingBox, so the inherited 50%
    # endpoint is 50 user units, not 200. ---
    case("gradient_href_units",
         '<defs>'
         '<linearGradient id="base" gradientUnits="userSpaceOnUse" '
         'x1="0" y1="0" x2="50%" y2="0">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient>'
         '<linearGradient id="g" href="#base" gradientUnits="objectBoundingBox"/>'
         '</defs>'
         '<rect x="0" width="100" height="100" fill="url(#g)"/>'
         '<rect x="200" width="100" height="100" fill="url(#base)"/>',
         [expect_gradient_t(10, 50, 0.20, 0.05),
          expect_blue_at(90, 50),
          expect_blue_at(210, 50), expect_blue_at(290, 50),
          expect_transparent(390, 50)],
         w=400, h=100),
    # --- PAINT-01: href cycles are rejected, local attributes still apply. ---
    case("gradient_href_cycle",
         '<defs>'
         '<linearGradient id="a" href="#b" x1="0" y1="0" x2="1" y2="0">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient>'
         '<linearGradient id="b" href="#a"/></defs>'
         '<rect width="100" height="100" fill="url(#a)"/>',
         [expect_red_at(10, 50), expect_blue_at(90, 50)]),
    # --- PAINT-01: fx/fy specified on the referenced radial gradient are
    # inherited; an unspecified fx/fy follows cx/cy. ---
    case("gradient_href_focal",
         '<defs>'
         '<radialGradient id="base" gradientUnits="userSpaceOnUse" '
         'cx="50" cy="50" r="50" fx="20" fy="50">'
         '<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>'
         '</radialGradient>'
         '<radialGradient id="g" href="#base" cx="50" cy="50" r="50"/></defs>'
         '<rect width="100" height="100" fill="url(#g)"/>',
         [expect_red_at(20, 50), expect_blue_at(90, 50)]),
    # --- STYLE-04: CSS rules reach tspan and stop elements. ---
    case("css_tspan_fill",
         '<style>tspan { fill: red; }</style>'
         '<text x="10" y="70" font-size="40" fill="black"><tspan>A</tspan></text>',
         [expect_count(S.is_red, 20, 100000, "red glyph pixels")]),
    case("css_stop_color",
         '<style>.c { stop-color: red; }</style>'
         '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0">'
         '<stop class="c" offset="0"/><stop offset="1" stop-color="blue"/>'
         '</linearGradient></defs>'
         '<rect x="0" y="0" width="200" height="100" fill="url(#g)"/>',
         [expect_red_at(10, 50), expect_blue_at(190, 50)],
         w=200, h=100),
    # --- STYLE-05/06: attribute selectors and structural pseudo-classes. ---
    case("css_attribute_selector",
         '<style>rect[fill="blue"] { fill: red; }</style>'
         '<rect x="0" y="0" width="100" height="100" fill="blue"/>',
         [expect_red_at(50, 50)]),
    case("css_first_child",
         '<style>rect:first-child { fill: red; }</style>'
         '<g><rect x="0" y="0" width="100" height="100" fill="blue"/></g>',
         [expect_red_at(50, 50)]),
    # --- TEXT-01: a list of x coordinates positions successive glyphs ---
    case("text_x_list",
         '<text x="10 70 130" y="70" font-size="30" fill="red">ABCD</text>',
         [expect_ink_right(180, 199)], w=200, h=100),
    # --- TEXT-02: x and dx both apply (absolute, then relative shift) ---
    case("tspan_x_dx",
         '<text x="10" y="70" font-size="40" fill="red">'
         '<tspan x="10" dx="30">A</tspan></text>',
         [expect_ink_left(35, 45)]),
    # --- TEXT-06: startOffset percentages are percentages of the path ---
    case("textpath_startoffset_pct",
         '<defs><path id="p" d="M10 50 L90 50"/></defs>'
         '<text x="0" y="60" font-size="30" fill="red">'
         '<textPath href="#p" startOffset="50%">A</textPath></text>',
         [expect_ink_left(45, 55)]),
    # --- TEXT-07: stroke properties apply to glyphs ---
    case("text_stroke",
         '<text x="10" y="70" font-size="60" fill="none" stroke="red" '
         'stroke-width="4">A</text>',
         [expect_count(S.is_red, 20, 100000, "red glyph pixels")]),
    # --- STRUCT-01: a symbol's generated viewport defaults to 100% ---
    case("use_symbol_default_size",
         '<defs><symbol id="s" viewBox="0 0 10 10">'
         '<rect x="0" y="0" width="10" height="10" fill="red"/></symbol></defs>'
         '<use href="#s"/>',
         [expect_red_at(100, 50), expect_transparent(10, 50)],
         w=200, h=100),
    # --- STRUCT-02: width/height on <use> override the referenced <svg> ---
    case("use_svg_width_height",
         '<defs><svg id="s" x="0" y="0" width="20" height="20" viewBox="0 0 10 10">'
         '<rect x="0" y="0" width="10" height="10" fill="red"/></svg></defs>'
         '<use href="#s" x="10" y="10" width="80" height="80"/>',
         [expect_red_at(50, 50), expect_transparent(95, 95)]),
    # --- regression: the referenced <svg> keeps its own width/height when the
    # use site does not supply one (SVG 1.1 §5.6). ---
    case("use_svg_own_size",
         '<defs><svg id="s" x="0" y="0" width="20" height="20" viewBox="0 0 10 10">'
         '<rect x="0" y="0" width="10" height="10" fill="red"/></svg></defs>'
         '<use href="#s" x="10" y="10"/>',
         [expect_red_at(15, 15), expect_transparent(40, 40)],
         w=100, h=100),
    # --- textPath: an absolute startOffset is in user units, not a fraction.
    # The path runs 10..90, so 50 lands the glyph at x=60. ---
    case("textpath_startoffset_abs",
         '<defs><path id="p" d="M10 50 L90 50"/></defs>'
         '<text x="0" y="60" font-size="30" fill="red">'
         '<textPath href="#p" startOffset="50">A</textPath></text>',
         [expect_ink_left(55, 65)]),
    # --- TEXT-03: whitespace collapsing under xml:space="default" (§5.10.2). A
    # whitespace-only tspan between two glyphs is removed, so "A<tspan> </tspan>B"
    # renders as "AB" (ink ends near x=71 at font-size 40). ---
    case("text_whitespace_collapse",
         '<text x="10" y="70" font-size="40" fill="red">A<tspan> </tspan>B</text>',
         [expect_ink_right(65, 75)], w=220, h=100),
    # --- TEXT-03: xml:space="preserve" keeps whitespace-only nodes.  Here the
    # preserved spaces render as a visible three-space gap between A and B.
    # (The bitmap font does not advance the pen for a space-only chunk, so
    # the *gap* is asserted via ink columns rather than the bbox.) ---
    case("text_whitespace_preserve",
         '<text x="10" y="70" font-size="40" fill="red">A'
         '<tspan xml:space="preserve">   </tspan>B</text>',
         [expect_ink_right(65, 80)], w=220, h=100),
    # --- TEXT-05: a rotate list repeats its last value for extra glyphs. ---
    case("text_rotate_list",
         '<text x="10" y="70" font-size="30" fill="red" rotate="0 45 90">'
         'ABCDE</text>',
         [expect_ink_right(140, 160)], w=220, h=100),
    # --- TEXT-04: textLength calibrates the whole text element once. ---
    case("text_textlength",
         '<text x="10" y="70" font-size="30" fill="red" textLength="150">'
         'ABC</text>',
         [expect_ink_right(125, 140)], w=220, h=100),
    # --- TEXT-04: lengthAdjust="spacingAndGlyphs" stretches the glyphs too,
    # so a shorter textLength compresses them instead of only the spacing. ---
    case("text_textlength_glyphs",
         '<text x="10" y="70" font-size="30" fill="red" textLength="60" '
         'lengthAdjust="spacingAndGlyphs">ABC</text>',
         [expect_ink_right(64, 74)]),
]

# Remaining known conformance gaps from ISSUES.md.  Each is a spec-derived
# assertion that fails today; remove the xfail marker once fixed.
CASES = [
]

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
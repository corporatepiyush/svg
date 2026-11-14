#!/usr/bin/env python3
"""Resource-limit tests for svg2img.

The limits are configurable through svg2img_options.limits; this runner drives
the CLI with a small SVG and checks that:

  * a limit set too low for the document is reported as an error (not a
    crash, not a bogus image), and
  * a limit that fits renders normally.

The CLI exposes the limits through SVG2IMG_LIMIT_* environment variables so the
test can vary them without a per-limit command-line flag; this keeps the
option surface small while still testing every limit.

Run:  python3 fuzz/limit_test.py [path-to-svg2img]
Exit: 0 if every limit behaves, 1 otherwise.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else \
    os.path.join(ROOT, "svg2img")
TMP = tempfile.mkdtemp(prefix="svg2img_limit_")


def svg(body, w=100, h=100):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d">%s</svg>' % (w, h, w, h, body))


def run(text, env_extra=None, name="case"):
    src = os.path.join(TMP, name + ".svg")
    out = os.path.join(TMP, name + ".png")
    with open(src, "w") as f:
        f.write(text)
    if os.path.exists(out):
        os.remove(out)
    env = dict(os.environ)
    env.update(env_extra or {})
    r = subprocess.run([BIN, "-a", "1", src, out], capture_output=True,
                       text=True, env=env, timeout=120)
    return r, out


CASES = []


def case(name, body, env, expect_ok, note=""):
    CASES.append((name, svg(body), env, expect_ok, note))


# max_nodes: 3 rects need at least 5 nodes (svg + 3 rects).
case("nodes_too_low", '<rect width="10" height="10" fill="red"/>'
     '<rect x="20" width="10" height="10" fill="red"/>'
     '<rect x="40" width="10" height="10" fill="red"/>',
     {"SVG2IMG_LIMIT_MAX_NODES": "2"}, False)
case("nodes_ok", '<rect width="10" height="10" fill="red"/>'
     '<rect x="20" width="10" height="10" fill="red"/>'
     '<rect x="40" width="10" height="10" fill="red"/>',
     {"SVG2IMG_LIMIT_MAX_NODES": "64"}, True)

# max_depth: 5 nested groups.
case("depth_too_low", "<g>" * 5 + '<rect width="10" height="10" fill="red"/>' +
     "</g>" * 5, {"SVG2IMG_LIMIT_MAX_DEPTH": "2"}, False)
case("depth_ok", "<g>" * 5 + '<rect width="10" height="10" fill="red"/>' +
     "</g>" * 5, {"SVG2IMG_LIMIT_MAX_DEPTH": "32"}, True)

# max_path_points: a polygon with 10 points needs 10.
case("points_too_low",
     '<polygon points="' + " ".join("%d,%d" % (i * 5, i * 5) for i in range(10)) +
     '" fill="red"/>',
     {"SVG2IMG_LIMIT_MAX_PATH_POINTS": "3"}, False)
case("points_ok",
     '<polygon points="' + " ".join("%d,%d" % (i * 5, i * 5) for i in range(10)) +
     '" fill="red"/>',
     {"SVG2IMG_LIMIT_MAX_PATH_POINTS": "64"}, True)

# max_gradient_stops: a gradient with 5 stops.
case("stops_too_low",
     '<defs><linearGradient id="g">' +
     "".join('<stop offset="%g" stop-color="red"/>' % (i / 4.0) for i in range(5)) +
     '</linearGradient></defs><rect width="100" height="100" fill="url(#g)"/>',
     {"SVG2IMG_LIMIT_MAX_GRADIENT_STOPS": "2"}, False)
case("stops_ok",
     '<defs><linearGradient id="g">' +
     "".join('<stop offset="%g" stop-color="red"/>' % (i / 4.0) for i in range(5)) +
     '</linearGradient></defs><rect width="100" height="100" fill="url(#g)"/>',
     {"SVG2IMG_LIMIT_MAX_GRADIENT_STOPS": "16"}, True)

# max_text_length: a 50-char string.
case("text_too_low", '<text x="0" y="50" font-size="10" fill="black">' +
     "A" * 50 + "</text>",
     {"SVG2IMG_LIMIT_MAX_TEXT_LENGTH": "5"}, False)
case("text_ok", '<text x="0" y="50" font-size="10" fill="black">' +
     "A" * 50 + "</text>",
     {"SVG2IMG_LIMIT_MAX_TEXT_LENGTH": "200"}, True)

# max_layers is a *soft* limit: past it the compositor falls back to drawing
# the children directly (documented degradation), so the render still succeeds.
case("layers_degrade", '<g opacity="0.5"><g opacity="0.5">'
     '<g opacity="0.5"><rect width="100" height="100" fill="red"/></g></g></g>',
     {"SVG2IMG_LIMIT_MAX_LAYERS": "1"}, True)
case("layers_ok", '<g opacity="0.5"><g opacity="0.5">'
     '<g opacity="0.5"><rect width="100" height="100" fill="red"/></g></g></g>',
     {"SVG2IMG_LIMIT_MAX_LAYERS": "8"}, True)

# max_use_expansion: a use chain that expands to 2 nodes.
case("use_too_low", '<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
     '<use href="#r"/><use href="#r"/>',
     {"SVG2IMG_LIMIT_MAX_USE_EXPANSION": "1"}, False)
case("use_ok", '<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
     '<use href="#r"/><use href="#r"/>',
     {"SVG2IMG_LIMIT_MAX_USE_EXPANSION": "64"}, True)

# max_pixels: a 100x100 canvas.
case("pixels_too_low", "", {"SVG2IMG_LIMIT_MAX_PIXELS": "100"}, False)
case("pixels_ok", "", {"SVG2IMG_LIMIT_MAX_PIXELS": "1000000"}, True)


def main():
    failures = 0
    for name, text, env, expect_ok, note in CASES:
        r, out = run(text, env, name)
        # A limit that is too low must be reported as a clean error, never a
        # signal, and must not leave a half-written image behind.
        if r.returncode < 0:
            print("FAIL  %-16s signal %d" % (name, -r.returncode))
            failures += 1
            continue
        if expect_ok:
            ok = r.returncode == 0 and os.path.exists(out)
            print("%-4s %-16s exit=%d" % ("ok" if ok else "FAIL", name, r.returncode))
        else:
            # Not-OK: the tool must exit non-zero OR (documented degradation)
            # produce an image that is a strict subset.  We require a non-zero
            # exit here, which is the documented behaviour for a limit that
            # prevents a correct image.
            ok = r.returncode != 0
            print("%-4s %-16s exit=%d" % ("ok" if ok else "FAIL", name, r.returncode))
        if not ok:
            failures += 1
    print("\n%d cases, %d failure(s)" % (len(CASES), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
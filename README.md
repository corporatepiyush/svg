# svg2img

A pure **C17** SVG → PNG rasterizer with **SVG-only scope**: no HTML, no CSS
engine, no external dependencies beyond libc and libm.  It ships its own XML
parser, scanline rasterizer, and PNG encoder with a hand-written DEFLATE
compressor — there is no libxml, cairo, libpng, or build system beyond `make`.

```
$ ./svg2img icon.svg icon.png
$ ./svg2img -w 1920 -a 8 -b FFFFFFFF icon.svg icon@2x.png
```

```sh
$ make -j"$(nproc)"      # parallel build
$ make check             # 40 pixel-level regression checks
$ make strict            # -Werror + conversion/shadow/... warnings
```

## Scope

This is intentionally **not** a browser.  It covers a useful SVG profile for
flat vector art, icons, charts, and simple documents.

| Area | Supported |
|---|---|
| XML | elements, attributes, text, comments, CDATA, DOCTYPE, processing instructions, entities |
| Shapes | `rect` (`rx`/`ry`), `circle`, `ellipse`, `line`, `polyline`, `polygon`, `path` |
| Paths | all commands `M m L l H h V v C c S s Q q T t A a Z z`; arcs → cubics; adaptive flattening |
| Structure | `g`, `a`, `defs`, `symbol`, `use` (basic `x`/`y`/`transform`) |
| Paint | solid colours, `linearGradient`, `radialGradient` (focal point), `stop`, `spreadMethod` |
| Clipping | `clipPath` with `userSpaceOnUse` / `objectBoundingBox`, `clip-rule`, `fill-rule` |
| Transforms | `matrix`, `translate`, `scale`, `rotate` (with `cx`/`cy`), `skewX`, `skewY` |
| Viewport | `viewBox` + `preserveAspectRatio` (meet/slice, all nine alignments) |
| Units | user units, `px`, `pt`, `pc`, `in`, `cm`, `mm`, `em`, `ex`, `%` |
| Styling | presentation attributes and inline `style="..."` declarations |
| Colour | `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`, `hsl()`, `hsla()`, `transparent`, `currentColor`, 147 named colours |
| Stroke | caps, joins, miter limit, `stroke-dasharray` / `stroke-dashoffset` (odd lists) |
| Text | built-in 5×7 bitmap font; bold/italic, anchors, letter-spacing |
| Output | RGBA8 PNG, 1×–8× supersampling, optional background |

### SVG-only styling

There is **no CSS engine**:

- `<style>` elements are parsed as ordinary XML and ignored.  Their contents
  are opaque text; they never affect rendering.
- There are no CSS selectors, at-rules, specificity, or cascade.
- Presentation attributes (`fill="red"`) and inline `style="fill:red"` are
  supported.  Inline style is applied last, so it overrides presentation
  attributes.

Properties handled by the declaration parser:

```
fill, stroke, stroke-width, stroke-linecap, stroke-linejoin,
stroke-miterlimit, stroke-dasharray, stroke-dashoffset,
fill-rule, clip-rule, fill-opacity, stroke-opacity,
color, font-size, font-weight, font-style, text-anchor,
letter-spacing, display, visibility, opacity,
stop-color, stop-opacity, clip-path
```

`opacity`, `clip-path`, and `stop-color`/`stop-opacity` work both as
presentation attributes and as inline style.  `font-size` percentages resolve
against the parent font size.  `currentColor` resolves against `color`.

Geometry properties via CSS (`x`, `y`, `width`, `height`, `r`, `rx`, `ry`,
`cx`, `cy`, `d`, `points`, `transform` in `style=`) are **not** supported;
use the XML attributes.

## CLI

```
-w, --width N        output width  in px (0 = auto)
-h, --height N       output height in px (0 = auto)
-s, --scale F        extra uniform scale (default 1.0)
-a, --aa N           supersampling 1..8 (default 4)
-b, --background C   background RRGGBB or RRGGBBAA
-v, --verbose        progress on stderr
    --help           usage
```

## Library API

```c
#include "svg2img.h"

svg2img_options opts = {0};
opts.width  = 800;               /* 0 = derive from the SVG       */
opts.height = 0;
opts.scale  = 1.0f;
opts.aa     = 4;                 /* supersampling 1..8            */
opts.background = 0xFFFFFFFF;    /* 0xRRGGBBAA                    */
opts.has_background = 1;
opts.verbose = 0;

int rc = svg2img_convert(svg_text, svg_len, "out.png", &opts);
```

`svg2img_convert_file(in_path, out_path, &opts)` is a convenience wrapper.
Both return `0` on success and non-zero on failure.  The renderer keeps no
global state on the render path; the only module-level mutable data is the
lazily initialised CRC table in the PNG writer.

## Build

```sh
make                 # parallel object build (uses all cores)
make -j8             # explicit parallelism
make check           # 40 pixel checks
make test            # render the sample corpus to /tmp
make strict          # strict -Werror build of every translation unit
make clean           # remove objects, dependency files and binary
```

Objects are compiled independently with `-MMD -MP`, so a header change only
rebuilds the translation units that include it.  The source tree is split into
small modules (see below); a clean build is ~0.6 s wall / ~3.5 s user on an
8-core Apple Silicon machine.

Requirements: a C17 compiler and libm.  The strict build is warning-clean under:

```
-std=c17 -O2 -Wall -Wextra -pedantic -Werror -Wconversion -Wsign-conversion
-Wshadow -Wcast-qual -Wmissing-prototypes -Wold-style-definition
-Wredundant-decls -Wwrite-strings
```

## Architecture

```
XML bytes
  -> xml_parse.c          DOM
  -> svg_build.c          render tree (style, geometry, text)
  -> svg_refs.c           paint / clip / use references
  -> svg_viewport.c       width/height/viewBox/preserveAspectRatio
  -> raster_*.c           scanline fill, gradients, strokes, clips, layers
  -> png_*.c              DEFLATE + PNG container
```

## Layout

| File | Purpose |
|---|---|
| `svg2img.h` | Public API |
| `internal.h` | Shared types, math, allocation helpers |
| `cli.h`, `cli.c` | CLI option parsing |
| `main.c` | CLI driver |
| `xml_parse.c` | Lenient XML → DOM parser |
| `xml_dom.c` | Iterative DOM teardown and lookup |
| `path_parse.c` | Path parser, arc → cubic, adaptive flattening |
| `path_util.c` | Path copy/transform/bbox/free |
| `color.c` | SVG colour parsing |
| `length.c` | CSS/SVG length parsing |
| `svg_idmap.c` | id → node hash map |
| `svg_transform.c` | transform-list parsing |
| `svg_geometry.c` | primitive-shape geometry |
| `svg_gradient.c` | gradient definitions and stops |
| `svg_text.c` | text-node content collection |
| `svg_style.c` | style defaults, inline `style` declarations |
| `svg_build.c` | DOM → render tree |
| `svg_refs.c` | paint/clip/`use` reference resolution |
| `svg_viewport.c` | viewport and aspect-ratio transform |
| `svg_api.c` | public entry points and teardown |
| `raster_util.c` | safe float→int conversion |
| `raster_canvas.c` | canvas allocation and source-over blend |
| `raster_blend.c` | SIMD span blending and layer compositing |
| `raster_mask.c` | 8-bit alpha clip masks |
| `raster_gradient.c` | gradient LUTs and evaluation |
| `raster_fill.c` | scanline polygon fill |
| `raster_stroke.c` | stroke outlines and dashes |
| `raster_shape.c` | shape fill/stroke dispatch |
| `raster_clip.c` | clipPath geometry, masks, nested clips |
| `raster_text.c` | bitmap-font text rendering |
| `raster_node.c` | node bounds, opacity layers, tree traversal |
| `png_deflate.c` | DEFLATE compressor (fixed Huffman + LZ77) |
| `png_write.c` | PNG container, checksums, quantisation |
| `font.c` | built-in 5×7 bitmap font |
| `test/` | sample corpus, pixel checks, strict-build helper |
| `fuzz/` | crash, grammar, semantic, resource and leak harnesses |

Private headers: `svg_internal.h`, `raster_internal.h`, `png_internal.h`.

## Rendering model

- **Straight-alpha float canvas.** RGBA float, non-premultiplied; the PNG
  writer quantises to 8-bit at the end.
- **Scanline coverage.** Edges are bucketed by sub-scanline; the crossing walk
  emits horizontal runs.  Supersampling is in Y (1×–8×); horizontal edges are
  widened to whole pixels.  This is fast and robust, but near-vertical edges
  are not 2D anti-aliased like a browser.
- **Gradients.** A 1024-entry LUT is baked per fill; linear and radial (with
  focal point) are evaluated per pixel with pad/reflect/repeat.
- **Strokes.** Outline construction with caps, joins, miter limit, and dashed
  patterns.  Closed contours use two opposite-wound loops.
- **Opacity.** Group opacity uses an isolated layer composited once; chains of
  single-child groups are collapsed.
- **Clipping.** `clipPath` is rasterised to an 8-bit alpha mask and
  intersected with inherited clips.
- **SIMD.** NEON (with SSE2 and scalar fallbacks) blends 4 pixels per
  iteration.

## Correctness and testing

| Suite | What it checks |
|---|---|
| `test/check.py` | 40 exact RGBA checks over the sample corpus; PNG CRC/IHDR/IDAT validation |
| `fuzz/regress.py` | 22 saved regression cases with pixel expectations |
| `fuzz/difftest.py` | renderer invariants: fill rules, gradients, clips, opacity, transforms, viewBox |
| `fuzz/semantic_cases.py` | pixel-oracle tests: 14 controls + 12 `xfail` conformance gaps |
| `fuzz/grammar_fuzz.py` | XML/reference/numeric grammar: DOCTYPE, CDATA, entities, use/symbol graphs, extreme numbers |
| `fuzz/featurfuzz.py` | structure-aware feature documents (paths, gradients, dashes, use graphs) |
| `fuzz/rawfuzz.py` | raw-byte / malformed-markup robustness |
| `fuzz/resource_fuzz.py` | timeouts and unbounded work: use chains, dashes, wide/deep trees, huge text, large canvases |
| `fuzz/leak_check.py` | leak regression under macOS `leaks` or Linux valgrind |
| `make strict` | warning-clean build of every module |
| `fuzz/docker/` | linux/amd64 strict build, pixel suite, regressions, invariants, semantic, resource, leak, sanitizers |

Sanitizers and limits:

- ASan + UBSan fork-per-input runner with a 60 s per-input `SIGALRM`
  (`SVG2IMG_FUZZ_TIMEOUT=N` overrides; `TIMEOUT`); clean over the seed, feature, grammar, and regression corpora.
- Fuzz runs are process-parallel: `fuzz/run_parallel.sh` defaults to the
  online CPU count (`JOBS=0`), and `feature`, `grammar`, `run`, `gen-run`,
  `raw-run`, `campaign`, `big`, and the Docker cross-test all use it.
- DOM depth capped at 1024; output capped at 32768 px/side and 256 Mpx.
- Non-finite coordinates are clamped before float→int conversion.
- Allocation failures are checked at the render-path allocation sites; the
  fuzzers still have an allocation-failure injection gap (see below).

### Semantic oracle

`fuzz/semantic_cases.py` is the conformance TODO.  It renders small documents
and asserts pixels.  Cases marked `xfail` are confirmed bugs; the runner
prints `XPASS` when one is fixed, so remove the marker then.

```sh
python3 fuzz/semantic_cases.py ./svg2img
14 controls, 12 gap cases: 0 unexpected failure(s), 12 xfail
```

Known SVG gaps currently tracked:

1. Open subpaths are not filled as if closed (`polyline`/open `path`).
2. `Z` followed by implicit lineto is appended to the closed subpath.
3. `<use>` does not inherit styles from the `use` element.
4. Forward/nested `<use>` can lose nested content.
5. Chained gradient `href` fails for forward references.
6. `<symbol>` renders at the top level (should require `<use>`).
7. Nested `<svg>` ignores `x`/`y`/`width`/`height`/`viewBox`.
8. `visibility="visible"` cannot override inherited `visibility:hidden`.
9. Strokes can be clipped by an opacity-layer bbox under a scaled CTM.
10. `userSpaceOnUse` gradients are wrong under non-uniform scale/shear.
11. `stop-color` is not inherited from the gradient element.
12. `<use>` with children leaks the child nodes (`fuzz/leak_check.py`).

### Fuzzing gaps / TODO

- No allocation-failure injection yet (a failing-malloc wrapper).
- No coverage-guided fuzzing (libFuzzer/AFL++); the generators are
  structure-aware but not instrumented.
- No Chrome-headless differential; `refdiff.py` uses macOS WebKit via
  `qlmanage` and compares interiors only.
- No conformance tests for nested `<svg>`, `<symbol>` viewports, `use`
  shadow-tree styles, or real font shaping (they are the `xfail` cases).
- `fuzz_run.c` uses `_exit`, so the crash runner cannot detect leaks; use
  `fuzz/leak_check.py` or Linux ASan/LSan.

## Performance

Measured on Apple Silicon (clang `-O2`), best of three, against the
pre-optimisation baseline:

| Workload | Before | After | Speedup |
|---|---|---|---|
| 200 paths × 60 segments | 3.93 s | 0.24 s | 16.4× |
| 500 large circles, 2000×1500 | 1.11 s | 0.13 s | 8.5× |
| 300 nested opacity groups | 0.75 s | 0.01 s | 75× |
| 400 small circles + gradients | 0.14 s | 0.03 s | 4.7× |
| 1500 text nodes | 0.22 s | 0.05 s | 4.4× |
| **Total** | **6.29 s** | **0.52 s** | **12.1×** |

The nested-opacity case also dropped from 2.1 GB to 19 MB peak RSS.

## Limitations

- No `<image>`, `<filter>`, `<pattern>`, `<mask>`, `<marker>`, or animation.
- No real font files or shaping; text uses a 5×7 bitmap font.
- No `textPath`, `tspan` positioning, `dx`/`dy`, `textLength`,
  `dominant-baseline`, or `writing-mode`.
- No CSS `<style>`, selectors, at-rules, or cascade.
- Geometry properties via `style=` (`x`, `width`, `d`, `transform`, ...) are
  not supported; use XML attributes.
- No nested `<svg>` viewport, `<symbol>` viewport, or full `<use>` shadow tree
  (see the semantic `xfail` list).
- Radial `spreadMethod` with an off-centre focal point, and self-intersecting
  polygons, can differ from other renderers; the specification is ambiguous
  there and implementations disagree.
- Horizontal anti-aliasing is not 2D; near-vertical edges differ from a
  browser.

## Tests

```
test/shapes.svg      basic shapes, transforms, opacity
test/gradients.svg   linear/radial/focal/repeat gradients
test/paths.svg       Béziers, arcs, fill rules
test/text.svg        text rendering, anchors, weights
test/edge.svg        clipping, <use>, dashes, inline style
test/features.svg    objectBoundingBox clips, clip-rule, letter-spacing
test/realworld.svg   a realistic dashboard with mixed features
```

`python3 test/check.py` renders all of them, decodes the PNGs back, and
asserts exact pixel values at known coordinates.

For fuzzing and differential tests, see [`fuzz/README.md`](fuzz/README.md).
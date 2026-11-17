# svg2img

**svg2img** is a dependency-free **SVG → PNG rasterizer written in C17**. It
uses only libc and libm, with its own XML parser, CSS cascade, path parser and
flattener, scanline rasterizer, filter pipeline, and PNG encoder (including a
hand-written DEFLATE implementation). There is no libxml, cairo, libpng, or
external build system.

It is designed as a compact, embeddable renderer for icons, diagrams, charts,
maps, and other static vector artwork.

## Contents

- [Highlights](#highlights)
- [Quick start](#quick-start)
- [Supported SVG profile](#supported-svg-profile)
- [Command-line interface](#command-line-interface)
- [C API](#c-api)
- [Build and configuration](#build-and-configuration)
- [Verification](#verification)
- [Architecture](#architecture)
- [Project layout](#project-layout)
- [Further reading](#further-reading)
- [License](#license)

## Highlights

- **Self-contained.** The parser, style engine, rasterizer, font handling,
  image decoders, and PNG encoder are all in this tree.
- **Small public surface.** The renderer exposes two conversion functions, a
  default-limits helper, one options struct, and one limits struct through
  `libs/svg2img.h`.
- **Bounded resource use.** Conversion budgets cover nodes, nesting depth, path
  points, gradient stops, text length, clip depth, opacity layers, `<use>`
  expansion, and output pixels.
- **Portable C17.** The code builds with GCC or Clang on Linux and macOS, with
  no generated sources and no configuration step.
- **Verified behavior.** A spec-derived conformance corpus, pixel assertions,
  semantic regressions, external-browser checks, and the W3C SVG 1.1 suite are
  part of the repository.

## Quick start

Requirements: a C17 compiler, `make`, libc, libm, and POSIX threads (build
with `-DSVG2IMG_NO_THREADS` to drop the last). Python 3 is needed only
for the test, conformance, and tooling scripts; all of them are stdlib-only.

```sh
make -j"$(nproc)"              # Linux
make -j"$(sysctl -n hw.ncpu)"  # macOS
```

Render an SVG:

```sh
./svg2img icon.svg icon.png

# Fixed width with strong antialiasing and an opaque white background
./svg2img -w 1920 -a 8 -b FFFFFFFF icon.svg icon@2x.png

# Use a TrueType font and resolve relative image references
./svg2img --font Inter.ttf --base-path assets/ diagram.svg diagram.png
```

Run the local checks:

```sh
make check          # pixel assertions and font smoke test
make conformance    # spec-derived conformance corpus
make strict         # warning-clean C17 build
```

### Linux verification with Docker

The x86_64 Linux tree can be built and exercised in a container. On Apple
Silicon this runs through the platform emulator.

```sh
docker build --platform linux/amd64 --load -t svg2img-linux .
docker run --rm --platform linux/amd64 svg2img-linux
```

The conformance runner can also render cases in parallel with `--jobs`:

```sh
docker run --rm --platform linux/amd64 svg2img-linux \
    python3 conformance/run.py --bin ./svg2img --jobs 4
```

## Supported SVG profile

svg2img targets the practical SVG 1.1 feature set used by static artwork and
documents.

| Area | Coverage |
|---|---|
| XML | Elements, attributes, text, comments, CDATA, DOCTYPE, processing instructions, entities |
| Shapes | `rect` with `rx`/`ry`, `circle`, `ellipse`, `line`, `polyline`, `polygon`, `path` |
| Paths | All path commands; arcs converted to cubics; adaptive flattening |
| Structure | `g`, `a`, `defs`, `symbol`, `use` with `x`/`y`/`transform`, `switch`, nested `svg` |
| Paint | Solid colours, linear and radial gradients with focal points, stops, spread methods, patterns, `url()` fallback colours |
| Clipping | `clipPath` with `userSpaceOnUse` / `objectBoundingBox`, `clip-rule`, `fill-rule`, `clip-path` on a `clipPath` and on its children |
| Masking | `<mask>` using luminance × alpha, `maskUnits`, `maskContentUnits` |
| Transforms | `matrix`, `translate`, `scale`, `rotate`, `skewX`, `skewY` |
| Viewport | `viewBox`, `preserveAspectRatio`, intrinsic sizing from `viewBox` |
| Units | User units, `px`, `pt`, `pc`, `in`, `cm`, `mm`, `em`, `ex`, `%` |
| Styling | Presentation attributes, inline `style`, `<style>` rules, selectors, specificity, cascade, `inherit`, `!important`, `font` and `marker` shorthands |
| Colour | `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`, `hsl()`, `hsla()`, `transparent`, `currentColor`, named colours |
| Stroke | Caps, joins, miter limit, dash arrays, dash offsets |
| Text | Built-in bitmap font or TrueType outlines via `--font`; gradient/pattern fills; anchors, letter spacing, x/y lists, `dx`/`dy`, rotation, `textPath`, `textLength`, writing mode, dominant baseline |
| Images | PNG and JPEG, inline data or external files via `--base-path`, palette PNG, bilinear scaling, `preserveAspectRatio` |
| Filters | Blend, colour matrix, component transfer, composite, convolution, lighting, displacement, flood, blur, merge, morphology, offset, tile, and turbulence; primitive subregions; `flood-color`, `flood-opacity` and `lighting-color` as properties |
| Markers | Markers with units, reference points, automatic orientation, and the `marker` shorthand |
| Output | RGBA8 PNG, 1–8× supersampling, optional background |

### CSS support

The SVG 1.1 CSS subset is resolved entirely from the document:

- `<style>` elements, including rules that appear after the elements they style
- Type, class, ID, universal, and attribute selectors
- Descendant and child combinators, selector lists, and structural pseudo-classes
- Cascade order: presentation attributes, author rules, inline `style`, then
  `!important`
- CSS-wide `inherit`
- Presentation attributes for paint, stroke, opacity, clipping, text, and
  related properties

## Command-line interface

```text
Usage: svg2img [options] input.svg output.png
```

| Option | Description |
|---|---|
| `-w, --width N` | Output width in pixels; `0` derives it from the SVG, or from `-h` keeping the aspect ratio (default 0) |
| `-h, --height N` | Output height in pixels; `0` derives it from the SVG, or from `-w` keeping the aspect ratio (default 0) |
| `-s, --scale F` | Additional uniform scale factor (default 1.0) |
| `-a, --aa N` | Supersampling factor, 1–8 (default 4) |
| `-b, --background C` | Background colour as `RRGGBB` or `RRGGBBAA` (default transparent) |
| `--font PATH` | TrueType font for `<text>`; default is the built-in bitmap font |
| `--base-path DIR` | Base directory for relative `<image>` references (files outside it, absolute paths and `..` are refused; defaults to the input file's directory) |
| `-j, --threads N` | Worker threads, 1–64 (default: the `SVG2IMG_THREADS` environment variable, else the CPU count); output is identical for every value |
| `-v, --verbose` | Progress messages on stderr |
| `--help` | Usage text |

`-h` is the height option; use `--help` for the usage summary. The process exits
with status 0 on success and 1 on an argument or conversion error.

## C API

Include `libs/svg2img.h` (or add `-Ilibs` and include `"svg2img.h"`) and link
the renderer objects under `libs/` (everything except `libs/cli.o` and
`libs/main.o`) together with `-lm`.

```c
#include <stdio.h>
#include "svg2img.h"

int main(void) {
    svg2img_options opts = {0};

    opts.width          = 800;             /* 0 = derive from the SVG */
    opts.height         = 0;
    opts.scale          = 1.0f;
    opts.aa             = 4;               /* 1..8 */
    opts.background     = 0xFFFFFFFFu;     /* 0xRRGGBBAA */
    opts.has_background = 1;
    opts.font_path      = "font.ttf";      /* NULL = built-in bitmap font */
    opts.base_path      = "assets/";       /* NULL = no external file loads */
    opts.limits.max_pixels = 16 * 1024 * 1024;

    int rc = svg2img_convert_file("in.svg", "out.png", &opts);
    if (rc != SVG2IMG_OK) {
        fprintf(stderr, "svg2img failed (error %d)\n", rc);
        return 1;
    }
    return 0;
}
```

The two conversion entry points are:

```c
int svg2img_convert(const char *svg_data, size_t len,
                    const char *out_path,
                    const svg2img_options *opts);

int svg2img_convert_file(const char *in_path, const char *out_path,
                         const svg2img_options *opts);
```

Passing `NULL` for `opts` selects the defaults. The input buffer is borrowed for
the duration of the call; no state is retained. The library keeps no global
mutable state, so independent conversions may run concurrently from separate
threads.

`opts->threads` controls parallelism inside one conversion. The library
default (`0`/`1`) is serial, so a host that already runs conversions in
parallel is not oversubscribed. With `N > 1` the image is rasterised in row
bands and the PNG is compressed in fixed-size segments on up to `N` threads.
The PNG bytes are identical for every thread count: bands keep full-canvas
geometry and only clamp writes to their rows, and the segment size does not
depend on `N`. Worker threads get the main thread's stack limit (8–64 MiB) so
the recursion guards hold there too. In an `SVG2IMG_ALLOC_HOOKS` build the
hooks are called from worker threads when `threads > 1` and must be
thread-safe.

### Error codes

| Code | Value | Meaning |
|---|---:|---|
| `SVG2IMG_OK` | 0 | Success |
| `SVG2IMG_ERR_FORMAT` | 1 | Not XML, no `<svg>` root, or empty input |
| `SVG2IMG_ERR_NOMEM` | 2 | Allocation failed |
| `SVG2IMG_ERR_ARG` | 3 | Invalid argument, such as a NULL buffer or path |
| `SVG2IMG_ERR_IO` | 4 | Input could not be read or the PNG could not be written |
| `SVG2IMG_ERR_LIMIT` | 5 | A resource limit was exceeded |

### Resource limits

Every conversion is bounded. A limit field set to `0` selects its compiled-in
default. `svg2img_default_limits(&limits)` returns the full default set. The CLI
reads the same limits from environment variables after parsing its flags.

| Field | Default | Bounds | CLI environment variable |
|---|---:|---|---|
| `max_nodes` | 200000 | Render-tree nodes | `SVG2IMG_LIMIT_MAX_NODES` |
| `max_depth` | 1024 | Element nesting depth | `SVG2IMG_LIMIT_MAX_DEPTH` |
| `max_path_points` | 500000 | Points per path | `SVG2IMG_LIMIT_MAX_PATH_POINTS` |
| `max_path_subpaths` | 100000 | Subpaths per path | `SVG2IMG_LIMIT_MAX_PATH_SUBPATHS` |
| `max_gradient_stops` | 256 | Stops per gradient | `SVG2IMG_LIMIT_MAX_GRADIENT_STOPS` |
| `max_text_length` | 100000 | Characters per text element | `SVG2IMG_LIMIT_MAX_TEXT_LENGTH` |
| `max_clip_depth` | 16 | Nested clip-path levels | `SVG2IMG_LIMIT_MAX_CLIP_DEPTH` |
| `max_layers` | 64 | Opacity layers per render | `SVG2IMG_LIMIT_MAX_LAYERS` |
| `max_use_expansion` | 20000 | Nodes created by `<use>` cloning | `SVG2IMG_LIMIT_MAX_USE_EXPANSION` |
| `max_pixels` | 64 Mpx | Output pixels (`width × height`) | `SVG2IMG_LIMIT_MAX_PIXELS` |

## Build and configuration

### Build targets

| Target | Action |
|---|---|
| `make` / `make -jN` | Build the `svg2img` binary |
| `make check` | Run 53 pixel assertions and the font smoke test |
| `make test` | Render the sample corpus to `/tmp/svg2img_*.png` |
| `make conformance` | Run the 168-case spec-derived conformance corpus |
| `make strict` | Compile every translation unit with warnings as errors |
| `make strict-alloc` | Build the optional allocation-hook path under strict flags |
| `make baseline` | Capture and compare a local visual baseline |
| `make diff-external` | Differentially render against an installed external renderer |
| `make verify-external` | Verify conformance assertions against Chrome, Inkscape, or resvg |
| `make docker-test` | Build and smoke-test the x86_64 Linux image |
| `make clean` | Remove build outputs and generated artifacts |

### Compiler and flags

The default build uses the system C compiler and these flags:

```sh
cc -O3 -std=c17 -Wall -Wextra -pedantic -Ilibs -DSVG2IMG_SLAB -MMD -MP -c libs/file.c
cc -O3 -std=c17 -Wall -Wextra -pedantic -Ilibs -DSVG2IMG_SLAB -o svg2img libs/*.o -lm -lpthread
```

`OPTFLAGS` is the optimization knob; it is expanded into `CFLAGS` and into the
link command, which is what LTO and profile-guided builds need:

```sh
make clean && make -j"$(nproc)" OPTFLAGS='-O3 -flto'   # LTO
make clean && make ALLOC=libc                         # plain libc allocator
make clean && make DOC_ARENA=1                        # experimental doc arena
make clean && make CC=clang                           # any C17 compiler
```

Profile-guided optimization uses the compiler's native flags through
`OPTFLAGS`: `-fprofile-instr-generate` / `-fprofile-instr-use` on Clang, and
`-fprofile-generate` / `-fprofile-use` on GCC. The strict target compiles with:

```text
-std=c17 -O2 -Wall -Wextra -pedantic -Werror -Wconversion -Wsign-conversion
-Wshadow -Wcast-qual -Wmissing-prototypes -Wold-style-definition
-Wredundant-decls -Wwrite-strings -Ilibs
```

Sanitizer and allocation-failure builds live in `fuzz/Makefile` and use their
own flag sets, including:

```sh
cc -std=c17 -g -O2 -fsanitize=address,undefined \
   -fno-sanitize-recover=all -Ilibs ...
cc -std=c17 -g -O2 -fsanitize=address,undefined \
   -fno-sanitize-recover=all -DSVG2IMG_ALLOC_HOOKS -Ilibs ...
```

### Portability notes

The Makefile does not force a job count. Use `-j"$(nproc)"` on Linux or
`-j"$(sysctl -n hw.ncpu)"` on macOS. The Dockerfile uses `make -j"$(nproc)"`.

With Clang on Linux, LTO needs `lld` (`-fuse-ld=lld`). Some GCC versions
mis-handle make's jobserver when `lto-wrapper` runs under `docker run`; use an
interactive shell or `make -j1` for the link if that happens. The default
non-LTO build is unaffected.

## Verification

The repository includes several independent checks:

- **Pixel checks** assert exact colours and alpha at known coordinates across
  the sample corpus and validate every produced PNG structure.
- **Conformance** checks 168 spec-derived cases with 496 assertions, covering
  shapes, paths, transforms, viewports, paint servers, clipping, masking,
  patterns, markers, images, filters, text, styling, and units.
- **Semantic oracle** covers 57 fixed regressions and 32 positive controls.
- **Saved regressions** replay 41 known cases and compare their pixels.
- **External verification** samples every conformance assertion from an
  independent browser or renderer.
- **W3C SVG 1.1 suite** compares the renderer with Chrome across the accepted
  test suite, both normally and with text suppressed.

Run the standard verification pass:

```sh
./scripts/finish_spec_and_perf.sh status
./scripts/finish_spec_and_perf.sh external
./scripts/finish_spec_and_perf.sh w3c
./scripts/finish_spec_and_perf.sh triage
```

| Check | Result |
|---|---|
| Strict build | Pass |
| Pixel checks | 53/53 |
| Conformance | 168/168 cases, 496/496 checks |
| Semantic oracle | 57 fixed regressions + 32 positive controls, 0 xfail |
| Saved regressions | 41 files, 0 failures |
| External Chrome assertions | 421 sampled, 412 agree, 0 disagree, 9 antialiased-edge skips |
| W3C normal pass | 240/525 match within 3%; 272 differ, 13 text-only |
| W3C text-suppressed geometry pass | 397/525 match within 3% |

The W3C suite is an external conformance signal. The text-suppressed pass
removes the built-in-font difference so geometry can be assessed separately.
The Chrome assertion runner uses a process pool (`--jobs`, default
`min(6, cpus)`; `--jobs 1` is serial) and retries a failed browser launch once,
serially, so a transient headless timeout does not become a false skip.

## Architecture

```text
XML bytes
  -> libs/xml_parse.c        DOM
  -> libs/svg_build.c        render tree with style, geometry, and text
  -> libs/svg_refs.c         paint, clip, and use reference resolution
  -> libs/svg_viewport.c     width, height, viewBox, preserveAspectRatio
  -> libs/raster_*.c         scanline fill, gradients, strokes, clips, layers
  -> libs/png_*.c            DEFLATE and PNG container
```

The renderer uses a straight-alpha float canvas, scanline coverage with
supersampling in Y, per-fill scratch from a reusable per-canvas bump arena,
gradient lookup tables, group-opacity layers, and 8-bit clip masks. Hot blend
loops have NEON, SSE2, and scalar paths.

Hot paths are organised around rows rather than pixels: gradients and patterns
shade a whole span into a scratch row before one blend pass, clip and mask
operations work on mask rows, and CSS selectors are compiled once and looked
up through a hashed id/tag/class rule index instead of being re-parsed and
scanned per element. The render tree itself (`Node`, `Style`, `Paint`) stays
object-shaped: building it is not a hot phase once the CSS index exists.

Allocation counts are kept low by packing objects that live and die together
into one block. An XML element, its name and every attribute string are one
allocation. A path (parsed, polyline, glyph run, stroke outline or a
transformed copy) is one `[Path][SubPath...][Pt...]` block; parsing and
flattening collect into stack buffers first and spill to the heap only for
long paths. The device-space copy a shape needs for one draw comes from the
thread's scratch stack, and dash tables live on the stack. Against the
previous layout this removes 60-76% of allocations (`kitchen_sink` 29.6k to
11k, `geo_map` 59.5k to 14k) with identical output and slightly lower peak
memory; `tools/alloc_trace.c` reports them by phase and call site.

Parallel rendering (`threads > 1`) is divide / conquer / merge:

- **Divide.** The canvas is cut into row bands (about three per thread, at
  least 16 rows) handed out through an atomic counter. One serial pre-pass
  stores on every node a conservative range of device rows its subtree can
  paint (a group's range is the union of its children's; a filtered node's is
  its filter region; markers, `textPath` and similar are left unbounded), so
  the tree doubles as a bounding-volume hierarchy for the bands.
- **Conquer.** Each band walks the tree through a canvas view that shares the
  pixels but clamps writes to its rows, and skips any subtree whose range
  misses the band with one comparison. Layers, clip-mask tiles and `<mask>`
  tiles are allocated and rasterised for the band's rows only; they keep
  their full logical size and origin, so every pixel's arithmetic matches a
  serial render. A filter needs its whole region, so its result is computed
  once by the first band that reaches it and shared: other bands wait for it,
  composite their own rows, and the last one frees it.
- **Merge.** None is needed for pixels: bands own disjoint rows of one canvas.

PNG encoding quantises and Up-filters
rows in parallel, then compresses 256 KiB segments independently (each ends
with a sync-flush stored block) and combines their Adler-32 checksums.

## Project layout

| Path | Purpose |
|---|---|
| `libs/` | Renderer sources and internal headers |
| `libs/svg2img.h` | Public API |
| `libs/internal.h` | Shared types, math, allocation helpers |
| `libs/cli.c`, `libs/main.c` | CLI parsing and driver |
| `libs/xml_parse.c`, `libs/xml_dom.c` | XML parser and DOM utilities |
| `libs/path_parse.c`, `libs/path_util.c` | Path parsing, arcs, flattening, bounds |
| `libs/color.c`, `libs/length.c` | Colour and length parsing |
| `libs/svg_build.c`, `libs/svg_refs.c`, `libs/svg_viewport.c` | Render-tree construction |
| `libs/svg_style.c`, `libs/svg_css.c`, `libs/svg_text.c` | Styling, selectors, text |
| `libs/svg_gradient.c`, `libs/svg_pattern.c` | Paint servers |
| `libs/svg_mask.c`, `libs/svg_marker.c`, `libs/svg_filter.c` | Masking, markers, filters |
| `libs/raster_*.c` | Rasterization and compositing |
| `libs/png_deflate.c`, `libs/png_write.c` | PNG encoding |
| `libs/font.c`, `libs/font_ttf.c` | Built-in font and TrueType outlines |
| `test/` | Sample corpus and pixel checks |
| `conformance/` | Spec-derived cases, Chrome comparison, W3C runner and triage |
| `fuzz/` | Grammar, semantic, resource, sanitizer, and regression harnesses |
| `tools/` | PNG decoding, perceptual diff, visual baseline, allocation tracing |
| `docs/SVG11-single-page.html` | Vendored SVG 1.1 specification |
| `scripts/finish_spec_and_perf.sh` | Verification and reporting driver |

## Further reading

- [`conformance/README.md`](conformance/README.md) — assertion kinds, scorecard,
  external verification, and the W3C runner
- [`fuzz/README.md`](fuzz/README.md) — grammar, semantic, resource, leak, and
  regression harnesses
- [`tools/README.md`](tools/README.md) — PNG utilities, perceptual diff, and the
  visual baseline workflow
- [`test/check.py`](test/check.py) — pixel assertions for the sample corpus

## License

MIT. See [`LICENSE`](LICENSE).

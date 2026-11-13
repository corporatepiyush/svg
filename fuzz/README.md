# svg2img fuzzing harness

Black-box, grammar-aware, semantic, resource and leak testing for the pure-C
SVG→PNG converter.  The crash runner links the renderer into a batch binary
that forks one child per input, so a crash cannot take down the driver and a
campaign can run unattended.

Everything lives outside the source tree by default (`$TMPDIR/svg2img_fuzz`);
set `SVG2IMG_FUZZ_WORK` to keep corpora elsewhere.

## Scope

The renderer is **SVG-only**.  CSS is intentionally out of scope: there is no
`<style>`, selector, at-rule, specificity or cascade.  Presentation attributes
and inline `style="..."` are supported.  Fuzz inputs may contain `<style>` as
opaque text (for example in XML-grammar tests); it must never crash.

## Layout

| file | purpose |
|---|---|
| `fuzz_run.c` | fork-per-input batch runner (ASan + UBSan) |
| `semantics.py` | shared PNG/render/pixel helpers for oracle tests |
| `semantic_cases.py` | pixel-oracle tests: 14 controls + 12 `xfail` conformance gaps |
| `gen_seeds.py` | structured seed corpus: every feature, odd numbers, deep/wide docs |
| `mutate.py` | structure-aware mutation (splice, duplicate, rewrite numbers/tags) |
| `rawfuzz.py` | raw-byte / malformed-markup fuzzer |
| `featurfuzz.py` | feature-targeted generator: path grammar, colours, transforms, dashes, id/use graphs |
| `grammar_fuzz.py` | XML/reference/numeric grammar: DOCTYPE, CDATA, entities, use/symbol graphs, extreme numbers |
| `resource_fuzz.py` | timeout / unbounded-work tests (use chains, dashes, wide/deep trees, huge text, canvases) |
| `leak_check.py` | leak regression under macOS `leaks` / Linux valgrind |
| `bigfuzz.py` | very large documents (100k shapes, 400k segments, 4000×4000) |
| `campaign.py` | generate → run (parallel) → minimise loop; stops on the first reproducible crash |
| `bigrun.py` | per-file timing and pass/fail for the big corpus |
| `run_parallel.sh` | run a file list with N parallel workers |
| `regress.py` | renders `regress/*.svg` and checks known-bad pixels |
| `difftest.py` | invariant tests (no reference renderer needed) |
| `refdiff.py` | pixel comparison against WebKit via `qlmanage` (macOS only) |
| `refdiff_rand.py` | randomised differential test against WebKit |
| `regress/` | 22 saved regression cases |
| `docker/` | linux/amd64 cross-build and cross-test image |

## Usage

```sh
cd fuzz

make                    # build ./fuzz_run (sanitizers) and ./one (single file)
make seeds              # ~200 structured seeds
make gen                # generate 3000 structured mutants
make gen-run            # generate + run them in parallel
make raw                # generate 4000 raw-byte mutants
make raw-run            # generate + run them in parallel
make feature            # 6000 feature-targeted mutants + parallel run
make grammar            # XML/reference/numeric grammar mutants + parallel run
make semantic           # pixel-oracle tests (14 controls, 12 xfail)
make resource           # timeout / unbounded-work tests (full)
make resource-quick     # timeout / unbounded-work tests (fast subset)
make leak               # leak regression tests
make run                # seeds under sanitizers (parallel)
make regress            # 22 saved crashers must render correctly
make campaign           # generate + run mutants until a reproducible crash is found
make refdiff            # fixed differential corpus vs WebKit (macOS)
make refdiff_rand       # randomised differential test vs WebKit (macOS)
make clean
```

### Parallelism

`JOBS=0` (the default) means "use all online CPUs"; set `JOBS=N` to cap the
worker count.  `feature`, `grammar`, `run`, `gen-run`, `raw-run`, `campaign`,
`big`, and the Docker cross-test all run through `run_parallel.sh`, which
splits the input list across workers and aggregates their summaries.

```sh
./run_parallel.sh list.txt 0 2       # auto workers, 2x AA
JOBS=4 ./run_parallel.sh list.txt    # four workers
BIN=/path/to/fuzz_run ./run_parallel.sh list.txt 0 2
```

Run any list of files with parallel workers:

```sh
./run_parallel.sh list.txt 8 2      # 8 workers, 2x AA
```

Exit status is non-zero if any input crashed or tripped a sanitizer.

## The crash runner

`fuzz_run.c` reads paths on stdin, forks one child per file, and renders it
with `svg2img_convert()`.  A signal, sanitizer abort, or unexpected exit is
reported.  It caps inputs at 8 MB by default and treats a non-zero conversion
return as a clean failure.  Each child has a 60 s `SIGALRM` by default
(`SVG2IMG_FUZZ_TIMEOUT=N` overrides it); exit code 3 is reported as `TIMEOUT`, so one pathological input cannot occupy a worker
forever.

Important: the child calls `_exit`, so LeakSanitizer cannot run in the crash
runner.  Use `leak_check.py` (or Linux ASan/LSan with a normally exiting
harness) for leak coverage.

## Semantic oracle tests

The crash fuzzers only check that the process survives.  `semantic_cases.py`
renders small documents and asserts pixels: fill rules, gradients, clips,
opacity, `use`, symbol/nested-viewport behaviour, stroke bounds, and SVG-only
styling.

```sh
python3 fuzz/semantic_cases.py ./svg2img
14 controls, 12 gap cases: 0 unexpected failure(s), 12 xfail
```

- **Controls** are behaviour that must keep working (closed-path fill,
  polygon, clip inside/outside, linear gradient, group opacity, `use`,
  symbol-via-use, text, `<style>` ignored, inline style, `font-size: %`,
  stop inline style, opacity inline style, clip-path inline style).
- **xfail cases** are confirmed SVG conformance bugs.  The runner prints
  `XPASS` when one is fixed; remove the marker then.

Current xfail list:

1. Open subpaths are not filled as if closed (`polyline`/open `path`).
2. `Z` followed by implicit lineto is appended to the closed subpath.
3. `<use>` does not inherit styles from the `use` element.
4. Forward/nested `<use>` can lose nested content.
5. Chained gradient `href` fails for forward references.
6. `<symbol>` renders at the top level.
7. Nested `<svg>` ignores `x`/`y`/`width`/`height`/`viewBox`.
8. `visibility="visible"` cannot override inherited `visibility:hidden`.
9. Strokes can be clipped by an opacity-layer bbox under a scaled CTM.
10. `userSpaceOnUse` gradients are wrong under non-uniform scale/shear.
11. `stop-color` is not inherited from the gradient element.
12. `<use>` with children leaks the child nodes (`leak_check.py`).

## Grammar fuzzer

`grammar_fuzz.py` builds documents around the corners that token mutators
under-sample:

- XML: DOCTYPE internal subsets, CDATA, processing instructions, comments,
  entities (`&#0;`, `&#x110000;`, `&bogus;`, missing `;`), duplicate
  attributes, namespaced attributes, BOM, invalid UTF-8, NUL, long names.
- Reference graphs: nested/forward `use`, `use` with children, symbol
  viewports, gradient `href` chains, `clipPath` inside `use`/`symbol`,
  duplicate ids, self-reference and cycles.
- Numeric/path: hex floats, subnormals, huge exponents, `NaN`/`inf`,
  arc-flag forms, extreme radii, dash arrays, miter limits, singular CTMs.

Output is written to `OUT/` and tagged in `OUT/manifest.txt` so a crash can be
traced to a feature.

## Resource / denial-of-service tests

`resource_fuzz.py` generates documents with unbounded work and runs each under
a wall-clock budget and an address-space limit.  Cases:

| case | what it stresses |
|---|---|
| `deep_2000`, `deep_5000` | DOM depth cap and recursive build/free |
| `wide_100k` | many siblings (`node_add_child` growth) |
| `use_10`, `use_18` | `use` expansion |
| `use_22` (xfail) | exponential `use` expansion (~2M nodes) |
| `dash_60k`, `dash_200k` | dashed-stroke extraction |
| `text_200k` | long text node (glyph-pixel accumulator) |
| `siblings_100k` | wide render tree |
| `attr_1m` | 1 MB attribute value |
| `canvas_4096` | large canvas allocation |
| `path_1m` | 1M-point path |

`--quick` skips `path_1m` and `canvas_4096`.  A timeout is a failure unless
the case is marked xfail; an xfail that finishes is reported as `XPASS`.

## Leak tests

`leak_check.py` runs a corpus through macOS `leaks --atExit` or Linux
valgrind.  It currently tracks one known leak:

- `<use>` with children: `resolve_use` frees the child array but not the child
  `Node`s (4 blocks per child).  The leak suite reports it as `xfail`; it will
  print `XPASS` when fixed.

Run `make leak` (macOS) or install `valgrind` (Linux).

## Differential testing against WebKit

`refdiff.py` and `refdiff_rand.py` render documents with both this converter
and the system WebKit (`qlmanage -t`) and compare interior pixels.  Only
interiors are compared: antialiasing and colour management differ between
rasterisers, so edge pixels are excluded.

The fixed corpus covers rects, circles, ellipses, polygons, rectilinear and
curved paths, viewBox, group opacity, nested groups, strokes, linear/radial
gradients, clips, even-odd holes, rotations, `use`, `preserveAspectRatio`, and
nested clips.  Known reference quirks (excluded from the random corpus):
WebKit's radial `spreadMethod` with an off-centre focal point behaves like pad,
and self-intersecting polygons can differ at edges.

There is no Chrome-headless differential yet; that is a TODO.

## Regression corpus (`regress/`)

One file per bug found, with pixel expectations where applicable.  Add a file
for every bug and, if the result is visual, add an entry to `EXPECT` in
`regress.py`.

## linux/amd64 cross-test

The SSE2 blend path only compiles on x86, so it is verified in a container:

```sh
podman machine start            # once
podman build --arch amd64 -t svg2img-test -f fuzz/docker/Dockerfile .
podman run --rm --arch amd64 svg2img-test
```

The container runs:

- strict `-Werror` build of every module;
- the 40 pixel checks;
- the 22 regression cases;
- the invariant tests;
- the semantic oracle (14 controls + 12 xfail);
- the quick resource tests;
- the leak checker (valgrind);
- sanitizer batch runs over seeds, structured mutants, and grammar mutants,
  using `run_parallel.sh` with all online CPUs.

## What the harness has found

- Stack overflow on deeply nested input → DOM depth cap + iterative teardown.
- NULL dereferences and leaks on allocation-failure paths (path subpath array,
  XML element stack, PNG bit writer, node child list).
- Uninitialised `Seg.c1/c2` read by the flattening bbox loop (UB).
- NaN / out-of-range coordinates reaching `(int)` casts in the scanline
  bucketing and the viewport.
- Gradient bugs found by the WebKit differential test: stop offsets must be
  clamped (not sorted), `spreadMethod` must be applied per pixel, the radial
  focal quadratic had a sign error, and equal-offset hard stops must use the
  later stop.
- Group opacity layers double-applying the group transform in the bbox.
- Silent wrong-pixel bugs found by the semantic oracle: open-subpath fill,
  `use` style/leak/forward refs, symbol/nested-viewport, stroke layer
  clipping, userSpace gradients, stop inheritance.

## How to add a case

- **Crash regression:** add `regress/foo.svg`; if it has a visual expectation,
  add `(x, y) -> rgba` to `EXPECT` in `regress.py`.
- **Semantic case:** add `case(...)` to `CONTROLS` (must pass) or `CASES`
  (xfail until fixed) in `semantic_cases.py`.
- **Grammar case:** add to `XML_RAW`, `REF_BODIES`, or `NUM_BODIES` in
  `grammar_fuzz.py`.
- **Resource case:** add `(name, svg, budget, xfail)` to `cases()` in
  `resource_fuzz.py`.
- **Leak case:** add `(name, svg, xfail)` to `CASES` in `leak_check.py`.

## Notes / TODO

- No allocation-failure injection yet; add a failing-malloc wrapper.
- No coverage-guided fuzzing (libFuzzer/AFL++); the generators are
  structure-aware but not instrumented.
- No Chrome-headless differential; `refdiff` is macOS WebKit only.
- The campaign stops at the first reproducible crash and writes
  `campaign/minimal.svg`.  Fix the bug, add a `regress/` case, then rerun.
- The sanitizer build finds the bugs; `make fast` is the timing build.
- `regress/` is the permanent record.
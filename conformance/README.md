# Conformance corpus

Spec-derived pixel assertions for the SVG features the renderer claims to
support. This is the *correctness* suite: it asks "does this feature render the
way SVG 1.1 says it should?" rather than "does the output still match what we
produced last time?" (that is the job of the visual baseline, below).

Everything is stdlib-only Python. No packages, no network.

## Layout

| path | what |
|---|---|
| `cases/*.svg` | 63 small self-contained documents, one per behaviour |
| `expectations.json` | per-case feature area, note, assertion list, `xfail` reasons |
| `run.py` | the runner: render, assert, scorecard, JSON summary |
| `measure.py` | dev helper: samples current output into an expectations file |
| `baseline/` | generated visual baselines (not committed) |

## Run it

```sh
make conformance                       # builds first if needed
python3 conformance/run.py              # equivalent
python3 conformance/run.py --filter gradients
python3 conformance/run.py --list
python3 conformance/run.py --tol 20 --verbose
python3 conformance/run.py --json /tmp/c.json --md /tmp/c.md
```

Exit status is 0 only when every assertion passes. `xfail` cases do not fail
the run; an `xfail` that starts passing is reported as `XPASS` so the marker
can be removed.

## Assertion kinds

Every case uses a `viewBox` equal to its `width`/`height`, so user units map
1:1 to pixels and assertions can use literal coordinates. Probe points sit in
flat interiors, away from antialiased edges, so they are stable across
supersampling factors.

| kind | meaning |
|---|---|
| `rgba` | RGBA at (x, y), per-channel tolerance (default 10) |
| `alpha` | alpha at (x, y) within `[min, max]` |
| `bbox` | ink bbox equals `[x0, y0, x1, y1]`, exclusive, `±tol` |
| `bbox_width` | ink bbox width `>= min` |
| `bbox_center_x` | ink bbox centre within `tol` of `x` |
| `ink` | count opaque dark pixels in a rect `>= min` |
| `compare_ink` | `ink(a) / ink(b) >= min_ratio` |

## Scorecard

`run.py` prints a per-area table and writes `summary.json`:

```json
{
  "cases_total": 63, "passed": 60, "failed": 0, "xfailed": 3,
  "checks_total": 152, "checks_failed": 4,
  "areas": {"paths": {"cases": 9, "pass": 8, "xfail": 1, "...": 0}},
  "results": [{"name": "...", "status": "pass", "checks": [...]}]
}
```

Feature areas: `shapes`, `paths`, `transforms`, `viewport`, `gradients`,
`clip`, `opacity`, `use`, `text`, `style`, `units`, `color`, `stroke`.

## Adding a case

1. Write `conformance/cases/<name>.svg`. Keep it under ~10 elements; use a
   `viewBox` equal to the canvas so coordinates are pixels.
2. Add an entry to `expectations.json` with `area`, `note`, and `checks`.
   Derive the expected values from the specification, not from the current
   output — that is the whole point.
3. Run `python3 conformance/run.py`. If the case is a known bug, add
   `"xfail": "module.c: what is wrong"` instead of weakening the assertion.

If you only want to *sample* what the renderer currently produces (for
exploration, never as a source of truth), use `measure.py`:

```sh
python3 conformance/measure.py > /tmp/expectations.json
```

## Known gaps (xfail)

These are confirmed conformance bugs, tracked here so they are measured rather
than forgotten. Each names the responsible module.

1. `color_rgb_int` -- `rgb(0,128,0)` renders as pure green. `color.c
   parse_rgb` treats every component as a 0..1 float; integers must be scaled
   by 1/255.
2. `text_letterspacing` -- `letter-spacing` is parsed into
   `Style.letterspacing` but `svg_build.c` never copies it onto the node, so it
   has no effect.
3. `path_cubic_extent` -- high-curvature cubics are flattened too coarsely and
   render visibly narrower than the true Bézier (Chrome and WebKit agree with
   the analytic curve; this renderer does not).

## Visual baseline (review aid)

`expectations.json` is the committed oracle. The baseline is a *review* tool:
it answers "what changed visually after this rendering change?", which sampled
pixels cannot.

```sh
python3 tools/baseline.py capture     # render the corpus into conformance/baseline
python3 tools/baseline.py compare     # re-render, diff, write report.json
python3 tools/baseline.py list        # show baseline metadata
```

Only failing cases get a `diff.png` artifact, so a clean run leaves a small
directory. Compare uses `tools/perceptual.py` (per-channel tolerance, optional
blur, differing-pixel budget) rather than a byte comparison, so an
antialiasing shift does not show up as a total change.

Baselines are machine-specific and are **not** committed; `make baseline` is a
local workflow. `make conformance` is the reproducible check.
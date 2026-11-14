# Conformance corpus

Spec-derived pixel assertions for the SVG features the renderer claims to
support. This is the *correctness* suite: it asks "does this feature render the
way SVG 1.1 says it should?" rather than "does the output still match what we
produced last time?" (that is the job of the visual baseline, below).

Everything is stdlib-only Python. No packages, no network.

## Layout

| path | what |
|---|---|
| `cases/*.svg` | 85 small self-contained documents, one per behaviour |
| `expectations.json` | per-case feature area, note, assertion list, `xfail` reasons |
| `run.py` | the runner: render, assert, scorecard, JSON summary |
| `verify_external.py` | samples every assertion from an independent renderer |
| `measure.py` | dev helper: samples current output into an expectations file (never the oracle) |
| `w3c_chrome.py` | the official W3C SVG 1.1 test suite, compared with Chrome |
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

There are currently **none**. The three that used to be tracked here
(`color_rgb_int`, `text_letterspacing`, `path_cubic_extent`) were real bugs and
are fixed; each is now an ordinary case.

`path_cubic_extent` was a flattening bug, not a tolerance choice: the de
Casteljau stack aliased its own slot, so the left half of every cubic was
dropped. Fixing it made all 86 cases agree with Chrome.

## Validating the expectations

Expectations are written from the specification, but hand-written values can
still be wrong. `verify_external.py` samples the exact coordinates of every
`rgba`/`alpha` assertion from an independent renderer (Chrome, Inkscape or
resvg) and compares them with the committed value:

```sh
python3 conformance/verify_external.py --backends chrome
173 sampled assertion(s): 173 agree, 0 disagree, 5 skipped
```

Rules it enforces:

- each case renders at its own `width`/`height`;
- pixels the reference leaves partially covered are skipped (antialiasing
  differs between rasterisers, so an edge pixel is not a conformance signal);
- cases that must differ by design are listed in `external_intentional`
  (currently `style_element`: there is no CSS engine).

This check is what caught a miter join that asserted no ink where the spec
requires the spike, and an `xfail` that was really a wrong expectation.

## The official W3C test suite

The hand-written corpus measures what we claim to support; it cannot measure
what we do not. The W3C SVG 1.1 suite (525 accepted tests) can. It is not
committed (14 MB of W3C material), so `w3c_chrome.py` downloads it on first
use into `conformance/.w3c-suite` (override with `SVG11_CACHE`).

```sh
python3 conformance/w3c_chrome.py            # all 525 tests
python3 conformance/w3c_chrome.py --limit 40 --json /tmp/w3c.json
```

It renders each test with this renderer and with Chrome, masks text (this
renderer has a bitmap font, so glyphs cannot match), and reports the pass rate
per specification chapter. Text-only tests are reported separately rather than
counted as passes.

Current result: **261 of 512 comparable tests match Chrome** (51%) before the
feature batches; after them, a 172-test subset of the implemented chapters
matches 111 of 170 comparable tests (65%). The full breakdown and the list of
what is missing are in
[`docs/conformance_status.md`](../docs/conformance_status.md).

The suite's own reference PNGs are not used: they come from a renderer whose
strokes snap to the pixel grid, so comparing against them measures that
renderer's antialiasing rather than conformance.

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
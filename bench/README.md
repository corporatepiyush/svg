# Benchmark harness

Deterministic timing and peak-RSS measurements for `svg2img`. The point is to
notice a rendering change that made things slower, not to publish a leaderboard
number.

## Run it

```sh
make bench                             # generate cases, run, compare to baseline
python3 bench/gen.py                   # regenerate bench/cases (deterministic)
python3 bench/run.py --repeat 7
python3 bench/run.py --threshold 1.2   # stricter regression threshold
python3 bench/run.py --check           # exit non-zero on regression (for CI)
python3 bench/run.py --update-baseline # re-pin after an intended change
```

## What is measured

Fixed documents stressing individual subsystems and realistic combinations:

| case | work | size |
|---|---|---|
| `circles_500` | 500 large filled circles | 2000x1500 |
| `paths_200x60` | 200 cubic paths | 600x600 |
| `opacity_300` | 300 nested opacity groups | 200x200 |
| `grad_400` | 400 gradient-filled circles | 800x600 |
| `text_1500` | 1500 text nodes | 1000x400 |
| `dash_200` | 200 dashed polylines | 800x600 |
| `clip_300` | 300 clipped groups | 600x600 |
| `dashboard_1200x800` | cards, bars, gradients and labels | 1200x800 |
| `map_1000x700` | roads, water, parks, markers and labels | 1000x700 |
| `illustration_800x800` | mask, pattern, blur, gradients and layers | 800x800 |
| `icons_1000x1000` | symbol/use icon grid with strokes | 1000x1000 |
| `chart_1200x600` | line/area chart, two series, markers, legend | 1200x600 |

Each case runs `--repeat` times (default 5) and reports the **best** wall time.
Best-of-N is the least noisy estimator on a shared machine: it is the run that
got a clean slice of CPU. Median and worst are reported too, so a bimodal
distribution is visible rather than hidden.

Peak RSS comes from `os.wait4` per child, so it is that render's high-water
mark, not the maximum over every child the harness has ever reaped.

## Baselines and thresholds

`bench/baseline.json` is generated locally by `--update-baseline` and ignored
by git; it stores the per-case best time, RSS, and the machine they were
measured on. Comparison is relative, never absolute:

* **Time** fails when a case is more than `--threshold` (default 1.35) times
  its baseline best.
* **RSS** fails when a case grows more than `--rss-threshold` (default 1.75)
  times its baseline.

If the baseline was measured on a different OS or architecture, the run reports
`no comparable baseline` and skips comparison entirely rather than comparing
apples to arm64. Re-pin with `--update-baseline` on the machine you care about.

Do not treat a threshold breach as automatically a regression: a shared CI
runner with noisy neighbours will produce them. That is why `--check` is
opt-in, and why the CI job runs the benchmark without it (informational) by
default.

## Not overfitting to one machine

* Machine identity (OS, arch, cores, Python) is recorded in both the baseline
  and the results.
* Thresholds are ratios, so a 2x faster machine does not need a new baseline
  to be useful — but it does need one to be *compared*.
* `baseline.json` is a local generated file, not part of the repository; it is
  expected to be re-pinned when the machine or the measurement protocol
  changes.
## Profiling corpus

`bench/stress_gen.py` writes ten large, feature-dense documents to
`bench/stress_cases/` (maps, time series, icon sheets, filters, text, gradients,
masks, patterns, heavy strokes, CSS-styled dashboards). They are meant for a
profiler, not for timing comparisons:

```sh
python3 bench/stress_gen.py
SVG2IMG_LIMIT_MAX_USE_EXPANSION=200000 ./svg2img bench/stress_cases/icon_sheet.svg out.png
```

`icon_sheet` exceeds the default `<use>` expansion budget, so raise it as shown.

## Complex corpus

`bench/complex_gen.py` writes ten authored-style documents to
`bench/complex_cases/` (dashboard, city map, infographic, UI kit, technical
drawing, illustration, calendar, circuit board, image poster, and a
"kitchen sink" mixing every feature). Each has one element per line, at least
500 lines and at least 100 distinct components (gradients, patterns, filters
of every primitive family, masks, clip paths, markers, symbols with `<use>`,
CSS classes and selectors, `textPath`, nested `<svg>`, embedded images, and
widget groups). The generator exits non-zero if any document falls below them.

```sh
python3 bench/complex_gen.py --stats
./svg2img -j 1 -s 2 bench/complex_cases/kitchen_sink.svg serial.png
./svg2img -j 10 -s 2 bench/complex_cases/kitchen_sink.svg parallel.png
cmp serial.png parallel.png      # identical for every thread count
```

The corpus is also checked against Chromium:

```sh
python3 fuzz/diff_external.py --bin ./svg2img --corpus bench/complex_cases --backends chrome
```

Last run: 10 of 10 documents with no mismatch groups (antialiasing, colour
management and font metrics are excluded by construction). This comparison
found the vanishing-circle arc bug and the coarse flattening of
small-user-unit shapes noted in `ISSUES.md`.

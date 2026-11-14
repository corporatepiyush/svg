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

Seven fixed documents, each stressing one subsystem:

| case | work | size |
|---|---|---|
| `circles_500` | 500 large filled circles | 2000x1500 |
| `paths_200x60` | 200 cubic paths | 600x600 |
| `opacity_300` | 300 nested opacity groups | 200x200 |
| `grad_400` | 400 gradient-filled circles | 800x600 |
| `text_1500` | 1500 text nodes | 1000x400 |
| `dash_200` | 200 dashed polylines | 800x600 |
| `clip_300` | 300 clipped groups | 600x600 |

Each case runs `--repeat` times (default 5) and reports the **best** wall time.
Best-of-N is the least noisy estimator on a shared machine: it is the run that
got a clean slice of CPU. Median and worst are reported too, so a bimodal
distribution is visible rather than hidden.

Peak RSS comes from `os.wait4` per child, so it is that render's high-water
mark, not the maximum over every child the harness has ever reaped.

## Baselines and thresholds

`bench/baseline.json` stores per-case best time and RSS, plus the machine it
was measured on:

```json
{"version": 1,
 "machine": {"system": "Darwin", "machine": "arm64", "cores": 10, "...": 0},
 "cases": [{"case": "circles_500", "best": 0.132, "rss_kb": 61712, "...": 0}]}
```

Comparison is relative, never absolute:

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
* `baseline.json` is a normal tracked file so a change in it is reviewable;
  it is expected to be re-pinned when the machine or the measurement protocol
  changes.
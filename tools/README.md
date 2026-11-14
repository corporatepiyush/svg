# Measurement tools

Shared, dependency-free helpers used by the conformance runner, the baseline
workflow and the differential harness. All stdlib only.

| file | purpose |
|---|---|
| `pngio.py` | PNG decode (filters 0-4, grey/RGB/RGBA) and RGBA8 encode |
| `perceptual.py` | CLI: perceptual diff with tolerance, blur, budget, artifacts |
| `baseline.py` | CLI: capture / compare / list a visual baseline |

## Perceptual diff

```sh
python3 tools/perceptual.py before.png after.png
python3 tools/perceptual.py before.png after.png --tolerance 20 --blur 2
python3 tools/perceptual.py before.png after.png --area 0.5 --save-artifacts
```

A byte comparison is useless for a rasteriser: any change in antialiasing or
curve flattening changes every edge pixel. `perceptual.py` instead:

* ignores per-channel deltas below `--tolerance` (default 12),
* optionally blurs both images (`--blur R`, premultiplied box blur) so a
  one-pixel shift is not a full-image change,
* compares premultiplied colour, so a transparent pixel does not count as a
  colour difference against opaque black,
* passes when the differing area is under `--area` percent (default 0.1),
* reports `max`, `mean`, and `mean over differing pixels` — the last is the
  "how bad is a real difference" number,
* writes `diff.png` (red where different) and `amp.png` (amplified |A-B|) only
  with `--save-artifacts`, plus `summary.json` always.

Exit status: 0 pass, 1 differ, 2 usage or decode error.

## Baseline workflow

```sh
python3 tools/baseline.py capture
python3 tools/baseline.py compare
python3 tools/baseline.py list
```

`capture` renders every conformance case at canonical settings (`-a 4`, no
background) and writes `manifest.json`. `compare` re-renders into a scratch
directory, runs the perceptual comparison per case, and writes `report.json`
plus a `diff.png` only for the cases that differ.

The baseline is a review aid, not the oracle. `conformance/expectations.json`
is committed and spec-derived; the baseline is machine-specific and generated.

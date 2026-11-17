# Measurement tools

Shared, dependency-free helpers used by the conformance runner, the baseline
workflow and the differential harness. All stdlib only.

| file | purpose |
|---|---|
| `pngio.py` | PNG decode (filters 0-4, grey/RGB/RGBA) and RGBA8 encode |
| `perceptual.py` | CLI: perceptual diff with tolerance, blur, budget, artifacts |
| `baseline.py` | CLI: capture / compare / list a visual baseline |
| `alloc_trace.c` | allocation-pattern recorder linked into a `-DSVG2IMG_ALLOC_TRACE` build |
| `analyze_alloc_trace.py` | offline lifetime, call-site, phase, and realloc analysis |

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

## Allocation trace

Build the renderer with the trace macros and link the recorder in addition to
the normal sources in `libs/`; `libs/svg_alloc.c` compiles to no backend in
this mode.

```sh
cc -O2 -std=c17 -DSVG2IMG_ALLOC_TRACE -Ilibs -c tools/alloc_trace.c
# compile the library sources under libs/ with the same flag, link tools/alloc_trace.o
SVG2IMG_TRACE_FILE=/tmp/alloc.trace ./svg2img in.svg out.png
python3 tools/analyze_alloc_trace.py /tmp/alloc.trace
```

Each event records the operation, requested and old size, call site, stable
allocation id, and phase marks from the parser, builder, renderer, encoder and
teardown. The analyser reports allocation counts and bytes by phase and call
site, peak live composition, lifetime percentiles, free-order stack distance,
and realloc movement between size classes.

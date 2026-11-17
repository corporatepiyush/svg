# svg2img remaining limitations

This file records known gaps that are intentionally **not fixed**. Completed
conformance fixes have been removed; use the conformance suite, triage reports,
and the benchmark script to verify the current behavior.

## Verification snapshot

- `make strict`: passes
- Pixel checks: 53/53
- Conformance: 168 cases, 496/496 checks
- Image-loading security tests (`test/security_test.py`): 13/13
- Semantic oracle: 57 fixed regressions + 32 positive controls, 0 xfail
- Saved regressions: 41 files, 0 failures
- External Chrome assertions: 421 sampled, 412 agree, 0 disagree, 9 antialiased-edge skips (5 cases excluded as intentional differences, listed in `conformance/expectations.json`)
- W3C SVG 1.1, normal inline Chrome: 240/525 tests match within 3% of pixels
  (46.9% comparable; 272 differ and 13 are text-only)
- W3C SVG 1.1, text-suppressed geometry pass: 397/525 tests match (75.6%)
- Benchmark and allocation measurements are generated locally with
  `make bench` and `tools/analyze_alloc_trace.py`; reference numbers are not
  stored in the repository.

Reproduce everything with:

```sh
./scripts/finish_spec_and_perf.sh all
```

## Deliberate scope limits

| ID | Item | Status |
|----|------|--------|
| SCOPE-01 | Animation (SMIL) is not implemented. W3C animation tests are informational only. | WONTFIX |
| SCOPE-02 | Scripting / DOM / event handling is not implemented. | WONTFIX |
| SCOPE-03 | Real font shaping (kerning, ligatures, hinting) is out of scope; the default is the built-in bitmap font. `--font` loads TrueType outlines but does not shape them. | WONTFIX |
| SCOPE-04 | External CSS stylesheets and at-rules (`@import`, `@font-face`, `@media`) are not fetched. | WONTFIX |
| SCOPE-05 | `foreignObject` is not rendered. | WONTFIX |

## Known compatibility gaps

These are not tracked as conformance failures in the corpus; they remain known
differences from a browser or from less common SVG 1.1 behavior.

- A `<use>` shadow tree is not fully materialized. The renderer clones the
  referenced subtree with inherited style, which covers the SVG 1.1 cases in
  the corpus but not dynamic DOM mutation.
- Geometry properties set through CSS (`x`, `width`, `d`, `transform`, ...) are
  not supported; SVG 1.1 uses XML attributes for these.
- Radial `spreadMethod` with an off-centre focal point and self-intersecting
  polygons can differ from other renderers; the specification is ambiguous
  there and implementations disagree.
- Horizontal antialiasing is not 2D; near-vertical edges can differ from a
  browser.
- W3C filters, masks/markers, text metrics, external references, and a few
  path/geometry corners still have failures. The per-test reports are in
  `conformance/w3c_triage.md` and
  `conformance/w3c_suppress_text_triage.md` (generated; not committed).
- Parsing, CSS matching and tree building are serial, so small or
  style-heavy documents gain little from threads (`css_dashboard`: 1.2x on
  10 cores against 3-6x for render-heavy documents).
- A shared filter result is computed by one thread while other bands that
  need it wait; a document dominated by one large filter does not scale.
- Fixed in this round, all pre-existing: built-in font glyphs were drawn
  mirrored left-to-right; an objectBoundingBox `clip-path` on a transformed
  element used the device-space box and was shifted off target; `-w`, `-h`
  and `-s` resized the canvas without scaling the drawing; `-w` or `-h` alone
  did not keep the aspect ratio. Covered by `text_glyph_orientation`,
  `clip_bbox_transformed` and the size checks in `test/check.py`.
- Also fixed, both pre-existing: for about 4 in 10 radii a `<circle>` or
  `<ellipse>` (and any clip or mask built from one) rendered as nothing,
  because rounding made the arc-centre radicand slightly negative and the
  NaN guard replaced each half-arc with a chord (`shapes_circle_radii`).
  Path flattening used a fixed 0.05 user-unit floor, so shapes in small user
  units (objectBoundingBox clips, `viewBox="0 0 1 1"`) became visible
  polygons; the tolerance is now relative to the path's size.
- Most remaining allocations come from building the render tree: one
  `Node` (about 1.2 KiB) per element plus small copies of ids and reference
  strings (`fill`/`clip-path`/`marker` URLs) per node. Interning those
  strings into a per-document pool would remove most of the rest but touches
  every place that copies or frees a `Node`.
- Parallel PNG encoding needs a separate 8-bit image buffer (a quarter of the
  float canvas), so peak RSS is that much higher than with `threads = 1`,
  which quantises in place.
- The fill rasteriser used to plant edges on the sub-scanline below a shape's
  bottom when that bottom fell exactly on a row boundary; fixed, so some
  shape bottoms differ slightly from older output (`fill_bottom_edge` case).
- Segmented DEFLATE costs about 0.2–0.5% in file size compared with one
  continuous stream; it is what makes the output thread-count independent.
- Allocation-failure injection covers the current corpus but is not exhaustive
  for every optional allocation.
- `fuzz/fuzz_run.c` uses `_exit`, so the crash runner cannot detect leaks; use
  `make -C fuzz leak` (now built with the plain libc allocator: the default
  slab arena hid small leaks from macOS `leaks`) or Linux ASan/LSan. Linux
  LSan found one pre-existing leak, now fixed: a `<use>` clone of
  gradient-filled text copied each chunk's gradient stops twice
  (`use_text_gradient` leak case).
- No coverage-guided fuzzing (libFuzzer/AFL++); the generators are
  structure-aware but not instrumented.
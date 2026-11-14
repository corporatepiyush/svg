# Conformance status

Where this renderer stands against the official specification, measured rather
than asserted.

Reference: **W3C Scalable Vector Graphics (SVG) 1.1, Second Edition,
Recommendation of 16 August 2011**. Element list: Appendix L. Attribute list:
Appendix M. Test suite: the W3C SVG 1.1 test suite (525 accepted tests).

## Headline

| measure | result |
|---|---|
| Elements instantiated | **18 / 80** (23%) |
| Presentation attributes honoured | **24 / 59** (41%) |
| Official W3C tests matching Chrome | **261 / 512 comparable** (51%) |
| Text-only tests (not comparable) | 13 |
| Conformance corpus (hand-written) | 85 / 85 |

The official-suite number is the honest one: it is measured against an
independent implementation of the same specification, not against our own
output.

## How the official suite is measured

`conformance/w3c_chrome.py` downloads the W3C suite on first use (14 MB, not
committed), renders every `svg/*.svg` with this renderer and with Chrome, and
compares the two images.

- **Why Chrome and not the suite's PNGs.** The committed reference images were
  produced by one renderer whose strokes snap to the pixel grid. A scanline
  rasteriser centres a 1-unit stroke on the path and gives half coverage to
  each pixel it touches, so a direct comparison measures the reference
  renderer's antialiasing, not conformance. Chrome is an independent
  implementation of the same specification.
- **Tolerance.** 16 per channel, and at most 3% of compared pixels may differ.
- **Text is masked.** Every test carries a revision label and most draw text.
  This renderer uses a built-in 5×7 bitmap font; glyph rasterisation cannot
  match a system font and would mask the geometry under test.
- **Text-only tests are excluded.** 13 tests leave fewer than 5,000 comparable
  pixels once text is masked; they are reported separately, not counted as
  passes.

```sh
python3 conformance/w3c_chrome.py --json /tmp/w3c.json
```

## Result by specification chapter

| spec chapter | tests | comparable | pass | rate |
|---|---:|---:|---:|---:|
| 15 Filter effects | 39 | 39 | 4 | 10% |
| 18 Scripting | 7 | 7 | 2 | 29% |
| 4 Data types | 13 | 13 | 4 | 31% |
| 19 Animation | 73 | 73 | 26 | 36% |
| 6 Styling | 17 | 17 | 7 | 41% |
| 13 Gradients and patterns | 30 | 30 | 13 | 43% |
| 10 Text | 62 | 52 | 25 | 48% |
| 16 Interactivity | 29 | 29 | 14 | 48% |
| 3 Rendering model | 8 | 8 | 4 | 50% |
| 11 Painting (markers) | 31 | 31 | 16 | 52% |
| 7 Coordinates and units | 32 | 32 | 19 | 59% |
| 14 Clipping and masking | 19 | 18 | 11 | 61% |
| 5 Document structure | 68 | 67 | 44 | 66% |
| 12 Color | 6 | 6 | 4 | 67% |
| 8 Paths | 21 | 21 | 14 | 67% |
| 17 Linking | 12 | 12 | 9 | 75% |
| 20 Fonts | 17 | 16 | 12 | 75% |
| 9 Basic shapes | 22 | 22 | 20 | 91% |
| App. G/B, metadata, unlabelled, misc | 19 | 19 | 13 | 68% |

Two readings matter:

- **Text and font rates are not evidence of text conformance.** The glyphs are
  masked out, so a pass there means the non-text content matches. Text itself
  is unimplemented beyond a bitmap font.
- **Scripting passes because Chrome does not run scripts in an `<img>` either.**
  Both render the static content.

## Element coverage (Appendix L)

| chapter | elements | implemented |
|---|---:|---|
| 5 Document structure | 10 | 6 — `svg g defs symbol use a`; `desc`/`title` are correctly non-rendering |
| 6 Styling | 1 | 0 — `<style>`; inline `style=` is supported |
| 8 Paths | 1 | 1 |
| 9 Basic shapes | 6 | 6 |
| 10 Text | 8 | 1 — `text` only |
| 11 Painting (markers) | 1 | 0 |
| 12 Color | 1 | 0 — `color-profile` |
| 13 Gradients and patterns | 4 | 3 — `linearGradient`, `radialGradient`, `stop`; `pattern` missing |
| 14 Clipping and masking | 2 | 1 — `mask` missing |
| 15 Filter effects | 25 | 0 |
| 16 Interactivity | 1 | 0 — `cursor` |
| 17 Linking | 2 | 0 — `image`, `view` |
| 18 Scripting | 1 | 0 (by design) |
| 19 Animation | 6 | 0 |
| 20 Fonts | 10 | 0 |
| 21 Metadata | 1 | 0 (by design) |
| **total** | **80** | **18** |

## Attribute coverage

- **Presentation attributes (Appendix M.2): 24 of 59.** Supported: `fill`,
  `fill-rule`, `fill-opacity`, `stroke`, `stroke-width`, `stroke-opacity`,
  `stroke-linecap`, `stroke-linejoin`, `stroke-miterlimit`,
  `stroke-dasharray`, `stroke-dashoffset`, `opacity`, `color`, `clip-path`,
  `clip-rule`, `display`, `visibility`, `font-size`, `font-style`,
  `font-weight`, `letter-spacing`, `text-anchor`, `stop-color`,
  `stop-opacity`. The other 35 are unimplemented (see below).
- **Regular attributes (Appendix M.1):** geometry, `transform`, `viewBox`,
  `preserveAspectRatio`, `gradientUnits`, `gradientTransform`, `spreadMethod`,
  `fx`/`fy`, `offset`, `clipPathUnits`, `href`/`xlink:href` are honoured on the
  elements that support them. Missing on otherwise-supported elements:
  `pathLength` (affects dash scaling), `overflow` on nested `<svg>`,
  `zoomAndPan`, and the text layout attributes `dx`, `dy`, `rotate`,
  `textLength`, `lengthAdjust`.

## What is still pending, in order of size

1. **Filter effects (chapter 15, 25 elements, 39 tests at 10%).** `<filter>`
   and all 24 `fe*` primitives, plus the `filter` property and
   `color-interpolation-filters`. Nothing is implemented.
2. **Animation (chapter 19, 6 elements, 73 tests at 36%).** SMIL is absent. A
   static rasteriser renders the base value; tests whose first frame depends on
   an animation fail.
3. **Text and fonts (chapters 10 and 20, 18 elements, 79 tests).** No
   `tspan`, `tref`, `textPath`, `altGlyph*`, `glyphRef`; no `dx`, `dy`,
   `rotate`, `textLength`, `lengthAdjust`, `writing-mode`,
   `dominant-baseline`; no font files, shaping, or font selection. The current
   text is a 5×7 bitmap font.
4. **Patterns and masks (chapters 13 and 14, 2 elements).** `<pattern>` with
   `patternUnits`, `patternContentUnits`, `patternTransform`; `<mask>` with
   `maskUnits`, `maskContentUnits`, the `mask` property, `enable-background`.
5. **Markers (chapter 11).** `<marker>`, `marker-start`, `marker-mid`,
   `marker-end`, `refX`, `refY`, `markerUnits`, `orient`.
6. **Images and views (chapter 17).** `<image>` (raster and SVG-in-SVG) and
   `<view>`.
7. **Styling (chapter 6).** `<style>`, `class`, selectors, at-rules,
   specificity, cascade, `!important`.
8. **Document structure long tail.** `<switch>` with `requiredFeatures`,
   `requiredExtensions`, `systemLanguage`; `overflow` on nested viewports.
9. **Interactivity and metadata.** `cursor`, `pointer-events`,
   `color-interpolation`, `color-rendering`, `shape-rendering`,
   `text-rendering`, `image-rendering` — hints that do not affect geometry, and
   `<script>` (correctly not executed).

## What this measurement changed

Running the official suite found two defects that the hand-written corpus had
not caught, because both live on antialiased edges:

- **Coverage was spread over whole pixels.** A horizontal run of sub-pixel
  samples was added to every pixel the run touched, so a run ending
  mid-pixel covered that pixel completely. Every antialiased edge was dilated
  by up to one pixel: a 1-unit vertical stroke at an integer coordinate
  rendered 255/255 instead of 128/128. Fixed in `raster_fill.c`.
- **`<use>` inherited only some properties.** `stroke-width`, caps, joins,
  miter limit, dash pattern and offset, fill rule, clip rule and the opacity
  properties are inherited in SVG 1.1 §5.6 but were not propagated into the
  shadow tree. Fixed in `svg_refs.c`.

Both fixes were validated against Chrome, and the per-assertion external
verifier now reports 175 of 179 sampled assertions in agreement with 0
disagreements.
# Scope, compatibility and resource limits

`svg2img` is an **SVG-only** rasterizer.  It is not a browser and does not
implement HTML, CSS selectors, at-rules, specificity, or cascade.  This file
is the contract between the library and its callers.

## Supported

- XML documents with an `<svg>` root: elements, attributes, text, comments,
  CDATA, DOCTYPE, processing instructions, entities.
- Shapes: `rect` (`rx`/`ry`), `circle`, `ellipse`, `line`, `polyline`,
  `polygon`, `path`.
- Paths: all SVG path commands, arc-to-cubic conversion, adaptive flattening.
- Structure: `g`, `a`, `defs`, `symbol`, `use` (basic `x`/`y`/`transform`,
  style inheritance and forward references).
- Paint: solid colours, `linearGradient`, `radialGradient` (focal point),
  `stop`, `spreadMethod`, `gradientUnits`, `gradientTransform`.
- Clipping: `clipPath` with `userSpaceOnUse`/`objectBoundingBox`,
  `clip-rule`, `fill-rule`.
- Transforms: `matrix`, `translate`, `scale`, `rotate`, `skewX`, `skewY`.
- Viewport: root and nested `<svg>` `x`/`y`/`width`/`height`/`viewBox` and
  `preserveAspectRatio` (meet/slice, all nine alignments).
- Units: user units, `px`, `pt`, `pc`, `in`, `cm`, `mm`, `em`, `ex`, `%`.
- Styling: presentation attributes, inline `style="..."` declarations, and a
  CSS subset in `<style>`: type, class, id and universal selectors, descendant
  and child combinators, comma-separated selector lists, and `!important`.
  The cascade follows SVG 1.1 §6.3 (presentation attributes < author rules <
  inline style < `!important` author rules).  At-rules, pseudo-classes,
  attribute selectors and media queries are not supported.
- Structure: `<switch>` with `requiredFeatures`, `requiredExtensions` and
  `systemLanguage`; `overflow` on nested viewports.
- Paint servers: `linearGradient`, `radialGradient` and `pattern`
  (`patternUnits`, `patternContentUnits`, `patternTransform`, viewBox).
- Clipping and masking: `clipPath` and `<mask>` (luminance × alpha,
  `maskUnits`, `maskContentUnits`).
- Markers: `<marker>` with `markerUnits`, `refX`/`refY`, `orient`,
  `markerWidth`/`markerHeight` and `marker-start`/`mid`/`end`.
- Colour: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`,
  `hsl()`, `hsla()`, `transparent`, `currentColor`, 147 named colours.
- Stroke: caps, joins, miter limit, dash arrays/offsets.
- Text: built-in 5×7 bitmap font, bold/italic, anchors, letter-spacing.
- Output: RGBA8 PNG, 1×–8× supersampling, optional background.

## Unsupported / ignored

- CSS at-rules, pseudo-classes, attribute selectors, media queries and
  `!important` in inline `style` (author `!important` is honoured).
- Geometry properties through CSS (`x`, `y`, `width`, `height`, `r`, `d`,
  `points`, `transform`).  Use XML attributes.
- `<image>`, `<filter>` (all 24 `fe*` primitives), `<foreignObject>`,
  `textPath`, `tspan` positioning, `dx`/`dy`, `textLength`, `lengthAdjust`,
  `dominant-baseline`, `writing-mode`, and SMIL animation.  `<script>` is not
  executed (correct for a rasteriser).
- Real font files and text shaping.  Text uses the built-in bitmap font.
- Full `<use>` shadow-tree styling beyond the implemented inheritance rules.

Unsupported elements are ignored rather than treated as errors, so a document
containing them still renders the supported subset.

## Resource limits

All limits live in `svg2img_limits` and are zero-initialised by default
(`memset(&opts, 0, sizeof opts)`).  A non-positive field selects the
compiled-in default.

| Field | Default | Behaviour when exceeded |
|---|---:|---|
| `max_nodes` | 200000 | stop building the subtree |
| `max_depth` | 1024 | stop building deeper nodes |
| `max_path_points` | 500000 | truncate the path |
| `max_path_subpaths` | 100000 | truncate the path |
| `max_gradient_stops` | 256 | ignore remaining stops |
| `max_text_length` | 100000 | truncate text |
| `max_clip_depth` | 16 | ignore deeper clip paths |
| `max_layers` | 64 | composite children directly |
| `max_use_expansion` | 20000 | stop cloning further `<use>` nodes |
| `max_pixels` | 64 Mpx | fail with `SVG2IMG_ERR_LIMIT` |

Limits never corrupt memory; they degrade the result or fail cleanly.  The
environment variables `SVG2IMG_LIMIT_MAX_*` can override individual limits in
the CLI for testing.

## API contract

- `svg_data`, `out_path` and `opts` are borrowed for the duration of the call.
- The library retains no global mutable state; separate calls are re-entrant.
- Return codes: `SVG2IMG_OK`, `SVG2IMG_ERR_FORMAT`, `SVG2IMG_ERR_NOMEM`,
  `SVG2IMG_ERR_ARG`, `SVG2IMG_ERR_IO`, `SVG2IMG_ERR_LIMIT`.
- Allocation failure is handled at every renderer allocation site; the
  allocation-failure harness in `fuzz/alloc_fail.py` exercises this.

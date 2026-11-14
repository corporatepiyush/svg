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
- Images: `<image>` with embedded `data:image/png` and `data:image/svg+xml`
  URIs, and external PNG and baseline JPEG files (see below).  `feImage` takes
  the same references.
- Filters: `<filter>` with `filterUnits`, `primitiveUnits`,
  `color-interpolation-filters`, `x`/`y`/`width`/`height`, href chaining, and
  all 24 `fe*` primitives — `feBlend`, `feColorMatrix`,
  `feComponentTransfer` (+ `feFuncR/G/B/A`), `feComposite`,
  `feConvolveMatrix`, `feDiffuseLighting`, `feDisplacementMap`,
  `feDistantLight`, `feFlood`, `feGaussianBlur`, `feImage`, `feMerge` /
  `feMergeNode`, `feMorphology`, `feOffset`, `fePointLight`,
  `feSpecularLighting`, `feSpotLight`, `feTile`, `feTurbulence`.  The
  `filter` property accepts `url(#id)`; `none` disables it.
- Rendering hints: `shape-rendering="crispEdges"` (and `optimizeSpeed`)
  disables antialiasing for that element, and `color-interpolation` on a
  gradient selects sRGB or linearRGB stop interpolation.
- Colour: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`,
  `hsl()`, `hsla()`, `transparent`, `currentColor`, 147 named colours.
- Stroke: caps, joins, miter limit, dash arrays/offsets.
- Text: built-in 5×7 bitmap font, bold/italic, anchors, letter-spacing,
  `<tspan>` `x`/`y`/`dx`/`dy` and per-run fill, `rotate` (one angle per
  glyph), `textLength`/`lengthAdjust`, `writing-mode="tb"`, `dominant-baseline`
  (as an em offset), `tref`, `textPath` (`href` or `path`, `startOffset`) and
  `altGlyph` (rendered as its fallback text).
- Output: RGBA8 PNG, 1×–8× supersampling, optional background.

## Unsupported / ignored

- CSS at-rules, pseudo-classes, attribute selectors, media queries and
  `!important` in inline `style` (author `!important` is honoured).
- Geometry properties through CSS (`x`, `y`, `width`, `height`, `r`, `d`,
  `points`, `transform`).  Use XML attributes.
- `<foreignObject>`, `<view>`, SMIL animation, and `<script>` (which is not
  executed, correctly for a rasteriser).
- `<color-profile>` and `cursor` are accepted but ignored: output is always
  sRGB and there is no pointer.  `text-rendering`, `color-rendering` and
  `image-rendering` are hints this rasteriser does not act on (text uses one
  bitmap font, colour is always sRGB, and images are always sampled
  nearest-neighbour).
- Text details: no font selection or shaping, so `font-family`, `font-stretch`
  and `font-variant` are ignored and every glyph comes from the built-in
  5×7 font; `textLength` distributes spacing per glyph rather than per
  character cluster; `dominant-baseline` is approximated as an em offset; and
  a `textPath` distance is not calibrated by the referenced path's
  `pathLength`.
- Filter details: a primitive's own `x`/`y`/`width`/`height` subregion is
  honoured only by `feImage`; other primitives use the whole filter region.
  `feTurbulence` uses a value-noise approximation rather than the reference
  gradient noise, so its exact pattern differs from Chrome's.  All primitives
  that the specification defines on premultiplied values do so here.
  `feConvolveMatrix` always premultiplies (it ignores `preserveAlpha`).  The
  filter output is clipped to the filter region.
- External image files need a base path: `svg2img_options.base_path` resolves
  a relative reference, and `svg2img_convert_file` defaults it to the input
  document's own directory.  `svg2img_convert` (a document in memory) has no
  default, so a relative reference is skipped unless the caller sets one.
  Progressive, arithmetic-coded and 12-bit JPEG are not supported (baseline
  sequential, Huffman, 8-bit only).  JPEG chroma is upsampled nearest-neighbour
  rather than with a triangle filter.
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

Filter chains are additionally bounded: the filter region is clipped to the
canvas, the chain may open at most `max_layers` filter buffers, and the total
filter buffer memory is capped at four times the region size plus 1 MiB.  A
chain that exceeds the budget is skipped and the element is drawn unfiltered.
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

/* svg2img.h - Public API for the pure C17 SVG to PNG converter.
 *
 * Converts a subset of SVG 1.1 to a PNG image using only libc and libm.
 * No external dependencies.
 *
 * See README.md for the supported/unsupported feature matrix and the
 * resource-limit defaults.  The conformance baseline is the SVG 1.1 spec at
 * docs/SVG11-single-page.html.  See the doc comments below for the ownership
 * and error-handling contract.
 */
#ifndef SVG2IMG_H
#define SVG2IMG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SVG2IMG_VERSION "1.0.0"

#ifdef SVG2IMG_STATIC
#define SVG2IMG_API static
#else
#define SVG2IMG_API
#endif

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

enum {
    SVG2IMG_OK            = 0,  /* success                                */
    SVG2IMG_ERR_FORMAT    = 1,  /* not XML / no <svg> root / empty input  */
    SVG2IMG_ERR_NOMEM     = 2,  /* an allocation failed                   */
    SVG2IMG_ERR_ARG       = 3,  /* invalid argument (NULL data/path)      */
    SVG2IMG_ERR_IO        = 4,  /* input could not be read / PNG not written */
    SVG2IMG_ERR_LIMIT     = 5   /* a resource limit was exceeded            */
};

/* ------------------------------------------------------------------ */
/* Resource limits                                                     */
/* ------------------------------------------------------------------ */

/* Upper bounds on one kind of work or memory.  A field set to 0 (or a
 * negative value) selects the compiled-in default shown in the comment.
 * Limits are enforced by truncating the work (never by corrupting memory),
 * and are reported through the return code: exceeding a limit that prevents
 * producing a correct image yields SVG2IMG_ERR_LIMIT. */
typedef struct {
    int max_nodes;          /* render-tree nodes built             (200000) */
    int max_depth;          /* element nesting depth               (1024)   */
    int max_path_points;    /* points kept per path                (500000) */
    int max_path_subpaths;  /* subpaths kept per path              (100000) */
    int max_gradient_stops; /* <stop> children per gradient        (256)   */
    int max_text_length;    /* characters kept per <text>          (100000) */
    int max_clip_depth;     /* nested clip-path levels             (16)    */
    int max_layers;         /* opacity layers per render           (64)    */
    int max_use_expansion;  /* nodes created by <use> cloning      (20000) */
    int max_pixels;         /* output pixels, width*height         (64 Mpx) */
} svg2img_limits;

typedef struct {
    int          width;          /* output width  in px, 0 = derive from SVG */
    int          height;         /* output height in px, 0 = derive from SVG */
    float        scale;          /* extra uniform scale (1.0 = none)          */
    int          aa;             /* supersampling factor 1..8 (default 4)     */
    unsigned int background;     /* 0xRRGGBBAA, used when has_background != 0 */
    int          has_background;
    int          verbose;
    /* Resource limits.  Zero-initialised (the usual memset(&o,0,...)) means
     * "all defaults", so existing callers need no change.  Added at the end
     * of the struct; recompilation required (the struct grew). */
    svg2img_limits limits;
    /* Base directory for resolving relative <image> references.  NULL (the
     * default) disables external file loading.  A relative base path is
     * resolved against the process working directory.  Only regular files
     * inside this directory are read: absolute hrefs, URL schemes, ".."
     * segments and symlinks resolving outside it are refused. */
    const char* base_path;
    /* TrueType font file for text.  NULL (the default) uses the built-in 5x7
     * bitmap font.  Loaded once per conversion and freed with the document. */
    const char* font_path;
    /* Worker threads for rasterisation and PNG encoding.  0 or 1 = render on
     * the calling thread only (the default, so a host that already runs
     * conversions in parallel is not oversubscribed); N > 1 splits the image
     * into row bands and compresses fixed-size segments in parallel.  The
     * output is identical for every value. */
    int threads;
} svg2img_options;

/* Fill *out* with the built-in default limits.  Never fails. */
SVG2IMG_API void svg2img_default_limits(svg2img_limits* out);

/* ------------------------------------------------------------------ */
/* Conversion                                                          */
/* ------------------------------------------------------------------ */

/* Convert an in-memory SVG document to a PNG file.
 *
 * Ownership: svg_data is borrowed for the duration of the call only; nothing
 *             is retained.  out_path is borrowed.  opts is borrowed and may
 *             be NULL (defaults).  On success the PNG at out_path is
 *             complete and self-contained.
 * Thread safety: the library keeps no global mutable state, so calls are
 *             re-entrant from multiple threads as long as each call gets its
 *             own options, paths and buffers.  The single-process CLI is not
 *             multi-threaded, but a host app may call this concurrently.
 * Returns: SVG2IMG_OK (0) on success, another SVG2IMG_ERR_* on failure. */
SVG2IMG_API int svg2img_convert(const char* svg_data, size_t len,
                                const char* out_path,
                                const svg2img_options* opts);

/* Read in_path, convert it, and write out_path.  Same contract as above. */
SVG2IMG_API int svg2img_convert_file(const char* in_path, const char* out_path,
                                     const svg2img_options* opts);

#ifdef __cplusplus
}
#endif

#endif /* SVG2IMG_H */
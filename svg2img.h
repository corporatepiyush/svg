/* svg2img.h - Public API for the pure C17 SVG to PNG converter.
 *
 * Converts a subset of SVG 1.1 to a PNG image using only libc and libm.
 * No external dependencies, no dynamic allocation limits beyond libc.
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

typedef struct {
    int          width;          /* output width  in px, 0 = derive from SVG */
    int          height;         /* output height in px, 0 = derive from SVG */
    float        scale;          /* extra uniform scale (1.0 = none)          */
    int          aa;             /* supersampling factor 1..8 (default 4)     */
    unsigned int background;     /* 0xRRGGBBAA, used when has_background != 0 */
    int          has_background;
    int          verbose;
} svg2img_options;

/* Convert an in-memory SVG document to a PNG file.
 * Returns 0 on success, non-zero on failure. */
SVG2IMG_API int svg2img_convert(const char* svg_data, size_t len,
                                const char* out_path,
                                const svg2img_options* opts);

/* Read in_path, convert it, and write out_path. */
SVG2IMG_API int svg2img_convert_file(const char* in_path, const char* out_path,
                                     const svg2img_options* opts);

#ifdef __cplusplus
}
#endif

#endif /* SVG2IMG_H */
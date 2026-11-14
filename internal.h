/* internal.h - Shared internal types and helpers.
 *
 * Not part of the public API.  Included by the implementation files only.
 */
#ifndef SVG2IMG_INTERNAL_H
#define SVG2IMG_INTERNAL_H

#include "svg2img.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SIMD intrinsics, when the compiler targets a CPU that has them. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define SVG2IMG_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define SVG2IMG_SSE2 1
#endif

#ifndef SVG_PI
#define SVG_PI 3.14159265358979323846f
#endif

/* Limits travel in the public options struct (svg2img.h).  These macros name
 * the compiled-in defaults for the implementation files that do not otherwise
 * include the public header directly. */
#define SVG2IMG_LIMIT_MAX_NODES          200000
#define SVG2IMG_LIMIT_MAX_DEPTH          1024
#define SVG2IMG_LIMIT_MAX_PATH_POINTS    500000
#define SVG2IMG_LIMIT_MAX_PATH_SUBPATHS  100000
#define SVG2IMG_LIMIT_MAX_GRADIENT_STOPS 256
#define SVG2IMG_LIMIT_MAX_TEXT_LENGTH    100000
#define SVG2IMG_LIMIT_MAX_CLIP_DEPTH     16
#define SVG2IMG_LIMIT_MAX_LAYERS         64
#define SVG2IMG_LIMIT_MAX_USE_EXPANSION  20000
#define SVG2IMG_LIMIT_MAX_PIXELS         (64 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

/* svg_malloc / svg_realloc / svg_strdup / svg_strndup are the only allocation
 * points in the renderer.  In a normal build they are `static inline` wrappers
 * around libc, so there is no indirect call in production.  Test builds that
 * define SVG2IMG_ALLOC_HOOKS route them through function pointers so a test
 * can inject allocation failure; that switch is compiled out by default and
 * costs production nothing. */
#ifdef SVG2IMG_ALLOC_HOOKS
typedef void* (*svg_alloc_fn)(size_t);
typedef void* (*svg_realloc_fn)(void*, size_t);
void  svg_set_alloc_hooks(svg_alloc_fn alloc, svg_realloc_fn realloc_fn);
void* svg_malloc(size_t n);
void* svg_realloc(void* p, size_t n);
#else
static inline void* svg_malloc(size_t n) { return malloc(n ? n : 1); }
static inline void* svg_realloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
#endif

static inline char* svg_strdup(const char* s) {
    if (!s) return NULL;                 /* tolerate a failed earlier alloc */
    size_t n = strlen(s);
    char* r = (char*)svg_malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static inline char* svg_strndup(const char* s, size_t n) {
    char* r = (char*)svg_malloc(n + 1);
    if (r) { memcpy(r, s, n); r[n] = 0; }
    return r;
}

/* ------------------------------------------------------------------ */
/* Math                                                                */
/* ------------------------------------------------------------------ */

typedef struct { float x, y; } Pt;

/* Affine matrix in SVG order: x' = a*x + c*y + e, y' = b*x + d*y + f */
typedef struct { float m[6]; } Mat;

static inline Mat mat_identity(void) { Mat m = {{1, 0, 0, 1, 0, 0}}; return m; }

static inline Mat mat_mul(Mat a, Mat b) {
    Mat r;
    r.m[0] = a.m[0] * b.m[0] + a.m[2] * b.m[1];
    r.m[1] = a.m[1] * b.m[0] + a.m[3] * b.m[1];
    r.m[2] = a.m[0] * b.m[2] + a.m[2] * b.m[3];
    r.m[3] = a.m[1] * b.m[2] + a.m[3] * b.m[3];
    r.m[4] = a.m[0] * b.m[4] + a.m[2] * b.m[5] + a.m[4];
    r.m[5] = a.m[1] * b.m[4] + a.m[3] * b.m[5] + a.m[5];
    return r;
}

static inline Pt mat_apply(Mat m, Pt p) {
    Pt r;
    r.x = m.m[0] * p.x + m.m[2] * p.y + m.m[4];
    r.y = m.m[1] * p.x + m.m[3] * p.y + m.m[5];
    return r;
}

static inline Mat mat_translate(float x, float y) { Mat m = {{1, 0, 0, 1, x, y}}; return m; }
static inline Mat mat_scale(float x, float y)     { Mat m = {{x, 0, 0, y, 0, 0}}; return m; }

static inline Mat mat_rotate(float deg) {
    float r = deg * SVG_PI / 180.0f;
    float c = cosf(r), s = sinf(r);
    Mat m = {{c, s, -s, c, 0, 0}};
    return m;
}

static inline Mat mat_skew_x(float deg) {
    float t = tanf(deg * SVG_PI / 180.0f);
    Mat m = {{1, 0, t, 1, 0, 0}};
    return m;
}

/* Mean scale factor of the matrix (sqrt |det|). */
static inline float mat_scale_factor(Mat m) {
    return sqrtf(fabsf(m.m[0] * m.m[3] - m.m[1] * m.m[2]));
}

/* Inverse of an affine matrix.  Returns 0 (and leaves *out untouched) when
 * the matrix is singular or non-finite, so callers can fall back to treating
 * the gradient as already being in device space. */
static inline int mat_invert_checked(Mat m, Mat* out) {
    float det = m.m[0] * m.m[3] - m.m[1] * m.m[2];
    if (!isfinite(det) || fabsf(det) < 1e-20f) return 0;
    float id = 1.0f / det;
    Mat r;
    r.m[0] =  m.m[3] * id;
    r.m[1] = -m.m[1] * id;
    r.m[2] = -m.m[2] * id;
    r.m[3] =  m.m[0] * id;
    r.m[4] = (m.m[2] * m.m[5] - m.m[3] * m.m[4]) * id;
    r.m[5] = (m.m[1] * m.m[4] - m.m[0] * m.m[5]) * id;
    for (int i = 0; i < 6; i++)
        if (!isfinite(r.m[i])) return 0;
    if (out) *out = r;
    return 1;
}

static inline Mat mat_invert(Mat m) {
    Mat r = mat_identity();
    (void)mat_invert_checked(m, &r);
    return r;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

typedef struct { Pt* p; int n; int closed; } SubPath;
typedef struct {
    SubPath* s;
    int n, cap;
    int shared_points;   /* subpaths alias one external point buffer */
    void* pts_arena;     /* the shared buffer, freed by the owner */
} Path;

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

typedef struct { float r, g, b, a; } Color;
typedef struct { float off; Color c; } Stop;

enum {
    PAINT_NONE = 0,
    PAINT_COLOR = 1,
    PAINT_LINEAR = 2,
    PAINT_RADIAL = 3,
    PAINT_CURRENT_COLOR = 4,
    PAINT_REF = 5
};

enum { UNITS_BBOX = 0, UNITS_USER = 1 };
enum { SPREAD_PAD = 0, SPREAD_REFLECT = 1, SPREAD_REPEAT = 2 };

/* Bits recorded in Node.spec for the properties the element itself
 * specified (presentation attribute or inline style).  Only the inheritable
 * paint properties are tracked; use_apply_style() in svg_refs.c consults
 * these so a <use> never overwrites a value the referenced element set. */
enum {
    SPEC_NONE         = 0u,
    SPEC_FILL         = 1u << 0,
    SPEC_STROKE       = 1u << 1,
    SPEC_FONT_SIZE    = 1u << 2,
    SPEC_FONT_WEIGHT  = 1u << 3,
    SPEC_FONT_STYLE   = 1u << 4,
    SPEC_TEXT_ANCHOR  = 1u << 5,
    SPEC_LETTER_SP    = 1u << 6,
    SPEC_VISIBILITY   = 1u << 7,
    SPEC_STROKE_WIDTH = 1u << 8,
    SPEC_LINECAP      = 1u << 9,
    SPEC_LINEJOIN     = 1u << 10,
    SPEC_MITERLIMIT   = 1u << 11,
    SPEC_FILL_RULE    = 1u << 12,
    SPEC_CLIP_RULE    = 1u << 13,
    SPEC_FILL_OPACITY = 1u << 14,
    SPEC_STROKE_OPACITY = 1u << 15,
    SPEC_DASH         = 1u << 16,
    SPEC_DASH_OFFSET  = 1u << 17
};

typedef struct Paint {
    int    type;
    Color  color;
    int    units;
    float  x1, y1, x2, y2;      /* linear gradient */
    float  cx, cy, r, fx, fy;   /* radial gradient */
    Stop*  stops;
    int    nstops;
    int    spread;
    Mat    tm;                  /* gradientTransform */
    char*  ref;                 /* referenced element id, without '#' */
} Paint;

/* ------------------------------------------------------------------ */
/* Render tree                                                         */
/* ------------------------------------------------------------------ */

typedef struct Node Node;
typedef struct Canvas Canvas;
typedef struct Mask Mask;

enum {
    NODE_GROUP = 0,
    NODE_SHAPE = 1,
    NODE_TEXT = 2,
    NODE_CLIPDEF = 3,
    NODE_GRADDEF = 4,
    NODE_IGNORE = 5,
    NODE_SYMBOL = 6,      /* <symbol>: never drawn directly, only via <use> */
    NODE_TRANSPARENT = 7  /* unsupported element: no rendering, but its known
                           * children are still traversed */
};

struct Node {
    int   type;
    char* id;
    Mat   tm;
    float opacity;
    Node** kids;
    int   nkids;

    /* shape */
    Path* path;
    Paint fill, stroke;
    float sw;
    int   linecap;      /* 0 butt, 1 round, 2 square */
    int   linejoin;     /* 0 miter, 1 round, 2 bevel */
    float miterlimit;
    float* dash;
    int   ndash;
    float dashoff;
    int   fillrule;     /* 0 nonzero, 1 evenodd */
    float fillop, strokeop;
    int   visible;

    /* text */
    char* text;
    float fontsize;
    float letterspacing;
    int   bold, italic;
    int   anchor;       /* 0 start, 1 middle, 2 end */
    float x, y;

    /* clip */
    Node* clip;
    char* clip_ref;
    int   clip_units;
    int   clip_rule;

    /* display:none hides the whole subtree (traversal stops); visibility is
     * inherited per element and only suppresses that element's own painting. */
    int hidden_subtree;

    /* use */
    char* use_ref;
    float use_x, use_y;
    float use_w, use_h;    /* width/height on <use> for <symbol>/<svg> */

    /* Bitmask of inheritable properties the element specified itself, used
     * only when cloning a <use> target: the use site may supply the rest
     * (SVG 1.1 §5.6).  See SPEC_* in internal.h. */
    unsigned spec;

    /* root viewport, valid only on the root node */
    struct { int w, h; Mat tm; int valid; } vp;

    /* nested <svg> viewport: clip children to this rectangle in the node's
     * own (pre-transform) user space */
    int   vp_clip;
    float vp_x, vp_y, vp_w, vp_h;
    Mat   vp_matrix;        /* viewport mapping, combined with the transform */
    int   vp_matrix_valid;
    float symbol_vb[4];     /* symbol viewBox (x,y,w,h) */
    int   symbol_vb_valid;
    int   symbol_par_none, symbol_par_slice, symbol_par_xa, symbol_par_ya;
};

struct Canvas { int w, h; float* px; };   /* RGBA, straight alpha */
struct Mask   { int x0, y0, x1, y1; float* m; int empty; };

/* ------------------------------------------------------------------ */
/* XML                                                                 */
/* ------------------------------------------------------------------ */

typedef struct XMLNode {
    char*  name;
    char** attrs;     /* interleaved name, value, name, value ... */
    int    nattrs, capattrs;
    struct XMLNode** kids;
    int    nkids, capkids;
    char*  text;              /* accumulated character data */
    struct XMLNode* parent;
} XMLNode;

XMLNode*    xml_parse(const char* data, size_t len);
void        xml_free(XMLNode* n);
const char* xml_attr(const XMLNode* n, const char* name);
XMLNode*    xml_child(const XMLNode* n, const char* name);

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

Path* path_parse(const char* d);
Path* path_parse_lim(const char* d, const svg2img_limits* lim, int* limit_hit);
void  path_free(Path* p);
void  path_transform(Path* p, Mat m);
void  path_bbox(const Path* p, float out[4]);
Path* path_copy(const Path* p);

/* ------------------------------------------------------------------ */
/* Style (svg_style.c <-> svg_build.c)                                 */
/* ------------------------------------------------------------------ */

typedef struct Style {
    Paint fill, stroke;
    float stroke_width;
    int   linecap, linejoin;
    float miterlimit;
    int   fillrule;
    int   cliprule;      /* -1 = unset, 0 = nonzero, 1 = evenodd */
    float fillop, strokeop;
    Color color;
    float fontsize;
    int   bold, italic;
    int   anchor;
    float letterspacing;
    int   display_none;
    int   visibility_hidden;
    unsigned spec;          /* which properties this element specified */
    float dashoff;
    float* dash;
    int   ndash;
    int   saw_fill;        /* the element specified fill (for <use> cloning) */
    int   saw_stroke;      /* the element specified stroke                    */
} Style;

/* Evaluation context passed down the DOM while building the tree. */
typedef struct IdMap IdMap;

typedef struct Ctx {
    float       vw, vh, em;
    IdMap*      ids;
    const svg2img_limits* lim;   /* resolved limits for this conversion */
    int         nodes;           /* nodes built so far (max_nodes)       */
    int         limit_hit;       /* a hard limit was exceeded            */
} Ctx;

Style  style_default(void);
void   style_copy(Style* dst, const Style* src);
void   style_free(Style* s);
void   style_apply_decls(Ctx* ctx, Style* st, const char* decls);
int    style_get_decl(const char* decls, const char* key, char* out, size_t cap);

/* ------------------------------------------------------------------ */
/* Color / length (color.c, length.c)                                  */
/* ------------------------------------------------------------------ */

int   color_parse(const char* s, Color* out);
float length_parse(const char* s, float vw, float vh, float em, int axis, float def);

/* ------------------------------------------------------------------ */
/* Render tree building (svg_*.c)                                      */
/* ------------------------------------------------------------------ */

Node* svg_node_new(int type);
int   svg_resolve(Node* n, IdMap* ids, const svg2img_limits* lim);
Node* svg_parse(const char* data, size_t len, const svg2img_options* opts,
                int* limit_hit);
void  svg_free(Node* n);
void  limits_resolve(const svg2img_limits* in, svg2img_limits* out);

/* ------------------------------------------------------------------ */
/* Rasterization (raster_*.c)                                          */
/* ------------------------------------------------------------------ */

Canvas* canvas_create(int w, int h);
void    canvas_free(Canvas* c);
void    canvas_clear(Canvas* c, Color col);
void    render_document(Canvas* c, Node* root, Mat view_tm,
                        const svg2img_options* opts, const svg2img_limits* lim);

/* ------------------------------------------------------------------ */
/* PNG output (png_*.c)                                                */
/* ------------------------------------------------------------------ */

int png_save(const char* path, const float* rgba, int w, int h);

/* ------------------------------------------------------------------ */
/* Font (font.c)                                                       */
/* ------------------------------------------------------------------ */

int   font_glyph(int ch, unsigned char rows[7]);

#endif /* SVG2IMG_INTERNAL_H */
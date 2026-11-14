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
    PAINT_REF = 5,
    PAINT_PATTERN = 6
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
    Mat    tm;                  /* gradientTransform / patternTransform */
    char*  ref;                 /* referenced element id, without '#' */
    void*  pat;                 /* NODE_PATTERNDEF, when type == PAINT_PATTERN */
    int    linear;              /* color-interpolation="linearRGB" */
} Paint;

/* One positioned run of text (a <text> or <tspan> fragment). */
typedef struct {
    char*  text;
    float  x, y;
    float  fontsize;
    float  letterspacing;
    int    bold, italic, anchor;
    Paint  fill;
    /* Text layout (SVG 1.1 §10) */
    float  textlen;          /* textLength in user units, <= 0 = unset */
    int    length_kind;      /* 0 = spacing, 1 = spacingAndGlyphs */
    int    writing;          /* 0 = horizontal, 1 = vertical (tb) */
    float  baseline;         /* dominant-baseline offset, em fractions */
    float* rot;              /* per-glyph rotation, degrees */
    int    nrot;
    char*  path;             /* textPath data, or NULL */
    Path*  refpath;          /* textPath href="#id", resolved to geometry */
    char*  path_ref;         /* textPath href="#id", resolved later */
    float  start_offset;     /* textPath startOffset */
} TextChunk;

/* ------------------------------------------------------------------ */
/* Render tree                                                         */
/* ------------------------------------------------------------------ */

typedef struct Node Node;
typedef struct Canvas Canvas;
typedef struct Mask Mask;

/* ------------------------------------------------------------------ */
/* Filter effects (SVG 1.1 §15)                                        */
/* ------------------------------------------------------------------ */

enum {
    FE_BLEND = 0, FE_COLOR_MATRIX, FE_COMPONENT_TRANSFER, FE_COMPOSITE,
    FE_CONVOLVE_MATRIX, FE_DIFFUSE_LIGHTING, FE_DISPLACEMENT_MAP,
    FE_DISTANT_LIGHT, FE_FLOOD, FE_GAUSSIAN_BLUR, FE_IMAGE, FE_MERGE,
    FE_MERGE_NODE, FE_MORPHOLOGY, FE_OFFSET, FE_POINT_LIGHT,
    FE_SPECULAR_LIGHTING, FE_SPOT_LIGHT, FE_TILE, FE_TURBULENCE,
    FE_FUNC_R, FE_FUNC_G, FE_FUNC_B, FE_FUNC_A
};

enum { FE_CM_MATRIX = 0, FE_CM_SATURATE, FE_CM_HUE_ROTATE,
       FE_CM_LUMINANCE_TO_ALPHA };
enum { FE_CT_IDENTITY = 0, FE_CT_TABLE, FE_CT_DISCRETE, FE_CT_LINEAR,
       FE_CT_GAMMA };
enum { FE_COMP_OVER = 0, FE_COMP_IN, FE_COMP_OUT, FE_COMP_ATOP, FE_COMP_XOR,
       FE_COMP_ARITH };
enum { FE_BLEND_NORMAL = 0, FE_BLEND_MULTIPLY, FE_BLEND_SCREEN,
       FE_BLEND_DARKEN, FE_BLEND_LIGHTEN };
enum { FE_TURB_FRACTAL = 0, FE_TURB_TURBULENCE };
enum { FE_LIGHT_DISTANT = 0, FE_LIGHT_POINT, FE_LIGHT_SPOT };
enum { FE_CHAN_R = 0, FE_CHAN_G, FE_CHAN_B, FE_CHAN_A };

/* One filter primitive.  A flat struct rather than a union: the parser and
 * the rasteriser both want the whole parameter set, and the struct is only
 * allocated once per primitive. */
typedef struct FilterPrim {
    int   type;
    char* in;                 /* input name; NULL = previous result / SourceGraphic */
    char* in2;                /* second input (blend, composite, displacement) */
    char* result;             /* name this primitive's output is known by */

    float num[8];             /* x, y, width, height, dx, dy, scale, ... */
    float values[32];         /* feColorMatrix values / kernelMatrix */
    int   nvalues;
    float table[16];          /* feComponentTransfer tableValues */
    int   ntable;

    /* feComponentTransfer: one record per channel (R, G, B, A) */
    int   ct_type[4];
    float ct_slope[4], ct_intercept[4], ct_amplitude[4], ct_exponent[4],
          ct_offset[4];
    float ct_table[4][16];
    int   ct_ntable[4];

    /* feConvolveMatrix */
    int   order_x, order_y, edge_mode;
    float target_x, target_y;
    float divisor, bias;
    float kernel[32];
    int   nkernel;

    /* feDiffuseLighting / feSpecularLighting */
    Color light_color;
    float surface_scale, diffuse_constant, specular_constant, specular_exponent;
    int   light_type;
    float lx, ly, lz;         /* point/spot position */
    float azimuth, elevation; /* distant light direction */
    float points_at_x, points_at_y, points_at_z;
    float limiting_cone_angle;

    /* feTurbulence */
    int   turb_type, stitch;
    float base_freq_x, base_freq_y;
    int   octaves, seed;

    /* feImage */
    char* href;
    unsigned char* px;       /* preloaded raster file, or NULL */
    int   pw, ph;

    /* feMerge: the inputs of its feMergeNode children, in order */
    char* merge_in[8];
    int   nmerge;
} FilterPrim;

enum {
    NODE_GROUP = 0,
    NODE_SHAPE = 1,
    NODE_TEXT = 2,
    NODE_CLIPDEF = 3,
    NODE_GRADDEF = 4,
    NODE_IGNORE = 5,
    NODE_SYMBOL = 6,      /* <symbol>: never drawn directly, only via <use> */
    NODE_TRANSPARENT = 7, /* unsupported element: no rendering, but its known
                           * children are still traversed */
    NODE_PATTERNDEF = 8, /* <pattern>: tile definition, never painted directly */
    NODE_MASKDEF = 9,    /* <mask>: luminance mask, never painted directly */
    NODE_MARKERDEF = 10, /* <marker>: vertex marker, never painted directly */
    NODE_IMAGE = 11,     /* <image>: raster or embedded SVG image */
    NODE_FILTERDEF = 12  /* <filter>: filter chain, never painted directly */
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
    int   marker_ok;          /* markers apply to path/line/polyline/polygon */
    int   crisp;              /* shape-rendering="crispEdges": no AA          */
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
    TextChunk* chunks;
    int nchunks;
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

    /* <image>: href and placement (SVG 1.1 §5.6.4 / 12.2) */
    char*  image_href;
    float  img_x, img_y, img_w, img_h;
    unsigned char* img_px;    /* preloaded file image (PNG/JPEG), or NULL */
    int    img_iw, img_ih;

    /* <marker>: vertex marker (SVG 1.1 §11.6) */
    Node* marker_start, * marker_mid, * marker_end;
    char* marker_start_ref, * marker_mid_ref, * marker_end_ref;
    int   mk_units;           /* 0 = strokeWidth (default), 1 = userSpaceOnUse */
    float mk_w, mk_h, mk_refx, mk_refy;
    int   mk_orient;          /* 0 = auto, 1 = angle, 2 = auto-start-reverse */
    float mk_angle;
    float mk_vb[4]; int mk_vb_valid;
    int   mk_par_none, mk_par_slice, mk_par_xa, mk_par_ya;

    /* <mask>: luminance mask (SVG 1.1 §14.4) */
    Node* mask;               /* resolved <mask> definition, borrowed */
    char* mask_ref;
    int   mask_units;         /* UNITS_USER / UNITS_BBOX */
    int   mask_content_units;
    float mask_x, mask_y, mask_w, mask_h;

    /* <pattern>: tile definition (SVG 1.1 §13.3) */
    int   pat_units;          /* UNITS_USER / UNITS_BBOX */
    int   pat_content_units;
    float pat_x, pat_y, pat_w, pat_h;
    Mat   pat_tm;
    float pat_vb[4]; int pat_vb_valid;
    int   pat_par_none, pat_par_slice, pat_par_xa, pat_par_ya;

    /* <filter>: filter effects (SVG 1.1 §15) */
    Node* filter;             /* resolved <filter> definition, borrowed */
    char* filter_ref;
    int   filt_units;         /* UNITS_BBOX (default) / UNITS_USER */
    int   filt_prim_units;    /* UNITS_USER (default) / UNITS_BBOX */
    float filt_x, filt_y, filt_w, filt_h;
    int   filt_linear;        /* color-interpolation-filters: linearRGB */
    FilterPrim* prims;
    int   nprims;
    char* filt_href;          /* href of the referenced filter */
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
    char* marker_start;    /* marker-start/mid/end: id without '#', or NULL   */
    char* marker_mid;
    char* marker_end;
    char* filter;          /* filter: url(#id) -> id, or NULL                */
    int   crisp;           /* shape-rendering: no antialiasing              */
    float textlen;         /* textLength (not inherited)                     */
    int   length_kind;     /* lengthAdjust: 0 spacing, 1 spacingAndGlyphs    */
    int   writing;         /* writing-mode: 0 horizontal, 1 vertical         */
    float baseline;        /* dominant-baseline, em fractions                */
    float* rot;            /* glyph rotation list (not inherited)            */
    int   nrot;
} Style;

/* Evaluation context passed down the DOM while building the tree. */
typedef struct IdMap IdMap;

typedef struct CssRule CssRule;
typedef struct Ctx {
    float       vw, vh, em;
    IdMap*      ids;
    const svg2img_limits* lim;   /* resolved limits for this conversion */
    int         nodes;           /* nodes built so far (max_nodes)       */
    int         limit_hit;       /* a hard limit was exceeded            */
    CssRule*    rules;           /* <style> rules, in document order     */
    int         nrules, caprules;
    const char* base_path;       /* for relative <image> references, or NULL */
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
float path_length(const Path* p);

/* image_png.c / image_jpeg.c */
unsigned char* png_decode_rgba(const unsigned char* d, size_t n,
                               int* out_w, int* out_h);
unsigned char* jpeg_decode_rgba(const unsigned char* d, size_t n,
                                int* out_w, int* out_h);
/* image_load.c: data URIs and (with a base path) external files. */
int   image_load_any(const char* href, const char* base_path, int aa,
                     unsigned char** out, int* out_w, int* out_h);

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
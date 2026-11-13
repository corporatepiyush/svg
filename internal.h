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

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

static inline void* svg_malloc(size_t n) { return malloc(n ? n : 1); }
static inline void* svg_realloc(void* p, size_t n) { return realloc(p, n ? n : 1); }

static inline char* svg_strdup(const char* s) {
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
    NODE_IGNORE = 5
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

    /* use */
    char* use_ref;
    float use_x, use_y;

    /* root viewport, valid only on the root node */
    struct { int w, h; Mat tm; int valid; } vp;
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
    float dashoff;
    float* dash;
    int   ndash;
} Style;

/* Evaluation context passed down the DOM while building the tree. */
typedef struct IdMap IdMap;

typedef struct Ctx {
    float       vw, vh, em;
    IdMap*      ids;
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
void  svg_resolve(Node* n, IdMap* ids);
Node* svg_parse(const char* data, size_t len, const svg2img_options* opts);
void  svg_free(Node* n);

/* ------------------------------------------------------------------ */
/* Rasterization (raster_*.c)                                          */
/* ------------------------------------------------------------------ */

Canvas* canvas_create(int w, int h);
void    canvas_free(Canvas* c);
void    canvas_clear(Canvas* c, Color col);
void    render_document(Canvas* c, Node* root, Mat view_tm, const svg2img_options* opts);

/* ------------------------------------------------------------------ */
/* PNG output (png_*.c)                                                */
/* ------------------------------------------------------------------ */

int png_save(const char* path, const float* rgba, int w, int h);

/* ------------------------------------------------------------------ */
/* Font (font.c)                                                       */
/* ------------------------------------------------------------------ */

int   font_glyph(int ch, unsigned char rows[7]);

#endif /* SVG2IMG_INTERNAL_H */
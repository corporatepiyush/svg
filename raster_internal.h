/* raster_internal.h - Shared rasterizer internals. */
#ifndef RASTER_INTERNAL_H
#define RASTER_INTERNAL_H

#include "internal.h"

#ifndef SVG_MIN
#define SVG_MIN(a,b) ((a) < (b) ? (a) : (b))
#define SVG_MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

int svg_f2i(float v, int lo, int hi, int nan_val);

void blend(Canvas* c, int x, int y, Color s, float a);
void blend_row_const(Canvas* c, int x0, int y, int n, Color s, const float* alpha);
void composite_layer(Canvas* c, int dx, int dy, const float* src,
                     int lw, int lh, float op);

Mask* mask_alloc(int x0, int y0, int x1, int y1);
void  mask_free(Mask* m);
float mask_get(const Mask* m, int x, int y);
Mask* mask_shift(const Mask* m, int x0, int y0, int x1, int y1);
Mask* mask_translate(const Mask* m, int dx, int dy);
Mask* mask_intersect(const Mask* a, const Mask* b);

#define GRAD_LUT_N 1024
#define GRAD_LUT_MASK (GRAD_LUT_N - 1)

typedef struct {
    int type;
    int spread;
    int normalized;
    float bx0, by0, bw, bh;
    float x1, y1, dx, dy, invlen2;
    float cx, cy, r, fx, fy;
    float rdx, rdy, rlen, r_inv;
    int   has_inv;     /* userSpaceOnUse with an invertible CTM */
    Mat   inv;         /* inverse of the CTM at the referencing element */
    Color lut[GRAD_LUT_N];
} Grad;

void  grad_build(Grad* g, const Paint* p, const float bbox[4], Mat world);
Color grad_eval(const Grad* g, float x, float y);

void fill_path(Canvas* c, const Path* path, int rule, const Paint* paint,
               float opacity, const Mask* clip, const float bounds[4],
               const float grad_bbox[4], int aa, Mat world);
Path* build_stroke(const Path* path, float w, int cap, int join,
                   float miterlimit, const float* dash, int ndash, float dashoff);

void render_shape(Canvas* c, Node* n, Mat world, float opacity,
                  Mask* clip, int aa);
void render_text(Canvas* c, Node* n, Mat world, float opacity,
                 Mask* clip, int aa);

Mask* make_clip_mask(const Node* clipdef, Mat world, const float* obj_bbox,
                     int W, int H, int aa);
Mask* make_rect_mask(const Node* n, Mat w, int W, int H, int aa);
int node_bbox_world(Node* n, Mat full_w, float out[4]);

#endif

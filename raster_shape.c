/* raster_shape.c - Shape rendering (fill + stroke). */
#include "raster_internal.h"

/* ------------------------------------------------------------------ */
/* Shape / text rendering                                              */
/* ------------------------------------------------------------------ */

void render_shape(Canvas* c, Node* n, Mat world, float opacity,
                         Mask* clip, int aa) {
    if (!n->path || n->path->n == 0) return;

    /* Fast path: an identity CTM is extremely common (most shapes have no
     * transform of their own).  Skip the copy and the per-point transform
     * and rasterise the path in place. */
    int identity = (world.m[0] == 1.0f && world.m[1] == 0.0f &&
                    world.m[2] == 0.0f && world.m[3] == 1.0f &&
                    world.m[4] == 0.0f && world.m[5] == 0.0f);

    Path* p;
    if (identity) {
        p = n->path;
    } else {
        p = path_copy(n->path);
        if (!p) return;
        path_transform(p, world);
    }
    float bb[4];
    path_bbox(p, bb);

    /* Stroke geometry is built in device space, so lengths that were
     * authored in user units must be scaled by the CTM. */
    float ds = mat_scale_factor(world);

    if (n->fill.type != PAINT_NONE && n->fillop > 0.0f) {
        float op = opacity * n->fillop;
        fill_path(c, p, n->fillrule, &n->fill, op, clip, bb, bb, aa, world);
    }
    if (n->stroke.type != PAINT_NONE && n->strokeop > 0.0f && n->sw > 0.0f) {
        float* dash = NULL;
        int ndash = 0;
        if (n->dash && n->ndash > 0) {
            dash = (float*)svg_malloc((size_t)n->ndash * sizeof(float));
            if (dash) {
                for (int i = 0; i < n->ndash; i++) dash[i] = n->dash[i] * ds;
                ndash = n->ndash;
            }
        }
        Path* sp = build_stroke(p, n->sw * ds, n->linecap, n->linejoin,
                                n->miterlimit, dash, ndash, n->dashoff * ds);
        free(dash);
        if (sp) {
            float sbb[4];
            path_bbox(sp, sbb);
            float op = opacity * n->strokeop;
            fill_path(c, sp, 0, &n->stroke, op, clip, sbb, bb, aa, world);
            path_free(sp);
        }
    }
    if (!identity) path_free(p);
}

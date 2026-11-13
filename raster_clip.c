/* raster_clip.c - clipPath geometry, masks and nested clips. */
#include "raster_internal.h"

static void render_clip_shape(Canvas* c, const Node* n, Mat world, int rule, int aa) {
    if (n->type != NODE_SHAPE || !n->path || n->path->n == 0) return;
    Path* p = path_copy(n->path);
    if (!p) return;
    path_transform(p, world);
    float bb[4];
    path_bbox(p, bb);
    Paint white;
    memset(&white, 0, sizeof(white));
    white.type = PAINT_COLOR;
    white.color = (Color){1, 1, 1, 1};
    fill_path(c, p, rule, &white, 1.0f, NULL, bb, NULL, aa, world);
    path_free(p);
}

/* Collect the union bbox of a clipPath's geometry, in the coordinate
 * system established by `cw` (clipPath transform applied). */
static int clip_bbox(const Node* clipdef, Mat cw, float out[4]) {
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    int any = 0;
    for (int i = 0; i < clipdef->nkids; i++) {
        const Node* kid = clipdef->kids[i];
        if (!kid->visible) continue;
        Mat kw = mat_mul(cw, kid->tm);
        if (kid->type == NODE_SHAPE && kid->path && kid->path->n > 0) {
            Path* p = path_copy(kid->path);
            if (!p) continue;
            path_transform(p, kw);
            float kb[4];
            path_bbox(p, kb);
            path_free(p);
            if (kb[0] < out[0]) out[0] = kb[0];
            if (kb[1] < out[1]) out[1] = kb[1];
            if (kb[2] > out[2]) out[2] = kb[2];
            if (kb[3] > out[3]) out[3] = kb[3];
            any = 1;
        } else if (kid->type == NODE_GROUP) {
            float gb[4];
            if (clip_bbox(kid, kw, gb)) {
                if (gb[0] < out[0]) out[0] = gb[0];
                if (gb[1] < out[1]) out[1] = gb[1];
                if (gb[2] > out[2]) out[2] = gb[2];
                if (gb[3] > out[3]) out[3] = gb[3];
                any = 1;
            }
        }
    }
    if (!any) out[0] = out[1] = out[2] = out[3] = 0.0f;
    return any;
}

static void render_clip_node(Canvas* c, Node* n, Mat world, int rule, int aa) {
    if (!n || !n->visible) return;
    Mat w = mat_mul(world, n->tm);
    if (n->type == NODE_SHAPE) {
        render_clip_shape(c, n, w, rule, aa);
    } else if (n->type == NODE_GROUP) {
        int r = (n->clip_rule >= 0) ? n->clip_rule : rule;
        for (int i = 0; i < n->nkids; i++) render_clip_node(c, n->kids[i], w, r, aa);
    }
}

/* Rasterise a clipPath into an 8-bit alpha mask.  `obj_bbox` is the
 * bounding box of the referencing element, used when clipPathUnits is
 * objectBoundingBox. */
Mask* make_clip_mask(const Node* clipdef, Mat world,
                            const float* obj_bbox, int W, int H, int aa) {
    if (!clipdef) return NULL;

    Mat cw = mat_mul(world, clipdef->tm);
    if (clipdef->clip_units == UNITS_BBOX && obj_bbox) {
        float bw = obj_bbox[2] - obj_bbox[0];
        float bh = obj_bbox[3] - obj_bbox[1];
        if (bw < 1e-9f) bw = 1e-9f;
        if (bh < 1e-9f) bh = 1e-9f;
        cw = mat_mul(cw, mat_mul(mat_translate(obj_bbox[0], obj_bbox[1]),
                                 mat_scale(bw, bh)));
    }

    float bb[4];
    if (!clip_bbox(clipdef, cw, bb)) {
        /* An empty (or fully degenerate) clipPath clips everything away.
         * Return a zero mask over the whole canvas rather than NULL, which
         * callers interpret as "no clipping". */
        return mask_alloc(0, 0, W, H);
    }

    int x0 = svg_f2i(floorf(bb[0]), -1, W + 1, 0);
    int y0 = svg_f2i(floorf(bb[1]), -1, H + 1, 0);
    int x1 = svg_f2i(ceilf(bb[2]), -1, W + 1, 0);
    int y1 = svg_f2i(ceilf(bb[3]), -1, H + 1, 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return mask_alloc(0, 0, W, H);

    Canvas* tmp = canvas_create(x1 - x0, y1 - y0);
    if (!tmp) return NULL;
    canvas_clear(tmp, (Color){0, 0, 0, 0});
    Mat tw = mat_mul(mat_translate(-(float)x0, -(float)y0), cw);
    for (int i = 0; i < clipdef->nkids; i++)
        render_clip_node(tmp, clipdef->kids[i], tw, clipdef->clip_rule, aa);

    Mask* m = mask_alloc(x0, y0, x1, y1);
    if (!m) { canvas_free(tmp); return NULL; }
    size_t row = (size_t)(x1 - x0);
    int any = 0;
    for (int y = 0; y < y1 - y0; y++) {
        for (int x = 0; x < x1 - x0; x++) {
            float v = tmp->px[(size_t)(y * tmp->w + x) * 4 + 3];
            m->m[(size_t)y * row + (size_t)x] = v;
            if (v > 0.0f) any = 1;
        }
    }
    m->empty = !any;
    canvas_free(tmp);
    return m;
}

/* raster_node.c - Node bounds, opacity layers and tree traversal. */
#include "raster_internal.h"

/* ------------------------------------------------------------------ */
/* Node bounding boxes                                                 */
/* ------------------------------------------------------------------ */

/* Bounding box of a path in device space, without copying or transforming
 * it.  Transforming every point is unnecessary: the bbox of a transformed
 * path is the transform of its *control-point* bbox only for axis-aligned
 * matrices, so instead we transform the four corners of the local bbox and
 * take their extent.  That is a superset for rotations (never an
 * under-estimate) and exact for the common translate/scale case. */
static void path_bbox_transformed(const Path* p, Mat m, float out[4]) {
    float bb[4];
    path_bbox(p, bb);
    if (bb[0] > bb[2]) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
    Pt c[4] = { {bb[0], bb[1]}, {bb[2], bb[1]}, {bb[2], bb[3]}, {bb[0], bb[3]} };
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt q = mat_apply(m, c[i]);
        if (q.x < out[0]) out[0] = q.x;
        if (q.y < out[1]) out[1] = q.y;
        if (q.x > out[2]) out[2] = q.x;
        if (q.y > out[3]) out[3] = q.y;
    }
}

/* `full_w` is the node's complete transform (parent CTM * n->tm), not the
 * parent CTM.  Applying n->tm here as well would double-transform every
 * opacity layer that has its own transform. */
int node_bbox_world(Node* n, Mat full_w, float out[4]) {
    if (!n || !n->visible) return 0;
    if (n->type == NODE_SHAPE && n->path && n->path->n > 0) {
        path_bbox_transformed(n->path, full_w, out);
        if (n->stroke.type != PAINT_NONE && n->sw > 0.0f) {
            out[0] -= n->sw * 0.5f; out[1] -= n->sw * 0.5f;
            out[2] += n->sw * 0.5f; out[3] += n->sw * 0.5f;
        }
        return 1;
    }
    if (n->type == NODE_TEXT && n->text && *n->text && n->fontsize > 0.0f) {
        float scale = n->fontsize / 7.0f;
        float adv = 6.0f * scale + n->letterspacing;
        float total = 0.0f;
        for (const char* q = n->text; *q; q++) total += adv;
        float x = n->x;
        if (n->anchor == 1) x -= total * 0.5f;
        else if (n->anchor == 2) x -= total;
        float y = n->y;
        Pt corners[4] = {
            {x, y - 7.0f * scale}, {x + total, y - 7.0f * scale},
            {x + total, y}, {x, y}
        };
        out[0] = out[1] = 1e30f;
        out[2] = out[3] = -1e30f;
        for (int i = 0; i < 4; i++) {
            Pt q = mat_apply(full_w, corners[i]);
            if (q.x < out[0]) out[0] = q.x;
            if (q.y < out[1]) out[1] = q.y;
            if (q.x > out[2]) out[2] = q.x;
            if (q.y > out[3]) out[3] = q.y;
        }
        return 1;
    }
    if (n->type == NODE_GROUP) {
        int any = 0;
        out[0] = out[1] = 1e30f;
        out[2] = out[3] = -1e30f;
        for (int i = 0; i < n->nkids; i++) {
            float kb[4];
            Mat cw = mat_mul(full_w, n->kids[i]->tm);
            if (node_bbox_world(n->kids[i], cw, kb)) {
                if (kb[0] < out[0]) out[0] = kb[0];
                if (kb[1] < out[1]) out[1] = kb[1];
                if (kb[2] > out[2]) out[2] = kb[2];
                if (kb[3] > out[3]) out[3] = kb[3];
                any = 1;
            }
        }
        return any;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tree rendering                                                      */
/* ------------------------------------------------------------------ */

/* Build the clip mask contributed by this node's own clip-path, combined
 * with the inherited mask.
 *
 * Return values:
 *   - NULL        no clipping applies (no clip-path, or a clip path whose
 *                 geometry lies entirely outside the canvas)
 *   - *out_empty  set to 1 when a mask was produced but is entirely zero,
 *                 i.e. the element must be fully clipped away
 * The returned mask is always freshly allocated and must be mask_free()d. */
static Mask* node_clip_mask(const Node* n, Mat world, Canvas* c,
                            Mask* inherited, const float* obj_bbox, int aa,
                            int* out_empty) {
    if (out_empty) *out_empty = 0;
    if (!n->clip) return NULL;
    Mask* m = make_clip_mask(n->clip, world, obj_bbox, c->w, c->h, aa);
    if (!m) return NULL;
    if (!inherited) {
        if (m->empty) { mask_free(m); if (out_empty) *out_empty = 1; return NULL; }
        return m;
    }
    Mask* both = mask_intersect(inherited, m);
    mask_free(m);
    if (!both) { if (out_empty) *out_empty = 1; return NULL; }
    return both;
}

static void render_node(Canvas* c, Node* n, Mat world, float opacity,
                        Mask* clip, int aa);

/* Render a group's children into a temporary layer and composite it with
 * the group's opacity.  `op` is the already-composed opacity (group
 * opacity * inherited opacity), and must be < 1 for a layer to be needed. */
static void render_group_layered(Canvas* c, Node* n, Mat w, float op,
                                 Mask* eff, int aa) {
    float bb[4];
    if (!node_bbox_world(n, w, bb)) return;

    int x0 = svg_f2i(floorf(bb[0]), -1, c->w + 1, 0);
    int y0 = svg_f2i(floorf(bb[1]), -1, c->h + 1, 0);
    int x1 = svg_f2i(ceilf(bb[2]), -1, c->w + 1, 0);
    int y1 = svg_f2i(ceilf(bb[3]), -1, c->h + 1, 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) return;

    Canvas* tmp = canvas_create(x1 - x0, y1 - y0);
    if (!tmp) return;

    Mat tw = mat_mul(mat_translate(-(float)x0, -(float)y0), w);
    Mask* tclip = mask_shift(eff, x0, y0, x1, y1);
    for (int i = 0; i < n->nkids; i++)
        render_node(tmp, n->kids[i], tw, 1.0f, tclip, aa);
    mask_free(tclip);

    composite_layer(c, x0, y0, tmp->px, tmp->w, tmp->h, op);
    canvas_free(tmp);
}

static void render_node(Canvas* c, Node* n, Mat world, float opacity,
                        Mask* clip, int aa) {
    if (!n || !n->visible) return;
    Mat w = mat_mul(world, n->tm);
    float op = opacity * n->opacity;
    if (op <= 0.0f) return;

    /* objectBoundingBox clip paths resolve against the element's own
     * bounding box, so compute it before building the mask. */
    float bb[4];
    int has_bb = node_bbox_world(n, w, bb);

    int clipped_away = 0;
    Mask* own = node_clip_mask(n, w, c, clip, has_bb ? bb : NULL, aa,
                               &clipped_away);
    if (clipped_away) { mask_free(own); return; }
    Mask* eff = own ? own : clip;

    if (n->type == NODE_GROUP) {
        /* Collapse a chain of single-child, unclipped groups into one layer.
         * "A group of one thing at opacity a" equals "that thing at opacity
         * a*b", so N nested <g opacity=.9> cost one layer, not N.  Without
         * this a 300-deep chain allocates hundreds of full-canvas buffers. */
        if (op < 0.999f) {
            float c_op = op;
            Node* g = n;
            while (g->nkids == 1 && !g->clip && g->kids[0]->type == NODE_GROUP &&
                   !g->kids[0]->clip) {
                Node* child = g->kids[0];
                c_op *= child->opacity;
                g = child;
                if (c_op <= 0.0f) { mask_free(own); return; }
            }
            if (g != n) {
                /* Render the collapsed group's children directly into a
                 * layer using the composed opacity.  The chain's transforms
                 * are baked into `w` by walking them below. */
                Mat chain_w = w;
                for (Node* t = n->kids[0]; t != g; t = t->kids[0])
                    chain_w = mat_mul(chain_w, t->tm);
                render_group_layered(c, g, chain_w, c_op, eff, aa);
                mask_free(own);
                return;
            }
            render_group_layered(c, n, w, op, eff, aa);
        } else {
            for (int i = 0; i < n->nkids; i++)
                render_node(c, n->kids[i], w, op, eff, aa);
        }
    } else if (n->type == NODE_SHAPE) {
        render_shape(c, n, w, op, eff, aa);
    } else if (n->type == NODE_TEXT) {
        render_text(c, n, w, op, eff, aa);
    }
    mask_free(own);
}

void render_document(Canvas* c, Node* root, Mat view_tm,
                     const svg2img_options* opts) {
    int aa = (opts && opts->aa > 0) ? opts->aa : 4;
    if (aa < 1) aa = 1;
    if (aa > 8) aa = 8;
    render_node(c, root, view_tm, 1.0f, NULL, aa);
}

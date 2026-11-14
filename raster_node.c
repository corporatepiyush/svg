/* raster_node.c - Node bounds, opacity layers and tree traversal. */
#include "raster_internal.h"

/* Per-render budget state.  Passed down the tree instead of stored globally,
 * so a single process can render several documents concurrently. */
typedef struct {
    const svg2img_limits* lim;   /* NULL = defaults */
    int layers;                  /* opacity layers opened so far */
    int clips;                   /* nested clip masks opened so far */
    int masks;                   /* <mask> buffers opened so far */
} RenderState;

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
    if (!n || !n->visible || n->hidden_subtree) return 0;
    if (n->type == NODE_SHAPE && n->path && n->path->n > 0) {
        path_bbox_transformed(n->path, full_w, out);
        if (n->stroke.type != PAINT_NONE && n->sw > 0.0f) {
            /* The stroke width is authored in user units, so in device space
             * it must be scaled by the CTM before it is added to the bbox.
             * Without this the layer for an opacity group clips the stroke
             * away whenever the CTM scales up. */
            float half = n->sw * 0.5f * mat_scale_factor(full_w);
            out[0] -= half; out[1] -= half;
            out[2] += half; out[3] += half;
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
                        Mask* clip, int aa, RenderState* rs);

/* Render a group's children into a temporary layer and composite it with
 * the group's opacity.  `op` is the already-composed opacity (group
 * opacity * inherited opacity), and must be < 1 for a layer to be needed. */
static void render_group_layered(Canvas* c, Node* n, Mat w, float op,
                                 Mask* eff, int aa, RenderState* rs) {
    /* Layer budget: past the limit, draw straight into the parent instead of
     * allocating another full-canvas buffer.  The result is slightly wrong
     * for that one group (its opacity is applied per child rather than to the
     * flattened group), which is the documented degradation. */
    if (rs->lim && rs->layers >= rs->lim->max_layers) {
        for (int i = 0; i < n->nkids; i++)
            render_node(c, n->kids[i], w, op, eff, aa, rs);
        return;
    }
    rs->layers++;

    float bb[4];
    if (!node_bbox_world(n, w, bb)) { rs->layers--; return; }

    int x0 = svg_f2i(floorf(bb[0]), -1, c->w + 1, 0);
    int y0 = svg_f2i(floorf(bb[1]), -1, c->h + 1, 0);
    int x1 = svg_f2i(ceilf(bb[2]), -1, c->w + 1, 0);
    int y1 = svg_f2i(ceilf(bb[3]), -1, c->h + 1, 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) { rs->layers--; return; }

    Canvas* tmp = canvas_create(x1 - x0, y1 - y0);
    if (!tmp) { rs->layers--; return; }

    Mat tw = mat_mul(mat_translate(-(float)x0, -(float)y0), w);
    /* The layer renders in local coordinates (the child transform includes
     * the -x0,-y0 shift), so the inherited mask must be re-based to the
     * layer's own origin: a mask that keeps absolute device coordinates
     * would sample outside a small layer and clip everything away. */
    Mask* tclip = mask_translate(eff, x0, y0);
    for (int i = 0; i < n->nkids; i++)
        render_node(tmp, n->kids[i], tw, 1.0f, tclip, aa, rs);
    mask_free(tclip);

    composite_layer(c, x0, y0, tmp->px, tmp->w, tmp->h, op);
    canvas_free(tmp);
    rs->layers--;
}

static void render_node(Canvas* c, Node* n, Mat world, float opacity,
                        Mask* clip, int aa, RenderState* rs) {
    /* display:none stops the traversal; a NODE_SYMBOL is a definition that is
     * only ever rendered through a <use> clone (which is retagged NODE_GROUP),
     * so a symbol left in the tree paints nothing. */
    if (!n || n->hidden_subtree || n->type == NODE_SYMBOL ||
        n->type == NODE_IGNORE || n->type == NODE_PATTERNDEF ||
        n->type == NODE_GRADDEF || n->type == NODE_CLIPDEF ||
        n->type == NODE_MASKDEF) return;
    /* visibility:hidden suppresses this element's own painting but is
     * inherited per element, so a descendant that sets visibility:visible is
     * still drawn: traversal continues, only the painting is skipped.  The
     * subtree is skipped for a hidden shape/text too, which have no children;
     * for a hidden group we still descend. */
    if (!n->visible && (n->type == NODE_SHAPE || n->type == NODE_TEXT)) return;
    Mat w = mat_mul(world, n->tm);
    float op = opacity * n->opacity;
    if (op <= 0.0f) return;

    /* objectBoundingBox clip paths resolve against the element's own
     * bounding box, so compute it before building the mask. */
    float bb[4];
    int has_bb = node_bbox_world(n, w, bb);

    int clipped_away = 0;
    Mask* own = NULL;
    if (n->vp_clip) {
        /* Nested <svg>: clip to the viewport rectangle.  The rectangle is
         * transformed by the node's full transform, which maps its user space
         * (including the viewBox) to device space. */
        Mask* vm = make_rect_mask(n, w, c->w, c->h, aa);
        if (vm) {
            if (vm->empty) { mask_free(vm); return; }   /* empty viewport */
            if (!clip) {
                own = vm;
            } else {
                own = mask_intersect(clip, vm);
                mask_free(vm);
                if (!own) return;                       /* fully clipped away */
            }
        }
    }
    if (n->clip) {
        /* Clip-depth budget: past the limit the clip is ignored rather than
         * recursing without bound. */
        if (!rs->lim || rs->clips < rs->lim->max_clip_depth) {
            Mask* cm;
            rs->clips++;
            cm = node_clip_mask(n, w, c, own ? own : clip, has_bb ? bb : NULL,
                                aa, &clipped_away);
            rs->clips--;
            if (clipped_away) { mask_free(own); return; }
            mask_free(own);
            own = cm;
        }
    }
    if (n->mask && (!rs->lim || rs->masks < rs->lim->max_layers)) {
        rs->masks++;
        Mask* mm = make_mask_from_node(n->mask, w, has_bb ? bb : NULL,
                                       c->w, c->h, aa);
        rs->masks--;
        if (mm) {
            if (mm->empty) { mask_free(mm); mask_free(own); return; }
            if (!own) {
                own = mm;
            } else {
                Mask* both = mask_intersect(own, mm);
                mask_free(mm);
                mask_free(own);
                if (!both) return;
                own = both;
            }
        }
    }
    Mask* eff = own ? own : clip;

    if (n->type == NODE_GROUP || n->type == NODE_SYMBOL ||
        n->type == NODE_TRANSPARENT) {
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
                render_group_layered(c, g, chain_w, c_op, eff, aa, rs);
                mask_free(own);
                return;
            }
            render_group_layered(c, n, w, op, eff, aa, rs);
        } else {
            for (int i = 0; i < n->nkids; i++)
                render_node(c, n->kids[i], w, op, eff, aa, rs);
        }
    } else if (n->type == NODE_SHAPE) {
        render_shape(c, n, w, op, eff, aa);
    } else if (n->type == NODE_TEXT) {
        render_text(c, n, w, op, eff, aa);
    }
    mask_free(own);
}

/* Render a node's children into an existing canvas (used by <pattern> tiles).
 * The node itself is a definition, so only its children are drawn. */
void render_subtree(Canvas* c, const Node* n, Mat world, int aa) {
    if (!c || !n) return;
    RenderState st;
    st.lim = NULL;
    st.layers = 0;
    st.clips = 0;
    st.masks = 0;
    for (int i = 0; i < n->nkids; i++)
        render_node(c, n->kids[i], world, 1.0f, NULL, aa, &st);
}

void render_document(Canvas* c, Node* root, Mat view_tm,
                     const svg2img_options* opts, const svg2img_limits* lim) {
    int aa = (opts && opts->aa > 0) ? opts->aa : 4;
    if (aa < 1) aa = 1;
    if (aa > 8) aa = 8;
    RenderState st;
    st.lim = lim;
    st.layers = 0;
    st.clips = 0;
    st.masks = 0;
    render_node(c, root, view_tm, 1.0f, NULL, aa, &st);
}

/* raster_shape.c - Shape rendering (fill + stroke). */
#include "raster_internal.h"

/* ------------------------------------------------------------------ */
/* Shape / text rendering                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Markers (SVG 1.1 §11.6)                                             */
/* ------------------------------------------------------------------ */

static float edge_angle(Pt a, Pt b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    if (dx * dx + dy * dy < 1e-12f) return 0.0f;
    return atan2f(dy, dx) * 180.0f / SVG_PI;
}

static void draw_one_marker(Canvas* c, const Node* mk, Pt v, float angle,
                            Mat world, float sw, float opacity, Mask* clip,
                            int aa) {
    if (!mk || mk->nkids == 0) return;
    float s = (mk->mk_units == 0) ? sw : 1.0f;
    if (!(s > 0.0f)) s = 1.0f;
    Mat m = mat_translate(v.x, v.y);
    m = mat_mul(m, mat_rotate(angle));
    m = mat_mul(m, mat_scale(s, s));
    if (mk->mk_vb_valid) {
        float sx = mk->mk_w / mk->mk_vb[2];
        float sy = mk->mk_h / mk->mk_vb[3];
        float sc = mk->mk_par_slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        float ex = mk->mk_w - mk->mk_vb[2] * sc;
        float ey = mk->mk_h - mk->mk_vb[3] * sc;
        float ox = mk->mk_par_xa == 1 ? ex * 0.5f
                 : mk->mk_par_xa == 2 ? ex : 0.0f;
        float oy = mk->mk_par_ya == 1 ? ey * 0.5f
                 : mk->mk_par_ya == 2 ? ey : 0.0f;
        m = mat_mul(m, mat_translate(ox, oy));
        m = mat_mul(m, mat_scale(sc, sc));
        m = mat_mul(m, mat_translate(-mk->mk_vb[0], -mk->mk_vb[1]));
        /* refX/refY are in the marker's own (viewBox) coordinate system. */
        m = mat_mul(m, mat_translate(-mk->mk_refx, -mk->mk_refy));
    } else {
        m = mat_mul(m, mat_translate(-mk->mk_refx, -mk->mk_refy));
    }
    m = mat_mul(world, m);
    render_children(c, mk, m, opacity, clip, aa);
}

static void draw_markers(Canvas* c, Node* n, Mat world, float opacity,
                         Mask* clip, int aa) {
    if (!n->marker_ok || !n->path) return;
    if (!n->marker_start && !n->marker_mid && !n->marker_end) return;
    for (int si = 0; si < n->path->n; si++) {
        const SubPath* sp = &n->path->s[si];
        int cnt = sp->n;
        if (cnt < 2) continue;
        if (n->marker_start)
            draw_one_marker(c, n->marker_start, sp->p[0],
                            edge_angle(sp->p[0], sp->p[1]),
                            world, n->sw, opacity, clip, aa);
        for (int i = 1; i < cnt - 1; i++) {
            if (!n->marker_mid) break;
            draw_one_marker(c, n->marker_mid, sp->p[i],
                            edge_angle(sp->p[i - 1], sp->p[i + 1]),
                            world, n->sw, opacity, clip, aa);
        }
        if (sp->closed) {
            /* The closing vertex coincides with the first: the mid marker
             * sits on the last point, the end marker on the start point. */
            if (n->marker_mid)
                draw_one_marker(c, n->marker_mid, sp->p[cnt - 1],
                                edge_angle(sp->p[cnt - 2], sp->p[0]),
                                world, n->sw, opacity, clip, aa);
            if (n->marker_end)
                draw_one_marker(c, n->marker_end, sp->p[0],
                                edge_angle(sp->p[cnt - 1], sp->p[0]),
                                world, n->sw, opacity, clip, aa);
        } else if (n->marker_end) {
            draw_one_marker(c, n->marker_end, sp->p[cnt - 1],
                            edge_angle(sp->p[cnt - 2], sp->p[cnt - 1]),
                            world, n->sw, opacity, clip, aa);
        }
    }
}

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
    draw_markers(c, n, world, opacity, clip, aa);
}

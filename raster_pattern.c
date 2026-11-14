/* raster_pattern.c - <pattern> tile rasterisation and sampling.
 *
 * A pattern is rendered once into a tile the size of one period in device
 * pixels, then sampled per pixel by wrapping the referencing element's user
 * coordinates into the tile.  patternTransform is applied to the sampling
 * coordinate (the tile content is defined in pattern space); a rotated or
 * sheared patternTransform therefore rotates the tiling rather than the
 * artwork inside each tile. */
#include "raster_internal.h"

static float wrapf(float a, float b) {
    if (!(b > 0.0f)) return 0.0f;
    float r = fmodf(a, b);
    if (r < 0.0f) r += b;
    return r;
}

int pattern_build(Pattern* out, const void* patptr, const float bbox[4],
                  Mat world, int aa) {
    memset(out, 0, sizeof(*out));
    const Node* pn = (const Node*)patptr;
    if (!pn || pn->type != NODE_PATTERNDEF) return 0;
    if (!(pn->pat_w > 0.0f) || !(pn->pat_h > 0.0f)) return 0;

    /* objectBoundingBox patterns position the tile as a fraction of the
     * referencing element's bounding box, in the element's own user space.
     * `bbox` arrives in device space, so map it back first. */
    float ub[4] = {bbox[0], bbox[1], bbox[2], bbox[3]};
    out->has_inv = mat_invert_checked(world, &out->inv);
    if (pn->pat_units == UNITS_BBOX) {
        if (out->has_inv) {
            Pt c[4] = { {bbox[0], bbox[1]}, {bbox[2], bbox[1]},
                        {bbox[2], bbox[3]}, {bbox[0], bbox[3]} };
            ub[0] = ub[1] = 1e30f;
            ub[2] = ub[3] = -1e30f;
            for (int i = 0; i < 4; i++) {
                Pt q = mat_apply(out->inv, c[i]);
                if (q.x < ub[0]) ub[0] = q.x;
                if (q.y < ub[1]) ub[1] = q.y;
                if (q.x > ub[2]) ub[2] = q.x;
                if (q.y > ub[3]) ub[3] = q.y;
            }
            if (ub[0] > ub[2]) return 0;
        }
    }

    float tx, ty, tw, th;
    if (pn->pat_units == UNITS_BBOX) {
        float bw = ub[2] - ub[0], bh = ub[3] - ub[1];
        tx = ub[0] + pn->pat_x * bw;
        ty = ub[1] + pn->pat_y * bh;
        tw = pn->pat_w * bw;
        th = pn->pat_h * bh;
    } else {
        tx = pn->pat_x; ty = pn->pat_y;
        tw = pn->pat_w; th = pn->pat_h;
    }
    if (!(tw > 1e-6f) || !(th > 1e-6f)) return 0;

    float s = mat_scale_factor(world);
    if (!(s > 1e-6f) || !isfinite(s)) s = 1.0f;
    /* A patternTransform scales the tile in user space: a scale(2) doubles
     * the period, so the tile must be rendered at twice the resolution to
     * stay sharp, and the sampling coordinate is mapped back through the
     * inverse transform.  objectBoundingBox patterns ignore it (the tile is
     * defined in bounding-box space, where a transform has no meaning). */
    float eff = s;
    int use_pt = 0;
    if (pn->pat_units == UNITS_USER) {
        float ps = mat_scale_factor(pn->pat_tm);
        if (ps > 1e-6f && isfinite(ps)) { eff = s * ps; use_pt = 1; }
        out->has_inv = mat_invert_checked(pn->pat_tm, &out->inv_pt);
        use_pt = out->has_inv;
    }
    if (!(eff > 1e-6f) || !isfinite(eff)) eff = s;
    int w = (int)ceilf(tw * eff - 1e-6f);
    int h = (int)ceilf(th * eff - 1e-6f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if ((size_t)w > (size_t)-1 / (size_t)h / 4 / sizeof(float)) return 0;
    if ((size_t)w * (size_t)h > (size_t)16 * 1024 * 1024) return 0;

    Canvas* tile = canvas_create(w, h);
    if (!tile) return 0;

    /* Content transform: pattern space -> tile pixels.  For
     * patternContentUnits="objectBoundingBox" the content coordinates are
     * fractions of the element's bounding box, so map them into user space
     * first, then into the tile. */
    Mat content = mat_mul(mat_scale(eff, eff), mat_translate(-tx, -ty));
    if (pn->pat_content_units == UNITS_BBOX) {
        content = mat_mul(content,
                          mat_mul(mat_translate(ub[0], ub[1]),
                                  mat_scale(ub[2] - ub[0], ub[3] - ub[1])));
    }
    if (pn->pat_vb_valid) {
        Mat vbm = mat_mul(mat_scale(tw / pn->pat_vb[2], th / pn->pat_vb[3]),
                          mat_translate(-pn->pat_vb[0], -pn->pat_vb[1]));
        content = mat_mul(content, vbm);
    }
    render_subtree(tile, pn, content, aa);

    out->ok = 1;
    out->w = w;
    out->h = h;
    out->px = tile->px;
    out->ox = tx;
    out->oy = ty;
    out->tw = tw;
    out->th = th;
    out->scale = eff;
    out->use_pt = use_pt;
    tile->px = NULL;              /* ownership moved to the Pattern */
    canvas_free(tile);
    return 1;
}

Color pattern_eval(const Pattern* p, float x, float y) {
    float u = x, v = y;
    if (p->use_pt) {
        Pt q = mat_apply(p->inv_pt, (Pt){x, y});
        u = q.x; v = q.y;
    }
    float tu = wrapf(u - p->ox, p->tw) * p->scale;
    float tv = wrapf(v - p->oy, p->th) * p->scale;
    int px = (int)tu;
    int py = (int)tv;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= p->w) px = p->w - 1;
    if (py >= p->h) py = p->h - 1;
    const float* t = p->px + ((size_t)py * (size_t)p->w + (size_t)px) * 4;
    return (Color){t[0], t[1], t[2], t[3]};
}

void pattern_free(Pattern* p) {
    if (!p) return;
    free(p->px);
    p->px = NULL;
    p->ok = 0;
}
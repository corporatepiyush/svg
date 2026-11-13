/* raster_text.c - Bitmap-font text rendering. */
#include "raster_internal.h"

void render_text(Canvas* c, Node* n, Mat world, float opacity,
                        Mask* clip, int aa) {
    if (!n->text || !*n->text || n->fontsize <= 0.0f) return;
    float scale = n->fontsize / 7.0f;
    if (scale <= 0.0f) return;

    /* Advance per glyph: 6 units in a 7-unit em, plus letter-spacing. */
    float adv = 6.0f * scale + n->letterspacing;
    float total = 0.0f;
    for (const char* q = n->text; *q; q++) total += adv;
    float x = n->x;
    if (n->anchor == 1) x -= total * 0.5f;
    else if (n->anchor == 2) x -= total;
    float y = n->y;

    Mat tw = world;
    if (n->italic) {
        Mat sk = mat_mul(mat_translate(n->x, n->y),
                         mat_mul(mat_skew_x(-12.0f), mat_translate(-n->x, -n->y)));
        tw = mat_mul(tw, sk);
    }

    Path* p = (Path*)svg_malloc(sizeof(Path));
    if (!p) return;
    memset(p, 0, sizeof(*p));
    float be = n->bold ? 0.35f * scale : 0.0f;
    const float gs = scale + 2.0f * be;

    /* One closed subpath per lit pixel.  Each glyph is at most 35 lit
     * pixels, so this is a bounded amount of work and keeps the path
     * well-formed for both fill and stroke. */
    int glyph_pixels = 0;
    for (const char* q = n->text; *q; q++) {
        unsigned char rows[7];
        if (font_glyph((unsigned char)*q, rows)) {
            for (int r = 0; r < 7; r++)
                for (int col = 0; col < 5; col++)
                    if (rows[r] & (1 << col)) glyph_pixels++;
        }
    }

    if (glyph_pixels > 0) {
        p->n = glyph_pixels;
        p->s = (SubPath*)svg_malloc((size_t)glyph_pixels * sizeof(SubPath));
        if (!p->s) { free(p); return; }
        memset(p->s, 0, (size_t)glyph_pixels * sizeof(SubPath));

        /* One shared point buffer for every glyph quad.  Each quad gets its
         * own SubPath descriptor (needed for fill/stroke correctness) but
         * they all point into this single allocation, so the whole string
         * costs one malloc instead of one per lit pixel. */
        Pt* pts = (Pt*)svg_malloc((size_t)glyph_pixels * 4 * sizeof(Pt));
        if (!pts) { free(p->s); p->s = NULL; p->n = 0; free(p); return; }
        p->pts_arena = pts;
        p->shared_points = 1;

        int gi = 0;
        float cx = x;
        for (const char* q = n->text; *q; q++) {
            unsigned char rows[7];
            if (font_glyph((unsigned char)*q, rows)) {
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 5; col++) {
                        if (!(rows[row] & (1 << col))) continue;
                        float gx = cx + (float)col * scale - be;
                        float gy = y - 7.0f * scale + (float)row * scale - be;
                        Pt* sq = pts + (size_t)gi * 4;
                        sq[0].x = gx;       sq[0].y = gy;
                        sq[1].x = gx + gs;  sq[1].y = gy;
                        sq[2].x = gx + gs;  sq[2].y = gy + gs;
                        sq[3].x = gx;       sq[3].y = gy + gs;
                        p->s[gi].p = sq;
                        p->s[gi].n = 4;
                        p->s[gi].closed = 1;
                        gi++;
                    }
                }
            }
            cx += adv;
        }
    }

    if (p->n > 0) {
        path_transform(p, tw);
        float bb[4];
        path_bbox(p, bb);
        float ds = mat_scale_factor(tw);
        if (n->fill.type != PAINT_NONE && n->fillop > 0.0f) {
            fill_path(c, p, 0, &n->fill, opacity * n->fillop, clip, bb, bb, aa, tw);
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
                fill_path(c, sp, 0, &n->stroke, opacity * n->strokeop, clip,
                          sbb, bb, aa, tw);
                path_free(sp);
            }
        }
    }
    path_free(p);   /* releases the shared point arena */
}

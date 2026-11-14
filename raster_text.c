/* raster_text.c - Bitmap-font text rendering.
 *
 * Text is a list of positioned runs (a <text> or <tspan> fragment).  Each run
 * is converted to one quad per lit pixel of the 5x7 font, then filled and
 * stroked through the normal path pipeline, so gradients, clips, opacity and
 * markers all work on text. */
#include "raster_internal.h"

static float adv_of(float fontsize, float letterspacing) {
    return 6.0f * (fontsize / 7.0f) + letterspacing;
}

/* Build the glyph quads for one run.  Returns a path with a shared point
 * arena, or NULL when the run has no lit pixels. */
static Path* build_glyph_path(const char* text, float x, float y, float scale,
                              int bold, float letterspacing) {
    int glyph_pixels = 0;
    for (const char* q = text; *q; q++) {
        unsigned char rows[7];
        if (font_glyph((unsigned char)*q, rows))
            for (int r = 0; r < 7; r++)
                for (int col = 0; col < 5; col++)
                    if (rows[r] & (1 << col)) glyph_pixels++;
    }
    if (glyph_pixels <= 0) return NULL;

    Path* p = (Path*)svg_malloc(sizeof(Path));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->n = glyph_pixels;
    p->s = (SubPath*)svg_malloc((size_t)glyph_pixels * sizeof(SubPath));
    Pt* pts = (Pt*)svg_malloc((size_t)glyph_pixels * 4 * sizeof(Pt));
    if (!p->s || !pts) { free(p->s); free(pts); free(p); return NULL; }
    memset(p->s, 0, (size_t)glyph_pixels * sizeof(SubPath));
    p->pts_arena = pts;
    p->shared_points = 1;

    float be = bold ? 0.35f * scale : 0.0f;
    float gs = scale + 2.0f * be;
    float adv = adv_of(scale * 7.0f, letterspacing);
    int gi = 0;
    float cx = x;
    for (const char* q = text; *q; q++) {
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
    return p;
}

static void draw_chunk(Canvas* c, const TextChunk* ch, Mat world,
                       float opacity, Mask* clip, int aa) {
    if (!ch || !ch->text || !*ch->text || !(ch->fontsize > 0.0f)) return;
    float scale = ch->fontsize / 7.0f;
    if (!(scale > 0.0f)) return;

    float total = (float)strlen(ch->text) * adv_of(ch->fontsize, ch->letterspacing);
    float x = ch->x;
    if (ch->anchor == 1) x -= total * 0.5f;
    else if (ch->anchor == 2) x -= total;
    float y = ch->y;

    Mat tw = world;
    if (ch->italic) {
        Mat sk = mat_mul(mat_translate(ch->x, ch->y),
                         mat_mul(mat_skew_x(-12.0f),
                                 mat_translate(-ch->x, -ch->y)));
        tw = mat_mul(tw, sk);
    }

    Path* p = build_glyph_path(ch->text, x, y, scale, ch->bold, ch->letterspacing);
    if (!p) return;
    path_transform(p, tw);
    float bb[4];
    path_bbox(p, bb);
    if (ch->fill.type != PAINT_NONE) {
        fill_path(c, p, 0, &ch->fill, opacity, clip, bb, bb, aa, tw);
    }
    path_free(p);
}

void render_text(Canvas* c, Node* n, Mat world, float opacity,
                        Mask* clip, int aa) {
    if (!n) return;
    if (n->nchunks > 0 && n->chunks) {
        for (int i = 0; i < n->nchunks; i++)
            draw_chunk(c, &n->chunks[i], world, opacity, clip, aa);
        return;
    }
    if (!n->text || !*n->text) return;
    TextChunk ch;
    memset(&ch, 0, sizeof(ch));
    ch.text = n->text;
    ch.x = n->x;
    ch.y = n->y;
    ch.fontsize = n->fontsize;
    ch.letterspacing = n->letterspacing;
    ch.bold = n->bold;
    ch.italic = n->italic;
    ch.anchor = n->anchor;
    ch.fill = n->fill;
    draw_chunk(c, &ch, world, opacity, clip, aa);
}
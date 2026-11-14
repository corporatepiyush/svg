/* raster_text.c - Bitmap-font text rendering.
 *
 * Text is a list of positioned runs (a <text> or <tspan> fragment).  Each run
 * is laid out glyph by glyph, honouring letter-spacing, textLength, per-glyph
 * rotation, writing-mode, dominant-baseline and textPath, then converted to
 * one quad per lit pixel of the 5x7 font.  The quads go through the normal
 * path pipeline, so gradients, clips, opacity and filters all work on text.
 */
#include "raster_internal.h"

static float adv_of(float fontsize, float letterspacing) {
    return 6.0f * (fontsize / 7.0f) + letterspacing;
}

/* Point at `dist` along a flattened path, plus the tangent angle.  Returns 0
 * when the distance is past the end. */
static int path_sample(const Path* p, float dist, float* ox, float* oy,
                       float* ang) {
    if (!p || dist < 0.0f) return 0;
    float acc = 0.0f;
    for (int i = 0; i < p->n; i++) {
        const SubPath* s = &p->s[i];
        int n = s->closed ? s->n + 1 : s->n;
        for (int k = 1; k < n; k++) {
            const Pt* a = &s->p[k - 1];
            const Pt* b = &s->p[(k < s->n) ? k : 0];
            float dx = b->x - a->x, dy = b->y - a->y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1e-9f) continue;
            if (acc + len >= dist) {
                float t = (dist - acc) / len;
                *ox = a->x + dx * t;
                *oy = a->y + dy * t;
                *ang = atan2f(dy, dx) * 180.0f / (float)SVG_PI;
                return 1;
            }
            acc += len;
        }
    }
    return 0;
}

/* Emit one glyph's lit pixels as quads.  `ox`,`oy` is the glyph origin on the
 * baseline; `angle` rotates the glyph clockwise (degrees).  Returns the number
 * of quads added. */
static int emit_glyph(Path* p, int gi, Pt* pts, unsigned char rows[7],
                      float ox, float oy, float angle, float scale, float be,
                      float gscale, int italic) {
    float a = angle * (float)SVG_PI / 180.0f;
    float ca = cosf(a), sa = sinf(a);
    float gs = (scale + 2.0f * be) * gscale;
    int added = 0;
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (!(rows[row] & (1 << col))) continue;
            float lx = ((float)col * scale - be) * gscale;
            float ly = (-7.0f * scale + (float)row * scale - be) * gscale;
            if (italic) lx += -0.2126f * ly;      /* 12 degree skew */
            float rx = lx * ca - ly * sa + ox;
            float ry = lx * sa + ly * ca + oy;
            float ux = gs * ca, uy = gs * sa;     /* local +x in device */
            float vx = -gs * sa, vy = gs * ca;    /* local +y in device */
            Pt* q = pts + (size_t)(gi + added) * 4;
            q[0].x = rx;             q[0].y = ry;
            q[1].x = rx + ux;        q[1].y = ry + uy;
            q[2].x = rx + ux + vx;   q[2].y = ry + uy + vy;
            q[3].x = rx + vx;        q[3].y = ry + vy;
            p->s[gi + added].p = q;
            p->s[gi + added].n = 4;
            p->s[gi + added].closed = 1;
            added++;
        }
    }
    return added;
}

static Path* build_text_path(const TextChunk* ch, float* out_total) {
    int n = (int)strlen(ch->text);
    float scale = ch->fontsize / 7.0f;
    if (n <= 0 || !(scale > 0.0f)) return NULL;

    float adv = adv_of(ch->fontsize, ch->letterspacing);
    float gscale = 1.0f;
    float total = (float)n * adv;
    if (ch->textlen > 0.0f && total > 0.0f) {
        if (ch->length_kind == 1) {
            gscale = ch->textlen / total;
            adv *= gscale;
            total = ch->textlen;
        } else {
            adv = ch->textlen / (float)n;
            total = ch->textlen;
        }
    }

    int glyph_pixels = 0;
    for (const char* q = ch->text; *q; q++) {
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

    float be = ch->bold ? 0.35f * scale : 0.0f;
    float ox = ch->x;
    float oy = ch->y + ch->baseline * ch->fontsize;
    if (!ch->writing) {
        if (ch->anchor == 1) ox -= total * 0.5f;
        else if (ch->anchor == 2) ox -= total;
    } else {
        if (ch->anchor == 1) oy -= total * 0.5f;
        else if (ch->anchor == 2) oy -= total;
    }

    Path* ref = NULL;
    int ref_owned = 0;
    if (ch->refpath) {
        ref = ch->refpath;
    } else if (ch->path && *ch->path) {
        ref = path_parse(ch->path);
        ref_owned = 1;
    }

    int gi = 0;
    int i = 0;
    for (const char* q = ch->text; *q; q++, i++) {
        unsigned char rows[7];
        int known = font_glyph((unsigned char)*q, rows);
        if (known && gi < glyph_pixels) {
            float px = ox, py = oy;
            float angle = (ch->writing ? 90.0f : 0.0f);
            if (ch->nrot > 0) angle += ch->rot[i % ch->nrot];
            if (ref) {
                float d = ch->start_offset + (float)i * adv;
                float sx, sy, sang;
                if (!path_sample(ref, d, &sx, &sy, &sang)) break;
                px = sx;
                py = sy;
                angle += sang;
            } else if (ch->writing) {
                py = oy + (float)i * adv;
            } else {
                px = ox + (float)i * adv;
            }
            gi += emit_glyph(p, gi, pts, rows, px, py, angle, scale, be,
                             gscale, ch->italic);
        }
    }
    if (ref_owned) path_free(ref);
    *out_total = total;
    return p;
}

static void draw_chunk(Canvas* c, const TextChunk* ch, Mat world,
                       float opacity, Mask* clip, int aa) {
    if (!ch || !ch->text || !*ch->text || !(ch->fontsize > 0.0f)) return;
    Path* p = build_text_path(ch, &(float){0.0f});
    if (!p) return;
    path_transform(p, world);
    float bb[4];
    path_bbox(p, bb);
    if (ch->fill.type != PAINT_NONE) {
        fill_path(c, p, 0, &ch->fill, opacity, clip, bb, bb, aa, world);
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
/* raster_text.c - Text rendering.
 *
 * Text is a list of positioned runs (a <text> or <tspan> fragment).  Each run
 * is laid out glyph by glyph, honouring letter-spacing, textLength, per-glyph
 * rotation, writing-mode, dominant-baseline and textPath, then converted to
 * geometry: either the built-in 5x7 bitmap (one quad per lit pixel) or, when
 * the caller supplied a font file, the glyph's TrueType outline flattened to
 * polylines.  The result goes through the normal path pipeline, so gradients,
 * clips, opacity and filters all work on text.
 */
#include "raster_internal.h"

static float adv_of(float fontsize, float letterspacing) {
    return 6.0f * (fontsize / 7.0f) + letterspacing;
}

/* ------------------------------------------------------------------ */
/* Geometry accumulator                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    SubPath* s;
    int n, cap;
} PB;

static int pb_reserve(PB* pb) {
    if (pb->n == pb->cap) {
        int cap = pb->cap ? pb->cap * 2 : 16;
        SubPath* ns = (SubPath*)svg_realloc(pb->s, (size_t)cap * sizeof(SubPath));
        if (!ns) return 0;
        pb->s = ns;
        pb->cap = cap;
    }
    return 1;
}

/* Append a contour, transforming it into device space. */
static int pb_contour(PB* pb, const float* xy, int npts, float ox, float oy,
                      float ca, float sa, float gscale, float italic,
                      int flip_y) {
    if (npts < 3) return 1;
    if (!pb_reserve(pb)) return 0;
    Pt* p = (Pt*)svg_malloc((size_t)npts * sizeof(Pt));
    if (!p) return 0;
    for (int i = 0; i < npts; i++) {
        float x = xy[i * 2] * gscale;
        /* TrueType outlines are y-up; SVG is y-down. */
        float y = (flip_y ? -xy[i * 2 + 1] : xy[i * 2 + 1]) * gscale;
        if (italic != 0) x += -0.2126f * y;
        p[i].x = x * ca - y * sa + ox;
        p[i].y = x * sa + y * ca + oy;
    }
    pb->s[pb->n].p = p;
    pb->s[pb->n].n = npts;
    pb->s[pb->n].closed = 1;
    pb->n++;
    return 1;
}

static Path* pb_finish(PB* pb) {
    if (pb->n == 0) { free(pb->s); return NULL; }
    Path* p = (Path*)svg_malloc(sizeof(Path));
    if (!p) { free(pb->s); return NULL; }
    memset(p, 0, sizeof(*p));
    p->s = pb->s;
    p->n = pb->n;
    p->cap = pb->cap;
    p->shared_points = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* Glyph emission                                                      */
/* ------------------------------------------------------------------ */

static int emit_bitmap_glyph(PB* pb, const char* q, float ox, float oy,
                             float ca, float sa, float scale, float be,
                             float gscale) {
    unsigned char rows[7];
    if (!font_glyph((unsigned char)*q, rows)) return 1;
    float gs = (scale + 2.0f * be) * gscale;
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (!(rows[row] & (1 << col))) continue;
            float lx = ((float)col * scale - be) * gscale;
            float ly = (-7.0f * scale + (float)row * scale - be) * gscale;
            float xy[8] = { lx, ly,
                            lx + gs, ly,
                            lx + gs, ly + gs,
                            lx, ly + gs };
            if (!pb_contour(pb, xy, 4, ox, oy, ca, sa, 1.0f, 0.0f, 0)) return 0;
        }
    }
    return 1;
}

static int emit_font_glyph(PB* pb, const FontFace* f, int gid, float size,
                           float ox, float oy, float ca, float sa, float gscale,
                           int italic) {
    PtList pl;
    memset(&pl, 0, sizeof(pl));
    int nc = 0;
    if (font_face_contours(f, gid, size, &pl, &nc)) {
        free(pl.xy);
        free(pl.cont);
        return 1;                       /* empty glyph: nothing to draw */
    }
    int ok = 1;
    size_t base = 0;
    for (int c = 0; c < nc && c < pl.nc; c++) {
        if (!pb_contour(pb, pl.xy + base, (int)pl.cont[c], ox, oy, ca, sa,
                        gscale, italic != 0, 1)) {
            ok = 0;
            break;
        }
        base += (size_t)pl.cont[c] * 2;
    }
    free(pl.xy);
    free(pl.cont);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Path sampling (textPath)                                            */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void draw_chunk(Canvas* c, const TextChunk* ch, Mat world,
                       float opacity, Mask* clip, int aa) {
    if (!ch || !ch->text || !*ch->text || !(ch->fontsize > 0.0f)) return;
    int n = (int)strlen(ch->text);
    const FontFace* f = ch->font;
    float scale = ch->fontsize / 7.0f;
    float adv = f ? 0.0f : adv_of(ch->fontsize, ch->letterspacing);
    if (f) {
        /* The run's natural advance: the sum of the glyph advances plus
         * letter-spacing after each glyph. */
        for (const char* q = ch->text; *q; q++) {
            int gid = font_face_glyph_index(f, (unsigned char)*q);
            adv += font_face_advance(f, gid) * ch->fontsize + ch->letterspacing;
        }
    }

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

    PB pb;
    memset(&pb, 0, sizeof(pb));
    float be = ch->bold ? 0.35f * scale : 0.0f;
    float pen = 0.0f;
    for (int i = 0; i < n; i++) {
        float px = ox, py = oy;
        float angle = (ch->writing ? 90.0f : 0.0f);
        if (ch->nrot > 0) angle += ch->rot[i % ch->nrot];
        if (ref) {
            float sx, sy, sang;
            if (!path_sample(ref, ch->start_offset + pen, &sx, &sy, &sang)) break;
            px = sx;
            py = sy;
            angle += sang;
        } else if (ch->writing) {
            py = oy + pen;
        } else {
            px = ox + pen;
        }
        float ca = cosf(angle * (float)SVG_PI / 180.0f);
        float sa = sinf(angle * (float)SVG_PI / 180.0f);
        if (f) {
            int gid = font_face_glyph_index(f, (unsigned char)ch->text[i]);
            emit_font_glyph(&pb, f, gid, ch->fontsize, px, py, ca, sa, gscale,
                            ch->italic);
            pen += font_face_advance(f, gid) * ch->fontsize * gscale +
                   ch->letterspacing * gscale;
        } else {
            emit_bitmap_glyph(&pb, ch->text + i, px, py, ca, sa, scale, be,
                              gscale);
            pen += adv;
        }
    }
    if (ref_owned) path_free(ref);

    Path* p = pb_finish(&pb);
    if (!p) return;
    path_transform(p, world);
    float bb[4];
    path_bbox(p, bb);
    if (ch->fill.type != PAINT_NONE)
        fill_path(c, p, 0, &ch->fill, opacity, clip, bb, bb, aa, world);
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
    ch.font = n->font;
    draw_chunk(c, &ch, world, opacity, clip, aa);
}
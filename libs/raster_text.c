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

/* Total length of a path (device or user space). */
static float path_length_of(const Path* p) {
    if (!p) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < p->n; i++) {
        const SubPath* s = &p->s[i];
        int n = s->closed ? s->n + 1 : s->n;
        for (int k = 1; k < n; k++) {
            const Pt* a = &s->p[k - 1];
            const Pt* b = &s->p[(k < s->n) ? k : 0];
            float dx = b->x - a->x, dy = b->y - a->y;
            total += sqrtf(dx * dx + dy * dy);
        }
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Geometry accumulator                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t off;              /* start of this contour in the point arena */
    int n, closed;
} PBRec;

typedef struct {
    PBRec* rec;
    Pt* pts;
    int n, cap;              /* contours */
    size_t npts, cap_pts;    /* points */
    Mat world;               /* applied while points are emitted */
    float bb[4];             /* device-space bounds, accumulated in the same pass */
} PB;

static int pb_reserve(PB* pb) {
    if (pb->n == pb->cap) {
        /* Text runs commonly produce 20-60 contours (a five-letter run in
         * the built-in 5x7 font lands there), so starting at 16 only caused
         * two growth copies per run. */
        int cap = pb->cap ? pb->cap * 2 : 64;
        PBRec* nr = (PBRec*)svg_realloc(pb->rec, (size_t)cap * sizeof(PBRec));
        if (!nr) return 0;
        pb->rec = nr;
        pb->cap = cap;
    }
    return 1;
}

/* Reserve room for `add` more points in the shared arena. */
static int pb_reserve_pts(PB* pb, size_t add) {
    if (add > (size_t)-1 - pb->npts) return 0;
    size_t need = pb->npts + add;
    if (need <= pb->cap_pts) return 1;
    size_t cap = pb->cap_pts ? pb->cap_pts : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) { cap = need; break; }
        cap *= 2;
    }
    if (cap > (size_t)-1 / sizeof(Pt)) return 0;
    Pt* np = (Pt*)svg_realloc(pb->pts, cap * sizeof(Pt));
    if (!np) return 0;
    pb->pts = np;
    pb->cap_pts = cap;
    return 1;
}

/* Append a contour, transforming it into device space. */
static int pb_contour(PB* pb, const float* xy, int npts, float ox, float oy,
                      float ca, float sa, float gscale, float italic,
                      int flip_y) {
    if (npts < 3) return 1;
    if (!pb_reserve(pb) || !pb_reserve_pts(pb, (size_t)npts)) return 0;
    Pt* dst = pb->pts + pb->npts;
    for (int i = 0; i < npts; i++) {
        float x = xy[i * 2] * gscale;
        /* TrueType outlines are y-up; SVG is y-down. */
        float y = (flip_y ? -xy[i * 2 + 1] : xy[i * 2 + 1]) * gscale;
        if (italic != 0) x += -0.2126f * y;
        float lx = x * ca - y * sa + ox;
        float ly = x * sa + y * ca + oy;
        /* Bake the node transform here instead of walking the finished path
         * a second time with path_transform(), and accumulate the device
         * bounding box in the same pass (a third walk). */
        float dx = pb->world.m[0] * lx + pb->world.m[2] * ly + pb->world.m[4];
        float dy = pb->world.m[1] * lx + pb->world.m[3] * ly + pb->world.m[5];
        dst[i].x = dx;
        dst[i].y = dy;
        if (dx < pb->bb[0]) pb->bb[0] = dx;
        if (dy < pb->bb[1]) pb->bb[1] = dy;
        if (dx > pb->bb[2]) pb->bb[2] = dx;
        if (dy > pb->bb[3]) pb->bb[3] = dy;
    }
    pb->rec[pb->n].off = pb->npts;
    pb->rec[pb->n].n = npts;
    pb->rec[pb->n].closed = 1;
    pb->npts += (size_t)npts;
    pb->n++;
    return 1;
}

static Path* pb_finish(PB* pb) {
    if (pb->n == 0) { svg_mfree(pb->rec); svg_mfree(pb->pts); return NULL; }
    /* One block: [Path][SubPath x n][Pt x npts]. */
    Path* p = (Path*)svg_malloc(sizeof(Path) + (size_t)pb->n * sizeof(SubPath) +
                                pb->npts * sizeof(Pt));
    if (!p) {
        svg_mfree(pb->rec);
        svg_mfree(pb->pts);
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    SubPath* s = (SubPath*)(void*)(p + 1);
    Pt* pts = (Pt*)(void*)(s + pb->n);
    if (pb->npts) memcpy(pts, pb->pts, pb->npts * sizeof(Pt));
    for (int i = 0; i < pb->n; i++) {
        s[i].p = pts + pb->rec[i].off;
        s[i].n = pb->rec[i].n;
        s[i].closed = pb->rec[i].closed;
    }
    p->s = p->s_inline = s;
    p->n = pb->n;
    p->cap = pb->n;
    p->shared_points = PATH_ONE_BLOCK;
    svg_mfree(pb->pts);
    svg_mfree(pb->rec);
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
            if (!(rows[row] & (0x10 >> col))) continue;   /* bit 4 = left */
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
        svg_mfree(pl.xy);
        svg_mfree(pl.cont);
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
    svg_mfree(pl.xy);
    svg_mfree(pl.cont);
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
                       float opacity, const Mask* clip, int aa) {
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
    /* startOffset as a percentage of the whole path (SVG 1.1 §10.13). */
    float start = ch->start_offset;
    if (ch->start_is_pct && ref) start = path_length_of(ref) * start;

    PB pb;
    memset(&pb, 0, sizeof(pb));
    pb.world = world;
    pb.bb[0] = pb.bb[1] = 1e30f;
    pb.bb[2] = pb.bb[3] = -1e30f;
    float be = ch->bold ? 0.35f * scale : 0.0f;
    float pen = 0.0f;
    for (int i = 0; i < n; i++) {
        float px = ox, py = oy;
        float angle = (ch->writing ? 90.0f : 0.0f);
        if (ch->nrot > 0) angle += ch->rot[i % ch->nrot];
        if (ref) {
            float sx, sy, sang;
            if (!path_sample(ref, start + pen, &sx, &sy, &sang)) break;
            px = sx;
            py = sy;
            angle += sang;
        } else if (ch->writing) {
            py = oy + pen;
        } else {
            px = ox + pen;
        }
        /* A coordinate list overrides the accumulated pen position for this
         * glyph; a relative list shifts it (SVG 1.1 §10.4/§10.5).  An entry
         * of 0 is still an explicit coordinate (x="0" is meaningful), so the
         * presence of a list is tracked by ngpos, not by the value. */
        if (i > 0 && i - 1 < ch->ngpos) {
            float qx = ch->gx ? ch->gx[i - 1] : 0.0f;
            float qy = ch->gy ? ch->gy[i - 1] : 0.0f;
            px = ch->x + qx;
            py = ch->y + qy;
            if (ch->gdx) px += ch->gdx[i - 1];
            if (ch->gdy) py += ch->gdy[i - 1];
            /* Continue from this glyph's real position, not the old pen. */
            pen = px - ox;
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
    float bb[4] = {pb.bb[0], pb.bb[1], pb.bb[2], pb.bb[3]};
    if (ch->fill.type != PAINT_NONE)
        fill_path(c, p, 0, &ch->fill, opacity, clip, bb, bb, aa, world);
    /* Text strokes use the same pipeline as shape strokes (SVG 1.1 §11.4);
     * dash lengths and stroke-width are authored in user units. */
    if (ch->stroke.type != PAINT_NONE && ch->stroke_width > 0.0f) {
        float ds = mat_scale_factor(world);
        float* dash = NULL;
        int ndash = 0;
        if (ch->dash && ch->ndash > 0) {
            dash = (float*)svg_malloc((size_t)ch->ndash * sizeof(float));
            if (dash) {
                for (int i = 0; i < ch->ndash; i++)
                    dash[i] = ch->dash[i] * ds;
                ndash = ch->ndash;
            }
        }
        Path* sp = build_stroke(p, ch->stroke_width * ds, ch->linecap,
                                ch->linejoin, ch->miterlimit, dash, ndash,
                                ch->dashoff * ds);
        svg_mfree(dash);
        if (sp) {
            float sbb[4];
            path_bbox(sp, sbb);
            fill_path(c, sp, 0, &ch->stroke, opacity, clip, sbb, bb, aa, world);
            path_free(sp);
        }
    }
    path_free(p);
}

void render_text(Canvas* c, const Node* n, Mat world, float opacity,
                        const Mask* clip, int aa) {
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
    ch.stroke = n->stroke;
    ch.stroke_width = n->sw;
    ch.font = n->font;
    draw_chunk(c, &ch, world, opacity, clip, aa);
}
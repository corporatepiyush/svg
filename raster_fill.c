/* raster_fill.c - Scanline polygon fill. */
#include "raster_internal.h"

/* ------------------------------------------------------------------ */
/* Scanline fill                                                       */
/* ------------------------------------------------------------------ */

typedef struct { float x_ymin, ymin, ymax, invslope; int dir; } Edge;
typedef struct { float x; int dir; } Cross;

/* Sort crossings by x.  Insertion sort below 32 elements; an in-place
 * quicksort (median-of-three, Hoare partition) above that, which avoids
 * qsort's per-comparison function call. */
static void sort_cross(Cross* a, int n) {
    if (n < 2) return;
    if (n < 32) {
        for (int i = 1; i < n; i++) {
            Cross key = a[i];
            int j = i - 1;
            while (j >= 0 && a[j].x > key.x) { a[j + 1] = a[j]; j--; }
            a[j + 1] = key;
        }
        return;
    }
    int lo[64], hi[64], sp = 0;
    lo[0] = 0; hi[0] = n - 1; sp = 1;
    while (sp > 0) {
        int l = lo[--sp];
        int h = hi[sp];
        while (l < h) {
            int m = l + (h - l) / 2;
            /* median-of-three into a[m] */
            if (a[m].x < a[l].x) { Cross t = a[m]; a[m] = a[l]; a[l] = t; }
            if (a[h].x < a[l].x) { Cross t = a[h]; a[h] = a[l]; a[l] = t; }
            if (a[h].x < a[m].x) { Cross t = a[h]; a[h] = a[m]; a[m] = t; }
            float pivot = a[m].x;
            int i = l, j = h;
            for (;;) {
                while (a[i].x < pivot) i++;
                while (a[j].x > pivot) j--;
                if (i >= j) break;
                Cross t = a[i]; a[i] = a[j]; a[j] = t;
                i++; j--;
            }
            /* Recurse on the smaller side, loop on the larger.  Guard
             * against a non-advancing partition (all keys equal). */
            int nl = i, nh = j;
            if (nl <= l) nl = l + 1;
            if (nh >= h) nh = h - 1;
            if (nl > nh) break;
            if (nh - l < h - nl) {
                if (l < nh) { lo[sp] = l; hi[sp] = nh; sp++; }
                l = nl;
            } else {
                if (nl < h) { lo[sp] = nl; hi[sp] = h; sp++; }
                h = nh;
            }
        }
    }
}

static int build_edges(const Path* path, Edge** out) {
    /* Count first so the edge array is allocated exactly once.  The count
     * and the fill loop below must agree on which subpaths contribute, so
     * both skip subpaths with fewer than two points.
     *
     * Every subpath is filled as if it were closed (SVG 1.1 §11.4): an open
     * subpath is implicitly closed with a straight segment before filling,
     * so it contributes exactly sp->n edges.  Stroke geometry is built
     * separately by build_stroke() and never comes through here, so this
     * does not affect open strokes. */
    int n = 0;
    for (int s = 0; s < path->n; s++) {
        const SubPath* sp = &path->s[s];
        if (sp->n < 2 || !sp->p) continue;
        n += sp->n;
    }
    if (n <= 0) { *out = NULL; return 0; }

    Edge* e = (Edge*)svg_malloc((size_t)n * sizeof(Edge));
    if (!e) { *out = NULL; return 0; }

    int k = 0;
    for (int s = 0; s < path->n; s++) {
        const SubPath* sp = &path->s[s];
        /* A subpath with fewer than two points has no segments; guard the
         * modulo too, since (i+1) % 0 is undefined. */
        if (sp->n < 2 || !sp->p) continue;
        const int nseg = sp->n;            /* fill closes open subpaths */
        const Pt* pts = sp->p;
        for (int i = 0; i < nseg; i++) {
            Pt a = pts[i];
            Pt b = pts[(i + 1) % sp->n];
            float dy = b.y - a.y;
            if (fabsf(dy) < 1e-12f) continue;
            if (!isfinite(a.x) || !isfinite(a.y) ||
                !isfinite(b.x) || !isfinite(b.y)) continue;
            if (k >= n) break;              /* defensive: never overrun */
            Edge* ed = &e[k++];
            ed->ymin = a.y < b.y ? a.y : b.y;
            ed->ymax = a.y < b.y ? b.y : a.y;
            ed->x_ymin = a.y < b.y ? a.x : b.x;
            ed->invslope = (b.x - a.x) / dy;
            ed->dir = (dy > 0.0f) ? 1 : -1;        }
    }
    *out = e;
    return k;
}

/* Add the coverage of one horizontal run of sub-pixel samples [s0, s1) to
 * the coverage row.  A pixel is fully covered only when the run covers all
 * `aa` of its sample columns.  Spreading a run over the pixels it merely
 * touches (the previous behaviour) made a run that ended mid-pixel cover
 * that pixel completely, which dilated every antialiased edge by up to one
 * pixel: a 1-unit vertical stroke at an integer coordinate rendered two
 * pixels wide instead of one. */
static void add_run(float* cov, int x0, int s0, int s1, int aa) {
    if (s1 <= s0) return;
    float* row = cov - x0;                 /* index by absolute pixel */
    const float k = 1.0f / (float)(aa * aa);
    const float full = (float)aa * k;      /* a fully covered pixel */
    int first = s0 / aa;
    int last = (s1 - 1) / aa;
    if (first == last) {
        row[first] += (float)(s1 - s0) * k;
        return;
    }
    row[first] += (float)((first + 1) * aa - s0) * k;
    for (int p = first + 1; p < last; p++) row[p] += full;
    row[last] += (float)(s1 - last * aa) * k;
}

void fill_path(Canvas* c, const Path* path, int rule, const Paint* paint,
                      float opacity, const Mask* clip, const float bounds[4],
                      const float grad_bbox[4], int aa, Mat world) {
    if (!path || path->n == 0 || opacity <= 0.0f) return;
    if (paint->type != PAINT_COLOR && paint->type != PAINT_LINEAR &&
        paint->type != PAINT_RADIAL && paint->type != PAINT_PATTERN) return;

    int x0 = svg_f2i(floorf(bounds[0]), -1, c->w + 1, 0);
    int y0 = svg_f2i(floorf(bounds[1]), -1, c->h + 1, 0);
    int x1 = svg_f2i(ceilf(bounds[2]), -1, c->w + 1, 0);
    int y1 = svg_f2i(ceilf(bounds[3]), -1, c->h + 1, 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) return;

    Grad grad;
    int has_grad = 0;
    if (paint->type == PAINT_LINEAR || paint->type == PAINT_RADIAL) {
        grad_build(&grad, paint, grad_bbox ? grad_bbox : bounds, world);
        has_grad = 1;
    }
    Pattern pat;
    int has_pat = 0;
    if (paint->type == PAINT_PATTERN && paint->pat)
        has_pat = pattern_build(&pat, paint->pat, bounds, world, aa);
    int has_shader = has_grad || has_pat;
    Color solid = paint->color;

    /* Fully opaque solid fill at aa=1: coverage is binary, so a covered run
     * can be stored straight into the canvas without touching the coverage
     * buffer or running the blender.  This is the common case for thin
     * strokes and flat fills. */
    const int direct = (aa == 1 && !has_shader && !clip &&
                        solid.a * opacity >= 0.999f);

    Edge* edges = NULL;
    int ne = build_edges(path, &edges);
    if (ne == 0) { free(edges); return; }

    int ss_h = aa * c->h;
    const int row_len = x1 - x0;

    /* One arena allocation for all per-fill scratch buffers.  This matters:
     * a document with thousands of shapes would otherwise do thousands of
     * malloc/free pairs, which shows up quickly in profiles.  Layout is
     * head | nxt | active | cross | cov | alpha_row | ylast, all suitably
     * aligned because every element is 4 bytes. */
    size_t sz_head = (size_t)(ss_h + 1) * sizeof(int);
    size_t sz_n    = (size_t)ne * sizeof(int);
    size_t sz_cr   = (size_t)ne * sizeof(Cross);
    size_t sz_row  = (size_t)row_len * sizeof(float);
    unsigned char* arena = (unsigned char*)svg_malloc(
        sz_head + sz_n * 2 + sz_cr + sz_row * 2 + sz_n);
    if (!arena) { free(edges); return; }
    /* `restrict` on the row scratch buffers: they never overlap each other or
     * the canvas, which lets the compiler vectorise the coverage fold and the
     * span adds without runtime alias checks. */
    int* restrict       head      = (int*)arena;
    int* restrict       nxt       = (int*)(arena + sz_head);
    int* restrict       active    = (int*)(arena + sz_head + sz_n);
    Cross* restrict     cross     = (Cross*)(arena + sz_head + sz_n * 2);
    float* restrict     cov       = (float*)(arena + sz_head + sz_n * 2 + sz_cr);
    float* restrict     alpha_row = (float*)(arena + sz_head + sz_n * 2 + sz_cr + sz_row);
    int* restrict       ylast     = (int*)(arena + sz_head + sz_n * 2 + sz_cr + sz_row * 2);

    /* Precompute the first and last sub-scanline each edge touches, and
     * bucket the edges by their first sub-scanline, in a single pass. */
    for (int i = 0; i <= ss_h; i++) head[i] = -1;
    for (int i = 0; i < ne; i++) {
        /* Bucket by the sub-scanline centres that actually lie inside the
         * edge: sample s is at y = (s + 0.5) / aa, so the edge is live for
         * s in [ceil(ymin*aa - 0.5), ceil(ymax*aa - 0.5) - 1].  Using the
         * exact range means an edge in the active list always spans the
         * current sample, so the crossing loop needs no per-edge range test.
         * Double precision here is cheap (once per edge) and avoids losing
         * the 0.5 offset on large canvases. */
        /* Clamp in double: a finite but enormous coordinate (1e30) is still
         * outside int range, and the conversion would be undefined. */
        double vf = ceil((double)edges[i].ymin * (double)aa - 0.5);
        double vl = ceil((double)edges[i].ymax * (double)aa - 0.5) - 1.0;
        int f = (vf >= 0.0) ? ((vf > (double)(ss_h - 1)) ? ss_h - 1 : (int)vf) : 0;
        int l = (vl >= 0.0) ? ((vl > (double)(ss_h - 1)) ? ss_h - 1 : (int)vl) : -1;
        if (f < 0) f = 0;
        if (l > ss_h - 1) l = ss_h - 1;
        if (f > ss_h - 1) continue;    /* starts past the canvas */
        if (l < f) continue;           /* thinner than one sample */
        ylast[i] = l;
        nxt[i] = head[f];
        head[f] = i;
    }

    int na = 0;
    const float invaa = 1.0f / (float)aa;

    /* Coverage buffer is cleared lazily: `dirty_lo`/`dirty_hi` track the
     * range written last row, and only that slice is reset before reuse. */
    int dirty_lo = x0, dirty_hi = x0;

    for (int py = y0; py < y1; py++) {
        /* Clear only the pixels the previous row touched. */
        if (!direct && dirty_hi > dirty_lo) {
            int n = dirty_hi - dirty_lo;
            float* d = cov + (dirty_lo - x0);
            for (int p = 0; p < n; p++) d[p] = 0.0f;
        }
        int touched_lo = x1, touched_hi = x0;

        for (int sub = 0; sub < aa; sub++) {
            int sy = py * aa + sub;
            for (int e = head[sy]; e != -1; e = nxt[e]) active[na++] = e;
            {
                int w = 0;
                for (int i = 0; i < na; i++)
                    if (ylast[active[i]] >= sy) active[w++] = active[i];
                na = w;
            }
            if (na == 0) continue;

            const float y = ((float)sy + 0.5f) * invaa;
            int nc = 0;
            for (int i = 0; i < na; i++) {
                /* The bucket range is exact, so every edge in `active`
                 * spans this sample; no per-edge range test is needed. */
                const Edge* ed = &edges[active[i]];
                cross[nc].x = ed->x_ymin + (y - ed->ymin) * ed->invslope;
                cross[nc].dir = ed->dir;
                nc++;
            }
            if (nc < 2) continue;
            sort_cross(cross, nc);

            /* Walk the sorted crossings left to right.  Between two
             * consecutive crossings the winding number is constant, so we
             * add coverage for the whole run at once instead of testing
             * every sub-pixel.
             *
             * Coincident crossings (duplicate vertices from flattening) are
             * absorbed in one group: their directions are summed before the
             * next run is emitted, so winding never depends on the order
             * the sort produced within a group. */
            int sx_lo = x0 * aa, sx_hi = x1 * aa;
            int wind = 0;
            int run_start = sx_lo;
            int i = 0;
            while (i < nc) {
                float cx0 = cross[i].x;
                int sx = svg_f2i(floorf(cx0 * (float)aa), sx_lo - 1, sx_hi + 1, sx_lo);
                if (sx < run_start) sx = run_start;
                if (sx > sx_hi) sx = sx_hi;
                if (sx > run_start) {
                    int inside = (rule == 0) ? (wind != 0) : ((wind & 1) != 0);
                    if (inside) {
                        int p0 = run_start / aa;
                        int p1 = (sx + aa - 1) / aa;
                        if (p0 < x0) p0 = x0;
                        if (p1 > x1) p1 = x1;
                        if (p0 < touched_lo) touched_lo = p0;
                        if (p1 > touched_hi) touched_hi = p1;
                        if (direct) {
                            float* d = c->px + ((size_t)py * (size_t)c->w + (size_t)p0) * 4;
                            for (int p = 0, cnt = p1 - p0; p < cnt; p++) {
                                d[p * 4 + 0] = solid.r;
                                d[p * 4 + 1] = solid.g;
                                d[p * 4 + 2] = solid.b;
                                d[p * 4 + 3] = 1.0f;
                            }
                        } else {
                            add_run(cov, x0, run_start, sx, aa);
                        }
                    }
                    run_start = sx;
                }
                do { wind += cross[i].dir; i++; }
                while (i < nc && cross[i].x == cx0);
            }

            /* Tail run from the last crossing to the right edge. */
            if (run_start < sx_hi) {
                int inside = (rule == 0) ? (wind != 0) : ((wind & 1) != 0);
                if (inside) {
                    int p0 = run_start / aa;
                    int p1 = (sx_hi + aa - 1) / aa;
                    if (p0 < x0) p0 = x0;
                    if (p1 > x1) p1 = x1;
                    if (p0 < touched_lo) touched_lo = p0;
                    if (p1 > touched_hi) touched_hi = p1;
                    if (direct) {
                        float* d = c->px + ((size_t)py * (size_t)c->w + (size_t)p0) * 4;
                        for (int p = 0, cnt = p1 - p0; p < cnt; p++) {
                            d[p * 4 + 0] = solid.r;
                            d[p * 4 + 1] = solid.g;
                            d[p * 4 + 2] = solid.b;
                            d[p * 4 + 3] = 1.0f;
                        }
                    } else {
                        add_run(cov, x0, run_start, sx_hi, aa);
                    }
                }
            }
        }

        /* Direct fills never touch the coverage buffer. */
        if (direct) continue;

        /* Blend only the touched span.  Pixels outside it have zero
         * coverage and would be skipped by the blender anyway. */
        int b_lo = touched_hi > touched_lo ? touched_lo : x0;
        int b_hi = touched_hi > touched_lo ? touched_hi : x0;
        int b_len = b_hi - b_lo;

        if (!has_shader && !clip && b_len > 0) {
            float k = solid.a * opacity;
            if (k >= 0.999f) {
                blend_row_const(c, b_lo, py, b_len, solid, cov + (b_lo - x0));
            } else {
                const float* src = cov + (b_lo - x0);
                for (int p = 0; p < b_len; p++) alpha_row[p] = src[p] * k;
                blend_row_const(c, b_lo, py, b_len, solid, alpha_row);
            }
        } else if (b_len > 0 && has_shader && !clip) {
            /* Gradient or pattern, no clip: the per-pixel test is only
             * coverage. */
            for (int px = b_lo; px < b_hi; px++) {
                float cv = cov[px - x0];
                if (cv <= 0.0f) continue;
                Color col = has_pat ? pattern_eval(&pat, (float)px + 0.5f,
                                                    (float)py + 0.5f)
                                    : grad_eval(&grad, (float)px + 0.5f,
                                                (float)py + 0.5f);
                blend(c, px, py, col, col.a * opacity * cv);
            }
        } else if (b_len > 0 && !has_shader) {
            /* Solid colour, clipped: one mask lookup per pixel. */
            for (int px = b_lo; px < b_hi; px++) {
                float cv = cov[px - x0] * mask_get(clip, px, py);
                if (cv <= 0.0f) continue;
                blend(c, px, py, solid, solid.a * opacity * cv);
            }
        } else if (b_len > 0) {
            /* Gradient or pattern, clipped. */
            for (int px = b_lo; px < b_hi; px++) {
                float cv = cov[px - x0];
                if (cv <= 0.0f) continue;
                float m = mask_get(clip, px, py);
                if (m <= 0.0f) continue;
                cv *= m;
                if (cv <= 0.0f) continue;
                Color col = has_pat ? pattern_eval(&pat, (float)px + 0.5f,
                                                    (float)py + 0.5f)
                                    : grad_eval(&grad, (float)px + 0.5f,
                                                (float)py + 0.5f);
                blend(c, px, py, col, col.a * opacity * cv);
            }
        }

        dirty_lo = touched_lo;
        dirty_hi = touched_hi;
    }

    free(edges);
    free(arena);
    if (has_pat) pattern_free(&pat);
}

/* raster_gradient.c - Gradient LUTs and evaluation. */
#include "raster_internal.h"

/* ------------------------------------------------------------------ */
/* Gradients                                                           */
/* ------------------------------------------------------------------ */



static Color grad_sample(const Paint* p, float t) {
    if (p->spread == SPREAD_REFLECT) {
        t = fabsf(t);
        t = fmodf(t, 2.0f);
        if (t > 1.0f) t = 2.0f - t;
    } else if (p->spread == SPREAD_REPEAT) {
        t = fmodf(t, 1.0f);
        if (t < 0.0f) t += 1.0f;
    } else {
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    int n = p->nstops;
    if (n <= 0) return (Color){0, 0, 0, 0};
    if (n == 1) return p->stops[0].c;
    if (t < p->stops[0].off) return p->stops[0].c;
    if (t >= p->stops[n - 1].off) return p->stops[n - 1].c;
    for (int i = 0; i < n - 1; i++) {
        float o0 = p->stops[i].off, o1 = p->stops[i + 1].off;
        if (o0 == o1) continue;          /* hard stop: zero-length segment */
        if (t >= o0 && t < o1) {
            float f = (t - o0) / (o1 - o0);
            Color a = p->stops[i].c, b = p->stops[i + 1].c;
            Color r;
            r.r = a.r + (b.r - a.r) * f;
            r.g = a.g + (b.g - a.g) * f;
            r.b = a.b + (b.b - a.b) * f;
            r.a = a.a + (b.a - a.a) * f;
            return r;
        }
    }
    /* t sits exactly on a stop offset (a hard stop, or the last stop). */
    {
        int i = n - 1;
        while (i > 0 && p->stops[i].off > t) i--;
        return p->stops[i].c;
    }
}

void grad_build(Grad* g, const Paint* p, const float bbox[4], Mat world) {
    memset(g, 0, sizeof(*g));
    g->type = p->type;
    g->spread = p->spread;
    g->bx0 = bbox[0]; g->by0 = bbox[1];
    g->bw = bbox[2] - bbox[0];
    g->bh = bbox[3] - bbox[1];
    if (fabsf(g->bw) < 1e-9f) g->bw = 1.0f;
    if (fabsf(g->bh) < 1e-9f) g->bh = 1.0f;

    if (p->type == PAINT_LINEAR) {
        float x1, y1, x2, y2;
        if (p->units == UNITS_BBOX) {
            Pt a = mat_apply(p->tm, (Pt){p->x1, p->y1});
            Pt b = mat_apply(p->tm, (Pt){p->x2, p->y2});
            x1 = a.x; y1 = a.y; x2 = b.x; y2 = b.y;
            g->normalized = 1;
        } else {
            Mat gt = mat_mul(world, p->tm);
            Pt a = mat_apply(gt, (Pt){p->x1, p->y1});
            Pt b = mat_apply(gt, (Pt){p->x2, p->y2});
            x1 = a.x; y1 = a.y; x2 = b.x; y2 = b.y;
        }
        g->x1 = x1; g->y1 = y1;
        g->dx = x2 - x1; g->dy = y2 - y1;
        float len2 = g->dx * g->dx + g->dy * g->dy;
        g->invlen2 = len2 > 1e-12f ? 1.0f / len2 : 0.0f;
    } else {
        if (p->units == UNITS_BBOX) {
            Pt c = mat_apply(p->tm, (Pt){p->cx, p->cy});
            Pt f = mat_apply(p->tm, (Pt){p->fx, p->fy});
            g->cx = c.x; g->cy = c.y; g->fx = f.x; g->fy = f.y;
            g->r = p->r * mat_scale_factor(p->tm);
            g->normalized = 1;
        } else {
            Mat gt = mat_mul(world, p->tm);
            Pt c = mat_apply(gt, (Pt){p->cx, p->cy});
            Pt f = mat_apply(gt, (Pt){p->fx, p->fy});
            g->cx = c.x; g->cy = c.y; g->fx = f.x; g->fy = f.y;
            g->r = p->r * mat_scale_factor(gt);
        }
        /* SVG requires the focal point to lie inside the circle: if it is
         * outside, snap it to the circle along the centre->focal line. */
        {
            float rr = g->r > 1e-9f ? g->r : 1e-9f;
            g->r_inv = 1.0f / rr;
            float ddx = g->fx - g->cx, ddy = g->fy - g->cy;
            float dd2 = ddx * ddx + ddy * ddy;
            if (dd2 > rr * rr) {
                float dl = sqrtf(dd2);
                g->fx = g->cx + ddx * (rr / dl);
                g->fy = g->cy + ddy * (rr / dl);
                ddx = g->fx - g->cx; ddy = g->fy - g->cy;
                dd2 = ddx * ddx + ddy * ddy;
            }
            if (dd2 > 1e-12f) {
                g->rdx = ddx; g->rdy = ddy; g->rlen = sqrtf(dd2);
            } else {
                g->rdx = 0.0f; g->rdy = 0.0f; g->rlen = 0.0f;
            }
        }
    }
    /* Bake the stop ramp into a lookup table.  Sampling at bin centres
     * (i + 0.5) / N keeps t=0 and t=1 exactly representable at the ends,
     * so an unmodified colour is not darkened by double quantisation. */
    for (int i = 0; i < GRAD_LUT_N; i++)
        g->lut[i] = grad_sample(p, ((float)i + 0.5f) / (float)GRAD_LUT_N);
}

Color grad_eval(const Grad* g, float x, float y) {
    float t;
    if (g->type == PAINT_LINEAR) {
        float px = x, py = y;
        if (g->normalized) {
            px = (x - g->bx0) / g->bw;
            py = (y - g->by0) / g->bh;
        }
        t = ((px - g->x1) * g->dx + (py - g->y1) * g->dy) * g->invlen2;
    } else {
        float px = x, py = y;
        if (g->normalized) {
            px = (x - g->bx0) / g->bw;
            py = (y - g->by0) / g->bh;
        }
        if (g->rlen <= 0.0f) {
            /* Focal point at the centre: t is the normalised distance. */
            float ddx = px - g->cx, ddy = py - g->cy;
            t = sqrtf(ddx * ddx + ddy * ddy) * g->r_inv;
        } else {
            /* Intersect the ray F -> P with the gradient circle; the colour
             * parameter is the ratio |FP| / |FQ| = 1 / t_intersect. */
            float dx = px - g->fx, dy = py - g->fy;
            float a = dx * dx + dy * dy;
            if (a < 1e-12f) {
                t = 0.0f;
            } else {
                float fx = g->fx - g->cx, fy = g->fy - g->cy;
                float b = dx * fx + dy * fy;
                float c = fx * fx + fy * fy - g->r * g->r;
                float disc = b * b - a * c;
                if (disc <= 0.0f) {
                    t = 0.0f;
                } else {
                    float sq = sqrtf(disc);
                    /* a*t^2 + 2b*t + c = 0  =>  t = (-b ± sqrt(b²-ac))/a */
                    float t1 = (-b - sq) / a;
                    float t2 = (-b + sq) / a;
                    float ti = (t1 > 1e-9f) ? t1 : ((t2 > 1e-9f) ? t2 : 0.0f);
                    t = (ti > 1e-9f) ? (1.0f / ti) : 0.0f;
                }
            }
        }
    }
    /* Apply the spread mode per pixel: the LUT only covers one period, so a
     * repeating or reflecting gradient must wrap `t` before the lookup. */
    if (g->spread == SPREAD_REFLECT) {
        t = fabsf(t);
        t = fmodf(t, 2.0f);
        if (t > 1.0f) t = 2.0f - t;
    } else if (g->spread == SPREAD_REPEAT) {
        t = fmodf(t, 1.0f);
        if (t < 0.0f) t += 1.0f;
    } else {
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    /* Map [0,1] onto bin centres: index = floor(t*N), clamped. */
    int idx = svg_f2i(t * (float)GRAD_LUT_N, 0, GRAD_LUT_MASK, 0);
    if (idx < 0) idx = 0;
    if (idx > GRAD_LUT_MASK) idx = GRAD_LUT_MASK;
    return g->lut[idx];
}

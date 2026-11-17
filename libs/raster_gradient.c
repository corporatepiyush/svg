/* raster_gradient.c - Gradient LUTs and evaluation. */
#include "raster_internal.h"

static float s2l(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float l2s(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

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
    g->bw_inv = 1.0f / g->bw;
    g->bh_inv = 1.0f / g->bh;

    if (p->type == PAINT_LINEAR) {
        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        if (p->units == UNITS_BBOX) {
            Pt a = mat_apply(p->tm, (Pt){p->x1, p->y1});
            Pt b = mat_apply(p->tm, (Pt){p->x2, p->y2});
            x1 = a.x; y1 = a.y; x2 = b.x; y2 = b.y;
            g->normalized = 1;
        } else {
            /* userSpaceOnUse: the gradient lives in the user space of the
             * element that references it, so each pixel is mapped back into
             * that space before projection.  Forward-transforming the
             * gradient vector instead is only equivalent for a uniform
             * scale; under a non-uniform CTM it shears the ramp
             * (SVG 1.1 §13.2).  The geometry stays in user space and the
             * inverse CTM is carried for grad_eval(). */
            Mat gt = mat_mul(world, p->tm);
            g->x1 = x1 = p->x1; g->y1 = y1 = p->y1;
            g->dx = p->x2 - p->x1;
            g->dy = p->y2 - p->y1;
            x2 = p->x2; y2 = p->y2;
            (void)x2; (void)y2;
            g->normalized = 0;
            g->has_inv = mat_invert_checked(gt, &g->inv);
        }
        if (g->normalized) {
            g->x1 = x1; g->y1 = y1;
            g->dx = x2 - x1; g->dy = y2 - y1;
        }
        float len2 = g->dx * g->dx + g->dy * g->dy;
        g->invlen2 = len2 > 1e-12f ? 1.0f / len2 : 0.0f;
    } else {
        if (p->units == UNITS_BBOX) {
            /* objectBoundingBox: the evaluator works in bbox-normalised
             * coordinates, so transform the centre/focal and scale the
             * radius by the local factor in that same space.  (A non-uniform
             * or rotated gradientTransform cannot be represented exactly by
             * a single radius; userSpaceOnUse is the exact path.) */
            Pt c = mat_apply(p->tm, (Pt){p->cx, p->cy});
            Pt f = mat_apply(p->tm, (Pt){p->fx, p->fy});
            g->cx = c.x; g->cy = c.y; g->fx = f.x; g->fy = f.y;
            g->r = p->r * mat_scale_factor(p->tm);
            g->normalized = 1;
        } else {
            /* userSpaceOnUse: keep the definition in gradient space and
             * invert the combined transform per pixel.  This is exact for
             * non-uniform scale, shear and rotation, and it is also the
             * only form that honours gradientTransform correctly: the
             * evaluator maps device -> gradient space, so the radius must
             * stay in gradient units. */
            Mat gt = mat_mul(world, p->tm);
            g->cx = p->cx; g->cy = p->cy;
            g->fx = p->fx; g->fy = p->fy;
            g->r = p->r;
            g->normalized = 0;
            g->has_inv = mat_invert_checked(gt, &g->inv);
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
    if (p->linear) {
        /* Interpolate in linear light: convert the stops, build the LUT, then
         * encode the LUT back to sRGB so evaluation stays a table lookup. */
        Paint lin = *p;
        lin.linear = 0;
        if (lin.stops && lin.nstops > 0) {
            Stop* s2 = (Stop*)svg_malloc((size_t)lin.nstops * sizeof(Stop));
            if (s2) {
                memcpy(s2, lin.stops, (size_t)lin.nstops * sizeof(Stop));
                for (int i = 0; i < lin.nstops; i++) {
                    Color c = s2[i].c;
                    s2[i].c = (Color){s2l(c.r), s2l(c.g), s2l(c.b), c.a};
                }
                lin.stops = s2;
                for (int i = 0; i < GRAD_LUT_N; i++)
                    g->lut[i] = grad_sample(&lin, ((float)i + 0.5f) / (float)GRAD_LUT_N);
                for (int i = 0; i < GRAD_LUT_N; i++) {
                    Color c = g->lut[i];
                    g->lut[i] = (Color){l2s(c.r), l2s(c.g), l2s(c.b), c.a};
                }
                svg_mfree(s2);
                return;
            }
        }
    }
    /* Bake the stop ramp into a lookup table.  Sampling at bin centres
     * (i + 0.5) / N keeps t=0 and t=1 exactly representable at the ends,
     * so an unmodified colour is not darkened by double quantisation. */
    for (int i = 0; i < GRAD_LUT_N; i++)
        g->lut[i] = grad_sample(p, ((float)i + 0.5f) / (float)GRAD_LUT_N);
}

static inline Color grad_eval_inl(const Grad* g, float x, float y) {
    float t;
    if (g->type == PAINT_LINEAR) {
        float px = x, py = y;
        if (g->normalized) {
            px = (x - g->bx0) * g->bw_inv;
            py = (y - g->by0) * g->bh_inv;
        } else if (g->has_inv) {
            /* userSpaceOnUse: map the device pixel back into gradient space
             * through the inverse CTM, so a non-uniform transform is
             * reflected exactly rather than approximated. */
            Pt q = mat_apply(g->inv, (Pt){x, y});
            px = q.x; py = q.y;
        }
        t = ((px - g->x1) * g->dx + (py - g->y1) * g->dy) * g->invlen2;
    } else {
        float px = x, py = y;
        if (g->normalized) {
            px = (x - g->bx0) * g->bw_inv;
            py = (y - g->by0) * g->bh_inv;
        } else if (g->has_inv) {
            Pt q = mat_apply(g->inv, (Pt){x, y});
            px = q.x; py = q.y;
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
        /* fmodf() is a libm call in the per-pixel path; floorf() is one
         * instruction.  The range reduction is identical for the finite
         * coordinates a canvas can produce. */
        t = fabsf(t);
        t -= 2.0f * floorf(t * 0.5f);
        if (t > 1.0f) t = 2.0f - t;
    } else if (g->spread == SPREAD_REPEAT) {
        t -= floorf(t);
    } else {
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    /* Map [0,1] onto bin centres: index = floor(t*N), clamped.  Branch on
     * t rather than calling svg_f2i(): NaN and out-of-range values fall to
     * the same clamped result without an indirect call per pixel. */
    int idx;
    if (t >= 1.0f) idx = GRAD_LUT_MASK;
    else if (t > 0.0f) idx = (int)(t * (float)GRAD_LUT_N);
    else idx = 0;
    return g->lut[idx];
}

Color grad_eval(const Grad* g, float x, float y) {
    return grad_eval_inl(g, x, y);
}

/* Shade pixels x0..x0+n-1 of row y (pixel centres).  Same per-pixel maths as
 * grad_eval(); evaluating a whole row lets the compiler hoist the
 * type/units/spread branches out of the loop. */
void grad_eval_row(const Grad* g, int x0, int n, float y, Color* restrict out) {
    for (int i = 0; i < n; i++)
        out[i] = grad_eval_inl(g, (float)(x0 + i) + 0.5f, y);
}

/* raster_canvas.c - Canvas allocation and source-over blending. */
#include "raster_internal.h"

/* Canvas                                                              */
/* ------------------------------------------------------------------ */

Canvas* canvas_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    /* Guard the size computation against overflow before allocating. */
    if ((size_t)w > (size_t)-1 / (size_t)h / 4 / (unsigned)sizeof(float))
        return NULL;
    Canvas* c = (Canvas*)svg_malloc(sizeof(Canvas));
    if (!c) return NULL;
    c->w = w; c->h = h;
    c->px = (float*)svg_malloc((size_t)w * (size_t)h * 4 * sizeof(float));
    if (!c->px) { free(c); return NULL; }
    memset(c->px, 0, (size_t)w * (size_t)h * 4 * sizeof(float));
    return c;
}

void canvas_free(Canvas* c) {
    if (!c) return;
    free(c->px);
    free(c);
}

void canvas_clear(Canvas* c, Color col) {
    if (!c) return;
    size_t n = (size_t)c->w * (size_t)c->h;
    for (size_t i = 0; i < n; i++) {
        float* p = c->px + i * 4;
        p[0] = col.r; p[1] = col.g; p[2] = col.b; p[3] = col.a;
    }
}

void blend(Canvas* c, int x, int y, Color s, float a) {
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    float* d = c->px + ((size_t)y * (size_t)c->w + (size_t)x) * 4;
    float da = d[3];
    if (a >= 0.9995f && da <= 0.0005f) {
        d[0] = s.r; d[1] = s.g; d[2] = s.b; d[3] = a;
        return;
    }
    float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) { d[0] = d[1] = d[2] = d[3] = 0.0f; return; }
    float inv = 1.0f / oa;
    float wa = 1.0f - a;
    d[0] = (s.r * a + d[0] * da * wa) * inv;
    d[1] = (s.g * a + d[1] * da * wa) * inv;
    d[2] = (s.b * a + d[2] * da * wa) * inv;
    d[3] = oa;
}

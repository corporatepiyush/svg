/* raster_mask.c - 8-bit alpha clip masks. */
#include "raster_internal.h"

/* ------------------------------------------------------------------ */
/* Masks                                                               */
/* ------------------------------------------------------------------ */

Mask* mask_alloc(int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) {
        /* Degenerate mask: still return a valid object so callers can
         * distinguish "empty clip" from allocation failure. */
        Mask* m = (Mask*)svg_malloc(sizeof(Mask));
        if (!m) return NULL;
        m->x0 = x0; m->y0 = y0; m->x1 = x1; m->y1 = y1;
        m->empty = 1;
        m->m = (float*)svg_malloc(sizeof(float));
        if (!m->m) { free(m); return NULL; }
        m->m[0] = 0.0f;
        return m;
    }
    if ((size_t)(x1 - x0) > (size_t)-1 / (size_t)(y1 - y0) / sizeof(float))
        return NULL;
    Mask* m = (Mask*)svg_malloc(sizeof(Mask));
    if (!m) return NULL;
    m->x0 = x0; m->y0 = y0; m->x1 = x1; m->y1 = y1;
    m->empty = 0;
    size_t n = (size_t)(x1 - x0) * (size_t)(y1 - y0);
    m->m = (float*)svg_malloc(n * sizeof(float));
    if (!m->m) { free(m); return NULL; }
    memset(m->m, 0, n * sizeof(float));
    return m;
}

void mask_free(Mask* m) {
    if (!m) return;
    free(m->m);
    free(m);
}

float mask_get(const Mask* m, int x, int y) {
    if (!m) return 1.0f;
    if (x < m->x0 || y < m->y0 || x >= m->x1 || y >= m->y1) return 0.0f;
    return m->m[(size_t)(y - m->y0) * (size_t)(m->x1 - m->x0) + (size_t)(x - m->x0)];
}

Mask* mask_shift(const Mask* m, int x0, int y0, int x1, int y1) {
    if (!m) return NULL;
    Mask* r = mask_alloc(x0, y0, x1, y1);
    if (!r) return NULL;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            r->m[(size_t)(y - y0) * (size_t)(x1 - x0) + (size_t)(x - x0)] = mask_get(m, x, y);
    return r;
}

Mask* mask_intersect(const Mask* a, const Mask* b) {
    if (!a) return mask_shift(b, b->x0, b->y0, b->x1, b->y1);
    if (!b) return mask_shift(a, a->x0, a->y0, a->x1, a->y1);
    int x0 = SVG_MAX(a->x0, b->x0), y0 = SVG_MAX(a->y0, b->y0);
    int x1 = SVG_MIN(a->x1, b->x1), y1 = SVG_MIN(a->y1, b->y1);
    if (x0 >= x1 || y0 >= y1) return NULL;
    Mask* r = mask_alloc(x0, y0, x1, y1);
    if (!r) return NULL;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            r->m[(size_t)(y - y0) * (size_t)(x1 - x0) + (size_t)(x - x0)] =
                mask_get(a, x, y) * mask_get(b, x, y);
    return r;
}

/* path_util.c - Path ownership, copying, transformation and bbox. */
#include "internal.h"

void path_free(Path* p) {
    if (!p) return;
    if (p->shared_points) {
        /* Subpaths alias one external point buffer; free it once. */
        free(p->pts_arena);
    } else {
        for (int i = 0; i < p->n; i++) free(p->s[i].p);
    }
    free(p->s);
    free(p);
}

Path* path_copy(const Path* p) {
    if (!p) return NULL;
    Path* q = (Path*)svg_malloc(sizeof(Path));
    if (!q) return NULL;
    memset(q, 0, sizeof(*q));
    q->n = p->n;
    q->cap = p->n;
    /* A copy never shares the source's point arena: give it its own
     * per-subpath allocations so ownership stays unambiguous. */
    q->shared_points = 0;
    q->pts_arena = NULL;
    q->s = (SubPath*)svg_malloc((size_t)(p->n ? p->n : 1) * sizeof(SubPath));
    if (!q->s) { free(q); return NULL; }
    for (int i = 0; i < p->n; i++) {
        q->s[i].n = p->s[i].n;
        q->s[i].closed = p->s[i].closed;
        q->s[i].p = (Pt*)svg_malloc((size_t)(p->s[i].n ? p->s[i].n : 1) * sizeof(Pt));
        if (p->s[i].n && q->s[i].p) memcpy(q->s[i].p, p->s[i].p,
                                          (size_t)p->s[i].n * sizeof(Pt));
    }
    return q;
}

void path_transform(Path* p, Mat m) {
    if (!p) return;
    for (int i = 0; i < p->n; i++) {
        for (int k = 0; k < p->s[i].n; k++) {
            Pt q = p->s[i].p[k];
            p->s[i].p[k].x = m.m[0] * q.x + m.m[2] * q.y + m.m[4];
            p->s[i].p[k].y = m.m[1] * q.x + m.m[3] * q.y + m.m[5];
        }
    }
}

void path_bbox(const Path* p, float out[4]) {
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    if (!p) return;
    for (int i = 0; i < p->n; i++) {
        for (int k = 0; k < p->s[i].n; k++) {
            Pt q = p->s[i].p[k];
            if (q.x < out[0]) out[0] = q.x;
            if (q.y < out[1]) out[1] = q.y;
            if (q.x > out[2]) out[2] = q.x;
            if (q.y > out[3]) out[3] = q.y;
        }
    }
    if (out[0] > out[2]) { out[0] = out[1] = out[2] = out[3] = 0.0f; }
}

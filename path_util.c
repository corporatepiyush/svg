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
        if (!q->s[i].p) {
            /* Allocation failure: leave this subpath empty rather than
             * dereferencing NULL in memcpy below.  The copy keeps the
             * subpaths it could allocate. */
            q->s[i].n = 0;
            continue;
        }
        if (p->s[i].n)
            memcpy(q->s[i].p, p->s[i].p, (size_t)p->s[i].n * sizeof(Pt));
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

/* Total geometric length of a path, in user units.  Used by `pathLength`
 * scaling: the dash pattern is expressed in the author's declared length,
 * so it must be multiplied by (actual length / declared length). */
float path_length(const Path* p) {
    if (!p) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < p->n; i++) {
        const SubPath* s = &p->s[i];
        for (int k = 1; k < s->n; k++) {
            float dx = s->p[k].x - s->p[k - 1].x;
            float dy = s->p[k].y - s->p[k - 1].y;
            total += sqrtf(dx * dx + dy * dy);
        }
        if (s->closed && s->n > 2) {
            float dx = s->p[0].x - s->p[s->n - 1].x;
            float dy = s->p[0].y - s->p[s->n - 1].y;
            total += sqrtf(dx * dx + dy * dy);
        }
    }
    return total;
}

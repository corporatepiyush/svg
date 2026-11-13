/* svg_refs.c - Paint, clip and use reference resolution. */
#include "svg_internal.h"

/* Deep-copy src into dst, releasing anything dst already owns. */
static void paint_copy(Paint* dst, const Paint* src) {
    if (dst == src) return;
    free(dst->stops);
    free(dst->ref);
    *dst = *src;
    dst->stops = NULL;
    dst->nstops = 0;
    dst->ref = src->ref ? svg_strdup(src->ref) : NULL;
    if (src->stops && src->nstops > 0) {
        dst->stops = (Stop*)svg_malloc((size_t)src->nstops * sizeof(Stop));
        if (dst->stops) {
            memcpy(dst->stops, src->stops, (size_t)src->nstops * sizeof(Stop));
            dst->nstops = src->nstops;
        }
    }
}

static void resolve_paint_refs(Node* n, IdMap* ids) {
    if (!n) return;
    for (int k = 0; k < 2; k++) {
        Paint* p = (k == 0) ? &n->fill : &n->stroke;
        if (p->type == PAINT_REF && p->ref) {
            Node* g = idmap_get(ids, p->ref);
            if (g && g->type == NODE_GRADDEF) {
                paint_copy(p, &g->fill);
            } else {
                free(p->stops); p->stops = NULL; p->nstops = 0;
                free(p->ref);   p->ref = NULL;
                p->type = PAINT_NONE;
            }
        }
    }
    for (int i = 0; i < n->nkids; i++) resolve_paint_refs(n->kids[i], ids);
}

static void resolve_gradient_hrefs(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->type == NODE_GRADDEF && n->fill.ref && n->fill.nstops == 0) {
        Node* g = idmap_get(ids, n->fill.ref);
        if (g && g->type == NODE_GRADDEF && g != n) {
            Paint ref = g->fill;
            ref.ref = NULL;               /* do not inherit the href chain */
            paint_copy(&n->fill, &ref);
        }
        free(n->fill.ref);
        n->fill.ref = NULL;
    }
    for (int i = 0; i < n->nkids; i++) resolve_gradient_hrefs(n->kids[i], ids);
}

static void resolve_clip_refs(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->clip_ref) {
        Node* c = idmap_get(ids, n->clip_ref);
        if (c && c->type == NODE_CLIPDEF) n->clip = c;
        free(n->clip_ref);
        n->clip_ref = NULL;
    }
    for (int i = 0; i < n->nkids; i++) resolve_clip_refs(n->kids[i], ids);
}

static Node* clone_node(const Node* n) {
    if (!n) return NULL;
    Node* c = (Node*)svg_malloc(sizeof(Node));
    if (!c) return NULL;
    *c = *n;

    c->id = NULL;
    c->kids = NULL;
    c->nkids = 0;
    c->path = n->path ? path_copy(n->path) : NULL;

    for (int k = 0; k < 2; k++) {
        Paint* d = (k == 0) ? &c->fill : &c->stroke;
        const Paint* s = (k == 0) ? &n->fill : &n->stroke;
        d->stops = NULL; d->nstops = 0;
        d->ref = s->ref ? svg_strdup(s->ref) : NULL;
        if (s->stops && s->nstops > 0) {
            d->stops = (Stop*)svg_malloc((size_t)s->nstops * sizeof(Stop));
            if (d->stops) {
                memcpy(d->stops, s->stops, (size_t)s->nstops * sizeof(Stop));
                d->nstops = s->nstops;
            }
        }
    }

    c->dash = NULL;
    c->ndash = 0;
    if (n->dash && n->ndash > 0) {
        c->dash = (float*)svg_malloc((size_t)n->ndash * sizeof(float));
        if (c->dash) {
            memcpy(c->dash, n->dash, (size_t)n->ndash * sizeof(float));
            c->ndash = n->ndash;
        }
    }

    c->text = n->text ? svg_strdup(n->text) : NULL;
    c->clip_ref = NULL;
    c->use_ref = NULL;

    for (int i = 0; i < n->nkids; i++) {
        Node* kid = clone_node(n->kids[i]);
        if (kid && !node_add_child(c, kid)) svg_free(kid);
    }
    return c;
}

static void resolve_use(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->use_ref) {
        Node* target = idmap_get(ids, n->use_ref);
        if (target && target != n) {
            Node* clone = clone_node(target);
            if (clone) {
                clone->tm = mat_mul(mat_translate(n->use_x, n->use_y), clone->tm);
                free(n->kids);
                n->kids = NULL;
                n->nkids = 0;
                if (!node_add_child(n, clone)) svg_free(clone);
            }
        }
        free(n->use_ref);
        n->use_ref = NULL;
    }
    for (int i = 0; i < n->nkids; i++) resolve_use(n->kids[i], ids);
}

void svg_resolve(Node* n, IdMap* ids) {
    if (!n) return;
    resolve_gradient_hrefs(n, ids);
    resolve_paint_refs(n, ids);
    resolve_clip_refs(n, ids);
    resolve_use(n, ids);
}

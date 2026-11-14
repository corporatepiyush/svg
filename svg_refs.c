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
            if (g && g->type == NODE_GRADDEF && g->fill.nstops > 0) {
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

/* ------------------------------------------------------------------ */
/* Gradient href chains                                                */
/* ------------------------------------------------------------------ */

static int grad_ref_pending(const Node* n) {
    return n->type == NODE_GRADDEF && n->fill.ref && n->fill.nstops == 0;
}

static int gradient_refs_pending(Node* n) {
    if (!n) return 0;
    if (grad_ref_pending(n)) return 1;
    for (int i = 0; i < n->nkids; i++)
        if (gradient_refs_pending(n->kids[i])) return 1;
    return 0;
}

/* One resolution round: copy whatever a referenced gradient has already
 * resolved.  An href may point at a gradient defined *later* in the document
 * and chains may be several hops long, so the driver below iterates. */
static void resolve_gradient_hrefs_once(Node* n, IdMap* ids) {
    if (!n) return;
    if (grad_ref_pending(n)) {
        Node* g = idmap_get(ids, n->fill.ref);
        if (g && g->type == NODE_GRADDEF && g != n && g->fill.nstops > 0) {
            Paint ref = g->fill;
            ref.ref = NULL;               /* do not inherit the href chain */
            paint_copy(&n->fill, &ref);
            free(n->fill.ref);            /* resolved: drop the href */
            n->fill.ref = NULL;
        }
    }
    for (int i = 0; i < n->nkids; i++) resolve_gradient_hrefs_once(n->kids[i], ids);
}

/* Drop any still-unresolved href (a cycle or a dangling id) so it does not
 * leak and cannot be mistaken for a stop list. */
static void resolve_gradient_hrefs_clear(Node* n) {
    if (!n) return;
    if (n->type == NODE_GRADDEF && n->fill.ref && n->fill.nstops == 0) {
        free(n->fill.ref);
        n->fill.ref = NULL;
    }
    for (int i = 0; i < n->nkids; i++) resolve_gradient_hrefs_clear(n->kids[i]);
}

/* Resolve href chains to a fixed point.  Each round resolves at least one hop
 * for every chain, so the number of rounds is bounded by the longest chain;
 * the cap keeps a reference cycle (a -> b -> a) from spinning forever. */
static void resolve_gradient_hrefs(Node* n, IdMap* ids, int max_rounds) {
    if (max_rounds <= 0) max_rounds = 1;
    for (int r = 0; r < max_rounds; r++) {
        if (!gradient_refs_pending(n)) break;
        resolve_gradient_hrefs_once(n, ids);
    }
    resolve_gradient_hrefs_clear(n);
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

/* ------------------------------------------------------------------ */
/* <use> shadow trees                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    IdMap* ids;
    int    max_nodes;      /* total cloned nodes allowed */
    int    made;           /* clones created so far      */
    int    failed;         /* an allocation failed: stop expanding */
    int    truncated;      /* the node budget ran out    */
} UseCtx;

/* Deep-copy a subtree.  The copy keeps the referenced element's own style;
 * use_apply_style() then lets the use site supply anything the target did not
 * specify, which is what SVG's shadow-tree inheritance requires.  use_ref and
 * clip_ref are carried over so uses and clips nested inside the target still
 * resolve on a later pass. */
static Node* clone_node(const Node* n, UseCtx* uc) {
    if (!n || uc->failed) return NULL;
    if (uc->made >= uc->max_nodes) { uc->truncated = 1; return NULL; }
    Node* c = (Node*)svg_malloc(sizeof(Node));
    if (!c) { uc->failed = 1; return NULL; }
    *c = *n;
    uc->made++;

    c->id = NULL;
    c->kids = NULL;
    c->nkids = 0;
    c->path = n->path ? path_copy(n->path) : NULL;
    if (n->path && !c->path) uc->failed = 1;

    for (int k = 0; k < 2; k++) {
        Paint* d = (k == 0) ? &c->fill : &c->stroke;
        const Paint* s = (k == 0) ? &n->fill : &n->stroke;
        d->stops = NULL; d->nstops = 0;
        d->ref = s->ref ? svg_strdup(s->ref) : NULL;
        if (s->ref && !d->ref) uc->failed = 1;
        if (s->stops && s->nstops > 0) {
            d->stops = (Stop*)svg_malloc((size_t)s->nstops * sizeof(Stop));
            if (d->stops) {
                memcpy(d->stops, s->stops, (size_t)s->nstops * sizeof(Stop));
                d->nstops = s->nstops;
            } else {
                uc->failed = 1;
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
        } else {
            uc->failed = 1;
        }
    }

    c->text = n->text ? svg_strdup(n->text) : NULL;
    if (n->text && !c->text) uc->failed = 1;

    /* Keep the references (deep copies) so nested use/clip inside the
     * shadow tree still resolve; the caller re-runs clip resolution. */
    c->clip_ref = n->clip_ref ? svg_strdup(n->clip_ref) : NULL;
    if (n->clip_ref && !c->clip_ref) uc->failed = 1;
    c->use_ref = n->use_ref ? svg_strdup(n->use_ref) : NULL;
    if (n->use_ref && !c->use_ref) uc->failed = 1;

    /* A <symbol> is only instantiated by <use>, so a clone of one becomes a
     * plain group that renders. */
    if (c->type == NODE_SYMBOL) c->type = NODE_GROUP;

    for (int i = 0; i < n->nkids; i++) {
        Node* kid = clone_node(n->kids[i], uc);
        if (kid && !node_add_child(c, kid)) svg_free(kid);
        if (uc->failed) break;
    }
    return c;
}

/* Let the use site supply a property the referenced element did not specify.
 * `spec` records which inheritable properties the tree builder actually saw on
 * the element, so an explicit value on the target is never overwritten. */
static void use_inherit_paint(Paint* dst, const Paint* src, unsigned spec_bit,
                              unsigned* dst_spec) {
    if (*dst_spec & spec_bit) return;    /* target specified it: keep it */
    if (src->type == PAINT_NONE && !src->ref && src->nstops == 0) return;
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
    *dst_spec |= spec_bit;
}

static void use_apply_style(Node* n, const Node* use) {
    if (!n || !use) return;
    use_inherit_paint(&n->fill, &use->fill, (unsigned)SPEC_FILL, &n->spec);
    use_inherit_paint(&n->stroke, &use->stroke, (unsigned)SPEC_STROKE, &n->spec);
    for (int i = 0; i < n->nkids; i++) use_apply_style(n->kids[i], use);
}

/* Drop any still-unresolved use ref (cycle, dangling id, or a truncated
 * expansion) so it cannot leak or render as an empty group. */
static void resolve_use_clear(Node* n) {
    if (!n) return;
    if (n->use_ref) {
        free(n->use_ref);
        n->use_ref = NULL;
    }
    for (int i = 0; i < n->nkids; i++) resolve_use_clear(n->kids[i]);
}

/* Resolve every <use> in the subtree.  Returns 1 if any work was done, so the
 * driver can iterate to a fixed point (resolving one use can expose another).
 * The caller frees use_ref before recursing, so a use is expanded once. */
static int resolve_use(Node* n, IdMap* ids, UseCtx* uc) {
    if (!n || uc->failed) return 0;
    int used = 0;
    if (n->use_ref) {
        char* ref = n->use_ref;
        n->use_ref = NULL;               /* take ownership before recursing */
        Node* target = idmap_get(ids, ref);
        if (target && target != n) {
            Node* clone = clone_node(target, uc);
            if (clone) {
                use_apply_style(clone, n);
                clone->tm = mat_mul(mat_translate(n->use_x, n->use_y), clone->tm);
                resolve_clip_refs(clone, ids);
                /* A <use> never has children of its own: the shadow tree
                 * replaces them.  Free each node, not just the array. */
                for (int i = 0; i < n->nkids; i++) svg_free(n->kids[i]);
                free(n->kids);
                n->kids = NULL;
                n->nkids = 0;
                if (!node_add_child(n, clone)) svg_free(clone);
            }
        }
        free(ref);
        used = 1;
    }
    for (int i = 0; i < n->nkids; i++)
        if (resolve_use(n->kids[i], ids, uc)) used = 1;
    return used;
}

int svg_resolve(Node* n, IdMap* ids, const svg2img_limits* lim) {
    if (!n) return 0;
    resolve_gradient_hrefs(n, ids, 64);
    resolve_paint_refs(n, ids);
    resolve_clip_refs(n, ids);

    UseCtx uc;
    uc.ids = ids;
    uc.max_nodes = lim ? lim->max_use_expansion : SVG2IMG_LIMIT_MAX_USE_EXPANSION;
    uc.made = 0;
    uc.failed = 0;
    uc.truncated = 0;

    /* A use can reference a group that contains another use, and forward
     * references mean order alone is not enough, so iterate.  Each pass either
     * expands at least one use (made increases) or stops; the budget bounds
     * the total work. */
    for (int pass = 0; pass < 64; pass++) {
        int before = uc.made;
        if (!resolve_use(n, ids, &uc)) break;
        if (uc.failed || uc.made == before) break;
    }
    /* Anything left with an unresolved ref (cycle, dangling id, or a truncated
     * expansion) is dropped so it cannot leak or render as an empty group. */
    resolve_use_clear(n);
    return uc.truncated || uc.failed;
}
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
            if (g && g->type == NODE_PATTERNDEF && g->pat_w > 0.0f && g->pat_h > 0.0f) {
                free(p->stops); p->stops = NULL; p->nstops = 0;
                p->type = PAINT_PATTERN;
                p->pat = g;
            } else if (g && g->type == NODE_GRADDEF && g->fill.nstops > 0) {
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
    if (n->mask_ref) {
        Node* m = idmap_get(ids, n->mask_ref);
        if (m && m->type == NODE_MASKDEF) n->mask = m;
        free(n->mask_ref);
        n->mask_ref = NULL;
    }
    if (n->filter_ref) {
        Node* f = idmap_get(ids, n->filter_ref);
        if (f && f->type == NODE_FILTERDEF) n->filter = f;
        free(n->filter_ref);
        n->filter_ref = NULL;
    }
    if (n->marker_start_ref) {
        Node* m = idmap_get(ids, n->marker_start_ref);
        if (m && m->type == NODE_MARKERDEF) n->marker_start = m;
        free(n->marker_start_ref); n->marker_start_ref = NULL;
    }
    if (n->marker_mid_ref) {
        Node* m = idmap_get(ids, n->marker_mid_ref);
        if (m && m->type == NODE_MARKERDEF) n->marker_mid = m;
        free(n->marker_mid_ref); n->marker_mid_ref = NULL;
    }
    if (n->marker_end_ref) {
        Node* m = idmap_get(ids, n->marker_end_ref);
        if (m && m->type == NODE_MARKERDEF) n->marker_end = m;
        free(n->marker_end_ref); n->marker_end_ref = NULL;
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
    c->font_owned = NULL;
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

    /* Text runs own their string and paint copies; a shallow struct copy
     * would leave the clone and the original freeing the same memory. */
    c->chunks = NULL;
    c->nchunks = 0;
    if (n->nchunks > 0 && n->chunks) {
        c->chunks = (TextChunk*)svg_malloc((size_t)n->nchunks * sizeof(TextChunk));
        if (!c->chunks) {
            uc->failed = 1;
        } else {
            memcpy(c->chunks, n->chunks, (size_t)n->nchunks * sizeof(TextChunk));
            for (int i = 0; i < n->nchunks; i++) {
                TextChunk* d = &c->chunks[i];
                TextChunk* sc = &n->chunks[i];
                d->text = sc->text ? svg_strdup(sc->text) : NULL;
                d->fill.stops = NULL;
                d->fill.nstops = 0;
                d->fill.ref = sc->fill.ref ? svg_strdup(sc->fill.ref) : NULL;
                d->rot = NULL;
                d->nrot = 0;
                if (sc->rot && sc->nrot > 0) {
                    d->rot = (float*)svg_malloc((size_t)sc->nrot * sizeof(float));
                    if (d->rot) {
                        memcpy(d->rot, sc->rot, (size_t)sc->nrot * sizeof(float));
                        d->nrot = sc->nrot;
                    }
                }
                d->path = sc->path ? svg_strdup(sc->path) : NULL;
                d->path_ref = sc->path_ref ? svg_strdup(sc->path_ref) : NULL;
                d->refpath = sc->refpath ? path_copy(sc->refpath) : NULL;
                if (sc->fill.stops && sc->fill.nstops > 0) {
                    d->fill.stops = (Stop*)svg_malloc(
                        (size_t)sc->fill.nstops * sizeof(Stop));
                    if (d->fill.stops) {
                        memcpy(d->fill.stops, sc->fill.stops,
                               (size_t)sc->fill.nstops * sizeof(Stop));
                        d->fill.nstops = sc->fill.nstops;
                    }
                }
                if ((sc->text && !d->text) || (sc->fill.ref && !d->fill.ref))
                    uc->failed = 1;
            }
            c->nchunks = n->nchunks;
        }
    }

    /* Filter primitives own their parameter strings; a shallow struct copy
     * would free the original's strings twice. */
    c->prims = NULL;
    c->nprims = 0;
    if (n->nprims > 0 && n->prims) {
        c->prims = (FilterPrim*)svg_malloc((size_t)n->nprims * sizeof(FilterPrim));
        if (!c->prims) {
            uc->failed = 1;
        } else {
            memcpy(c->prims, n->prims, (size_t)n->nprims * sizeof(FilterPrim));
            for (int i = 0; i < n->nprims; i++) {
                FilterPrim* d = &c->prims[i];
                const FilterPrim* sc = &n->prims[i];
                d->in = sc->in ? svg_strdup(sc->in) : NULL;
                d->in2 = sc->in2 ? svg_strdup(sc->in2) : NULL;
                d->result = sc->result ? svg_strdup(sc->result) : NULL;
                d->href = sc->href ? svg_strdup(sc->href) : NULL;
                d->nmerge = 0;
                for (int m = 0; m < sc->nmerge && m < 8; m++)
                    d->merge_in[d->nmerge++] =
                        sc->merge_in[m] ? svg_strdup(sc->merge_in[m]) : NULL;
                if ((sc->in && !d->in) || (sc->in2 && !d->in2) ||
                    (sc->result && !d->result) || (sc->href && !d->href))
                    uc->failed = 1;
                for (int m = 0; m < sc->nmerge && m < 8; m++)
                    if (sc->merge_in[m] && !d->merge_in[m]) uc->failed = 1;
            }
            c->nprims = n->nprims;
        }
    }

    /* Keep the references (deep copies) so nested use/clip inside the
     * shadow tree still resolve; the caller re-runs clip resolution. */
    c->clip_ref = n->clip_ref ? svg_strdup(n->clip_ref) : NULL;
    if (n->clip_ref && !c->clip_ref) uc->failed = 1;
    c->use_ref = n->use_ref ? svg_strdup(n->use_ref) : NULL;
    if (n->use_ref && !c->use_ref) uc->failed = 1;

    /* A <symbol> is only instantiated by <use>, so a clone of one becomes a
     * plain group that renders.  NODE_IGNORE (e.g. <defs>) stays
     * non-rendering; its children are definitions, not the referenced
     * content, and raster_node skips it. */
    if (c->type == NODE_SYMBOL) c->type = NODE_GROUP;

    for (int i = 0; i < n->nkids; i++) {
        Node* kid = clone_node(n->kids[i], uc);
        if (kid && !node_add_child(c, kid)) svg_free(kid);
        if (uc->failed) break;
    }
    return c;
}

/* Deep-copy an inherited paint value (stops and href included). */
static void paint_inherit_copy(Paint* dst, const Paint* src) {
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

/* Let the use site supply a property the referenced root did not specify.
 * `spec` records which inheritable properties the element specified itself,
 * so an explicit value on the target is never overwritten. */
static void use_inherit_paint(Paint* dst, const Paint* src, unsigned spec_bit,
                              unsigned* dst_spec) {
    if (*dst_spec & spec_bit) return;    /* target specified it: keep it */
    if (src->type == PAINT_NONE && !src->ref && src->nstops == 0) return;
    paint_inherit_copy(dst, src);
    *dst_spec |= spec_bit;
}

/* Deep-copy a dash list (the array is owned per node). */
static void dash_inherit(Node* n, const Node* parent) {
    free(n->dash);
    n->dash = NULL;
    n->ndash = 0;
    if (parent->dash && parent->ndash > 0) {
        n->dash = (float*)svg_malloc(sizeof(float) * (size_t)parent->ndash);
        if (n->dash) {
            memcpy(n->dash, parent->dash, sizeof(float) * (size_t)parent->ndash);
            n->ndash = parent->ndash;
        }
    }
    n->dashoff = parent->dashoff;
}

/* Propagate already-resolved inherited values into a shadow-tree descendant
 * that did not specify the property itself.  Descendants inherit from their
 * shadow-tree parent, not directly from the use element: a target that sets
 * fill="blue" must keep blue for its children even when the use site sets
 * fill="red". */
/* Text runs bake their style at build time, so an inherited text-anchor
 * must be pushed into them when the shadow tree re-resolves it. */
static void push_anchor(Node* n) {
    if (!n || !n->chunks) return;
    for (int i = 0; i < n->nchunks; i++) n->chunks[i].anchor = n->anchor;
}

static void use_apply_child(Node* n, const Node* parent) {
    if (!n || !parent) return;
    if (!(n->spec & SPEC_FILL))   paint_inherit_copy(&n->fill, &parent->fill);
    if (!(n->spec & SPEC_STROKE)) paint_inherit_copy(&n->stroke, &parent->stroke);
    if (!(n->spec & SPEC_FONT_SIZE))     n->fontsize = parent->fontsize;
    if (!(n->spec & SPEC_FONT_WEIGHT))   n->bold = parent->bold;
    if (!(n->spec & SPEC_FONT_STYLE))    n->italic = parent->italic;
    if (!(n->spec & SPEC_TEXT_ANCHOR))   n->anchor = parent->anchor;
    if (!(n->spec & SPEC_LETTER_SP))     n->letterspacing = parent->letterspacing;
    if (!(n->spec & SPEC_VISIBILITY))    n->visible = parent->visible;
    if (!(n->spec & SPEC_STROKE_WIDTH))  n->sw = parent->sw;
    if (!(n->spec & SPEC_LINECAP))       n->linecap = parent->linecap;
    if (!(n->spec & SPEC_LINEJOIN))      n->linejoin = parent->linejoin;
    if (!(n->spec & SPEC_MITERLIMIT))    n->miterlimit = parent->miterlimit;
    if (!(n->spec & SPEC_FILL_RULE))     n->fillrule = parent->fillrule;
    if (!(n->spec & SPEC_CLIP_RULE))     n->clip_rule = parent->clip_rule;
    if (!(n->spec & SPEC_FILL_OPACITY))  n->fillop = parent->fillop;
    if (!(n->spec & SPEC_STROKE_OPACITY)) n->strokeop = parent->strokeop;
    if (!(n->spec & SPEC_DASH))          dash_inherit(n, parent);
    if (!(n->spec & SPEC_DASH_OFFSET))   n->dashoff = parent->dashoff;
    push_anchor(n);
    for (int i = 0; i < n->nkids; i++) use_apply_child(n->kids[i], n);
}

static void use_apply_style(Node* n, const Node* use) {
    if (!n || !use) return;
    use_inherit_paint(&n->fill, &use->fill, (unsigned)SPEC_FILL, &n->spec);
    use_inherit_paint(&n->stroke, &use->stroke, (unsigned)SPEC_STROKE, &n->spec);

    /* SVG 1.1 §5.6: the use element's inherited properties supply any the
     * referenced root did not set.  Only the root takes values from the use
     * site; descendants inherit within the shadow tree.  The use element's
     * *computed* value is what inherits, whether the use specified it or
     * inherited it from its own parent. */
    if (!(n->spec & SPEC_FONT_SIZE))      n->fontsize = use->fontsize;
    if (!(n->spec & SPEC_FONT_WEIGHT))    n->bold = use->bold;
    if (!(n->spec & SPEC_FONT_STYLE))     n->italic = use->italic;
    if (!(n->spec & SPEC_TEXT_ANCHOR))    n->anchor = use->anchor;
    if (!(n->spec & SPEC_LETTER_SP))      n->letterspacing = use->letterspacing;
    if (!(n->spec & SPEC_VISIBILITY))     n->visible = use->visible;
    if (!(n->spec & SPEC_STROKE_WIDTH))   n->sw = use->sw;
    if (!(n->spec & SPEC_LINECAP))        n->linecap = use->linecap;
    if (!(n->spec & SPEC_LINEJOIN))       n->linejoin = use->linejoin;
    if (!(n->spec & SPEC_MITERLIMIT))     n->miterlimit = use->miterlimit;
    if (!(n->spec & SPEC_FILL_RULE))      n->fillrule = use->fillrule;
    if (!(n->spec & SPEC_CLIP_RULE))      n->clip_rule = use->clip_rule;
    if (!(n->spec & SPEC_FILL_OPACITY))   n->fillop = use->fillop;
    if (!(n->spec & SPEC_STROKE_OPACITY)) n->strokeop = use->strokeop;
    if (!(n->spec & SPEC_DASH))           dash_inherit(n, use);
    if (!(n->spec & SPEC_DASH_OFFSET))    n->dashoff = use->dashoff;
    push_anchor(n);

    for (int i = 0; i < n->nkids; i++) use_apply_child(n->kids[i], n);
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
                /* The use's x/y translation applies to the whole shadow
                 * tree.  For a symbol, the viewport mapping (viewBox ->
                 * use width/height) is applied first, then x/y. */
                Mat use_tm = mat_translate(n->use_x, n->use_y);
                if (target->type == NODE_SYMBOL) {
                    float vw = n->use_w > 0.0f ? n->use_w : 100.0f;
                    float vh = n->use_h > 0.0f ? n->use_h : 100.0f;
                    clone->vp_clip = 1;
                    clone->vp_x = 0.0f; clone->vp_y = 0.0f;
                    clone->vp_w = vw; clone->vp_h = vh;
                    if (clone->symbol_vb_valid) {
                        float sx = vw / clone->symbol_vb[2];
                        float sy = vh / clone->symbol_vb[3];
                        if (clone->symbol_par_none) {
                            clone->tm = mat_mul(mat_scale(sx, sy),
                                        mat_translate(-clone->symbol_vb[0],
                                                      -clone->symbol_vb[1]));
                        } else {
                            float s = clone->symbol_par_slice ? (sx > sy ? sx : sy)
                                                              : (sx < sy ? sx : sy);
                            float ex = (float)vw - clone->symbol_vb[2] * s;
                            float ey = (float)vh - clone->symbol_vb[3] * s;
                            float ox = (clone->symbol_par_xa == 1) ? ex * 0.5f
                                 : (clone->symbol_par_xa == 2 ? ex : 0.0f);
                            float oy = (clone->symbol_par_ya == 1) ? ey * 0.5f
                                 : (clone->symbol_par_ya == 2 ? ey : 0.0f);
                            clone->tm = mat_mul(mat_translate(ox, oy),
                                        mat_mul(mat_scale(s, s),
                                        mat_translate(-clone->symbol_vb[0],
                                                      -clone->symbol_vb[1])));
                        }
                    }
                }
                clone->tm = mat_mul(use_tm, clone->tm);
                resolve_clip_refs(clone, ids);
                /* A <use> never has children of its own: the shadow tree
                 * replaces them.  Free each node, not just the array, and
                 * unregister their ids first so the map cannot hand out a
                 * dangling pointer to a later <use href="#id">.
                 * The use element itself keeps its own id (if any). */
                for (int i = 0; i < n->nkids; i++)
                    svg_free_detached(n->kids[i], ids);
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

/* Copy the attributes a pattern inherits from the pattern it references.
 * Chains are possible, so iterate a bounded number of rounds. */
static int pattern_href_pending(Node* n) {
    return n->type == NODE_PATTERNDEF && n->fill.ref &&
           !(n->pat_w > 0.0f) && !(n->pat_h > 0.0f);
}

static int pattern_hrefs_pending(Node* n) {
    if (!n) return 0;
    if (pattern_href_pending(n)) return 1;
    for (int i = 0; i < n->nkids; i++)
        if (pattern_hrefs_pending(n->kids[i])) return 1;
    return 0;
}

static void resolve_pattern_hrefs_once(Node* n, IdMap* ids) {
    if (!n) return;
    if (pattern_href_pending(n)) {
        Node* t = idmap_get(ids, n->fill.ref);
        if (t && t->type == NODE_PATTERNDEF && t != n && t->pat_w > 0.0f) {
            n->pat_units = t->pat_units;
            n->pat_content_units = t->pat_content_units;
            n->pat_x = t->pat_x; n->pat_y = t->pat_y;
            n->pat_w = t->pat_w; n->pat_h = t->pat_h;
            n->pat_tm = t->pat_tm;
            if (t->pat_vb_valid) {
                for (int i = 0; i < 4; i++) n->pat_vb[i] = t->pat_vb[i];
                n->pat_vb_valid = 1;
            }
            n->pat_par_none = t->pat_par_none;
            n->pat_par_slice = t->pat_par_slice;
            n->pat_par_xa = t->pat_par_xa;
            n->pat_par_ya = t->pat_par_ya;
        }
    }
    for (int i = 0; i < n->nkids; i++) resolve_pattern_hrefs_once(n->kids[i], ids);
}

/* textPath may reference a <path> by id; the referenced geometry is copied
 * because a chunk owns its path and frees it with the tree. */
static void resolve_text_paths(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->chunks) {
        for (int i = 0; i < n->nchunks; i++) {
            TextChunk* ch = &n->chunks[i];
            if (!ch->path_ref) continue;
            Node* ref = idmap_get(ids, ch->path_ref);
            if (ref && ref->type == NODE_SHAPE && ref->path && ref->path->n > 0) {
                path_free(ch->refpath);
                ch->refpath = path_copy(ref->path);
            }
            free(ch->path_ref);
            ch->path_ref = NULL;
        }
    }
    for (int i = 0; i < n->nkids; i++) resolve_text_paths(n->kids[i], ids);
}

/* feImage may reference an element in the same document (SVG 1.1 15.2.12):
 * the referenced node is rendered as the image. */
static void resolve_filter_image_refs(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->type == NODE_FILTERDEF && n->prims) {
        for (int i = 0; i < n->nprims; i++) {
            FilterPrim* p = &n->prims[i];
            if (p->type != FE_IMAGE || !p->href || p->href[0] != '#') continue;
            Node* t = idmap_get(ids, p->href + 1);
            if (t && t != n) p->ref_node = t;
        }
    }
    for (int i = 0; i < n->nkids; i++) resolve_filter_image_refs(n->kids[i], ids);
}

int svg_resolve(Node* n, IdMap* ids, const svg2img_limits* lim) {
    if (!n) return 0;
    for (int pass = 0; pass < 8 && pattern_hrefs_pending(n); pass++)
        resolve_pattern_hrefs_once(n, ids);
    resolve_gradient_hrefs(n, ids, 64);
    resolve_paint_refs(n, ids);
    resolve_text_paths(n, ids);
    resolve_filter_image_refs(n, ids);
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
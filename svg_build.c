/* svg_build.c - DOM -> render-tree construction. */
#include "svg_internal.h"
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Tree building                                                       */
/* ------------------------------------------------------------------ */

Node* svg_node_new(int type) {
    Node* n = (Node*)svg_malloc(sizeof(Node));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->opacity = 1.0f;
    n->visible = 1;
    n->fillop = n->strokeop = 1.0f;
    n->miterlimit = 4.0f;
    n->fill.tm = mat_identity();
    n->stroke.tm = mat_identity();
    n->clip_units = UNITS_USER;
    return n;
}

int node_add_child(Node* parent, Node* child) {
    if (!parent || !child) return 0;
    Node** nk = (Node**)svg_realloc(parent->kids,
                                    (size_t)(parent->nkids + 1) * sizeof(Node*));
    if (!nk) return 0;
    parent->kids = nk;
    parent->kids[parent->nkids++] = child;
    return 1;
}

Node* build_node(Ctx* ctx, const XMLNode* xn, const Style* parent, int depth) {
    if (depth > SVG2IMG_MAX_DEPTH) return NULL;
    /* Hard depth limit: refuse to build past max_depth.  The caller drops the
     * subtree and the conversion reports SVG2IMG_ERR_LIMIT. */
    if (ctx->lim && depth > ctx->lim->max_depth) { ctx->limit_hit = 1; return NULL; }
    /* Node budget: refuse to build past max_nodes. */
    if (ctx->lim && ctx->nodes >= ctx->lim->max_nodes) { ctx->limit_hit = 1; return NULL; }
    ctx->nodes++;
    Style st = style_default();
    if (parent) style_copy(&st, parent);
    ctx->em = st.fontsize;

    /* Cascades: presentation attributes, then CSS rules, then the inline
 * style attribute (later rules win). */
    for (int i = 0; i < xn->nattrs; i++) {
        char decl[512];
        snprintf(decl, sizeof(decl), "%s:%s", xn->attrs[i * 2], xn->attrs[i * 2 + 1]);
        style_apply_decls(ctx, &st, decl);
    }
    style_apply_decls(ctx, &st, xml_attr(xn, "style"));

    if (st.fill.type == PAINT_CURRENT_COLOR) {
        st.fill.type = PAINT_COLOR;
        st.fill.color = st.color;
    }
    if (st.stroke.type == PAINT_CURRENT_COLOR) {
        st.stroke.type = PAINT_COLOR;
        st.stroke.color = st.color;
    }

    const char* name = xn->name;
    Node* n = NULL;
    int in_defs = 0;

    if (!strcmp(name, "svg") || !strcmp(name, "g") || !strcmp(name, "a")) {
        n = svg_node_new(NODE_GROUP);
        if (n && !strcmp(name, "svg")) {
            /* A nested <svg> establishes a new viewport: x/y/width/height
             * position and size it, and viewBox maps its own user space into
             * that viewport (SVG 1.1 §5.1.2).  Only the *root* svg was
             * handled before; here a nested one gets the same treatment by
             * folding the viewport transform into the node's matrix and
             * clipping to the viewport rectangle. */
            float vx = len_attr(ctx, xn, "x", 0, 0.0f);
            float vy = len_attr(ctx, xn, "y", 1, 0.0f);
            float vw = len_attr(ctx, xn, "width", 0, 0.0f);
            float vh = len_attr(ctx, xn, "height", 1, 0.0f);
            if (!(vw > 0.0f)) vw = ctx->vw;
            if (!(vh > 0.0f)) vh = ctx->vh;
            Mat m = mat_translate(vx, vy);
            float vbx = 0, vby = 0, vbw = 0, vbh = 0;
            if (svg_parse_viewbox(xml_attr(xn, "viewBox"),
                                  &vbx, &vby, &vbw, &vbh) &&
                vbw > 0.0f && vbh > 0.0f) {
                /* preserveAspectRatio: only the "none" (stretch) case is
                 * honoured exactly; the default uniform fit is computed here
                 * and matches the root viewport code. */
                float sx = vw / vbw, sy = vh / vbh;
                int none = 0, slice = 0, xa = 1, ya = 1;
                const char* par = xml_attr(xn, "preserveAspectRatio");
                if (par && strncmp(par, "none", 4) == 0) none = 1;
                else if (par) {
                    if (strstr(par, "xMin")) xa = 0;
                    else if (strstr(par, "xMax")) xa = 2;
                    if (strstr(par, "YMin")) ya = 0;
                    else if (strstr(par, "YMax")) ya = 2;
                    if (strstr(par, "slice")) slice = 1;
                }
                if (none) {
                    m = mat_mul(m, mat_mul(mat_scale(sx, sy),
                                           mat_translate(-vbx, -vby)));
                } else {
                    float s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
                    float ex = vw - vbw * s, ey = vh - vbh * s;
                    float ox = (xa == 1) ? ex * 0.5f : (xa == 2 ? ex : 0.0f);
                    float oy = (ya == 1) ? ey * 0.5f : (ya == 2 ? ey : 0.0f);
                    m = mat_mul(m, mat_mul(mat_translate(ox, oy),
                                mat_mul(mat_scale(s, s),
                                        mat_translate(-vbx, -vby))));
                }
            }
            /* The node's own transform attribute is applied before the
             * viewport transform, per the SVG transform semantics; stash the
             * matrix and combine it once the transform attribute is parsed
             * below. */
            n->vp_matrix = m;
            n->vp_matrix_valid = 1;
            /* Clip to the viewport rectangle (overflow is hidden by default
             * on the outermost svg and on nested viewports).  The rectangle
             * is expressed in the node's own user space -- the space `w`
             * maps to device -- which starts at (0,0) and is vw x vh. */
            n->vp_clip = 1;
            n->vp_x = 0.0f; n->vp_y = 0.0f; n->vp_w = vw; n->vp_h = vh;
        }
    } else if (!strcmp(name, "symbol")) {
        /* <symbol> is never rendered directly: it is only instantiated by a
         * <use> reference, which clones it like any other subtree.  Building
         * it as a group but tagging it NODE_SYMBOL keeps the definition out
         * of the render tree while still allowing clone_node() to copy it.
         * A symbol may establish a viewport via viewBox + width/height on
         * the use site, so remember its viewBox here. */
        n = svg_node_new(NODE_SYMBOL);
        if (!n) goto oom;
        if (svg_parse_viewbox(xml_attr(xn, "viewBox"),
                              &n->symbol_vb[0], &n->symbol_vb[1],
                              &n->symbol_vb[2], &n->symbol_vb[3]) &&
            n->symbol_vb[2] > 0.0f && n->symbol_vb[3] > 0.0f)
            n->symbol_vb_valid = 1;
        n->symbol_par_xa = n->symbol_par_ya = 1;
        const char* spar = xml_attr(xn, "preserveAspectRatio");
        if (spar) {
            if (strncmp(spar, "none", 4) == 0) n->symbol_par_none = 1;
            else {
                if (strstr(spar, "xMin")) n->symbol_par_xa = 0;
                else if (strstr(spar, "xMax")) n->symbol_par_xa = 2;
                if (strstr(spar, "YMin")) n->symbol_par_ya = 0;
                else if (strstr(spar, "YMax")) n->symbol_par_ya = 2;
                if (strstr(spar, "slice")) n->symbol_par_slice = 1;
            }
        }
    } else if (!strcmp(name, "defs")) {
        /* Definitions are not rendered directly, but their ids must be
         * registered so <use> can clone them.  Build them into an IGNORE
         * container: raster_node skips IGNORE painting while clone_node can
         * still reach the definition through the id map. */
        n = svg_node_new(NODE_IGNORE);
        if (!n) goto oom;
        in_defs = 1;
    } else if (!strcmp(name, "rect")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        float x = len_attr(ctx, xn, "x", 0, 0.0f);
        float y = len_attr(ctx, xn, "y", 1, 0.0f);
        float w = len_attr(ctx, xn, "width", 0, 0.0f);
        float h = len_attr(ctx, xn, "height", 1, 0.0f);
        float rx = len_attr(ctx, xn, "rx", 0, 0.0f);
        float ry = len_attr(ctx, xn, "ry", 1, 0.0f);
        if (rx == 0.0f) rx = ry;
        if (ry == 0.0f) ry = rx;
        n->path = path_from_rect(x, y, w, h, rx, ry);
    } else if (!strcmp(name, "circle")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        float cx = len_attr(ctx, xn, "cx", 0, 0.0f);
        float cy = len_attr(ctx, xn, "cy", 1, 0.0f);
        float r  = len_attr(ctx, xn, "r",  2, 0.0f);
        n->path = path_from_ellipse(cx, cy, r, r);
    } else if (!strcmp(name, "ellipse")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        float cx = len_attr(ctx, xn, "cx", 0, 0.0f);
        float cy = len_attr(ctx, xn, "cy", 1, 0.0f);
        float rx = len_attr(ctx, xn, "rx", 0, 0.0f);
        float ry = len_attr(ctx, xn, "ry", 1, 0.0f);
        n->path = path_from_ellipse(cx, cy, rx, ry);
    } else if (!strcmp(name, "line")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        n->path = path_from_line(len_attr(ctx, xn, "x1", 0, 0.0f),
                                 len_attr(ctx, xn, "y1", 1, 0.0f),
                                 len_attr(ctx, xn, "x2", 0, 0.0f),
                                 len_attr(ctx, xn, "y2", 1, 0.0f));
    } else if (!strcmp(name, "polyline")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        n->path = path_from_points_lim(xml_attr(xn, "points"), 0, ctx->lim,
                                       &ctx->limit_hit);
    } else if (!strcmp(name, "polygon")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        n->path = path_from_points_lim(xml_attr(xn, "points"), 1, ctx->lim,
                                       &ctx->limit_hit);
    } else if (!strcmp(name, "path")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        const char* d = xml_attr(xn, "d");
        n->path = path_parse_lim(d ? d : "", ctx->lim, &ctx->limit_hit);
    } else if (!strcmp(name, "text")) {
        n = svg_node_new(NODE_TEXT);
        if (!n) goto oom;
        n->x = len_attr(ctx, xn, "x", 0, 0.0f);
        n->y = len_attr(ctx, xn, "y", 1, 0.0f);
        n->text = gather_text(xn, ctx->lim ? ctx->lim->max_text_length
                                           : SVG2IMG_LIMIT_MAX_TEXT_LENGTH,
                              &ctx->limit_hit);
        n->fontsize = st.fontsize;
        n->bold = st.bold;
        n->italic = st.italic;
        n->anchor = st.anchor;
    } else if (!strcmp(name, "linearGradient")) {
        n = build_gradient(ctx, xn, 0);
        if (!n) goto oom;
    } else if (!strcmp(name, "radialGradient")) {
        n = build_gradient(ctx, xn, 1);
        if (!n) goto oom;
    } else if (!strcmp(name, "clipPath")) {
        n = svg_node_new(NODE_CLIPDEF);
        if (!n) goto oom;
        const char* cu = xml_attr(xn, "clipPathUnits");
        n->clip_units = (cu && !strcmp(cu, "objectBoundingBox")) ? UNITS_BBOX : UNITS_USER;
        n->clip_rule = -1;
        const char* cr = xml_attr(xn, "clip-rule");
        if (cr) n->clip_rule = !strcmp(cr, "evenodd") ? 1 : 0;
    } else if (!strcmp(name, "use")) {
        n = svg_node_new(NODE_GROUP);
        if (!n) goto oom;
        const char* href = xml_attr(xn, "href");
        if (!href) href = xml_attr(xn, "xlink:href");
        if (href) {
            const char* h = href;
            if (*h == '#') h++;
            n->use_ref = svg_strdup(h);
        }
        n->use_x = len_attr(ctx, xn, "x", 0, 0.0f);
        n->use_y = len_attr(ctx, xn, "y", 1, 0.0f);
        n->use_w = len_attr(ctx, xn, "width", 0, 0.0f);
        n->use_h = len_attr(ctx, xn, "height", 1, 0.0f);
        /* The use element's own computed style is the inherited-value source
         * for the shadow tree, so keep the style values on the node. */
        n->fontsize = st.fontsize;
        n->bold = st.bold;
        n->italic = st.italic;
        n->anchor = st.anchor;
        n->letterspacing = st.letterspacing;
    } else {
        /* Unsupported element.  Policy (see docs/limitations.md): unknown
         * elements are ignored, but they are treated as transparent
         * containers, so a known child (e.g. <rect> inside <foo>) still
         * renders.  Only elements that *define* content for others (defs,
         * clipPath, gradients) are opaque.  <switch>, <tref>, animation and
         * filter primitives therefore cost nothing and change nothing. */
        n = svg_node_new(NODE_TRANSPARENT);
        if (!n) goto oom;
    }
    if (!n) goto oom;

oom:
    if (!n) { style_free(&st); return NULL; }

    /* Take ownership of the style's heap-owned fields. */
    if (n->type != NODE_GRADDEF) {
        n->fill = st.fill;
        n->stroke = st.stroke;
        n->dash = st.dash;
        n->ndash = st.ndash;
        st.fill.stops = NULL; st.fill.nstops = 0;
        st.fill.ref = NULL;
        st.stroke.stops = NULL; st.stroke.nstops = 0;
        st.stroke.ref = NULL;
        st.dash = NULL; st.ndash = 0;
    }

    /* Record which paint properties this element specified itself, so a
     * <use> can supply the ones it did not (shadow-tree inheritance). */
    if (st.saw_fill)   n->spec |= SPEC_FILL;
    if (st.saw_stroke) n->spec |= SPEC_STROKE;
    /* Text/visibility properties are inherited too, and a <use> may supply
     * them when the referenced element did not specify them.  Only the
     * properties this element actually specified are recorded; the values
     * themselves are already in the node. */
    n->spec |= st.spec;

    n->sw = st.stroke_width;
    n->linecap = st.linecap;
    n->linejoin = st.linejoin;
    n->miterlimit = st.miterlimit;
    n->dashoff = st.dashoff;
    n->fillrule = st.fillrule;
    n->clip_rule = st.cliprule;
    n->fillop = st.fillop;
    n->strokeop = st.strokeop;
    n->letterspacing = st.letterspacing;

    char style_val[256];
    const char* op = xml_attr(xn, "opacity");
    if (style_get_decl(xml_attr(xn, "style"), "opacity", style_val, sizeof style_val))
        op = style_val;
    if (op) {
        float v = strtof(op, NULL);
        n->opacity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    /* visibility:hidden suppresses this element's own painting but is
     * inherited per element; a descendant that sets visibility:visible must
     * still draw, so this is not a subtree flag.  display:none, by contrast,
     * stops the traversal entirely. */
    n->visible = !st.visibility_hidden;
    n->hidden_subtree = st.display_none;
    n->tm = transform_attr(xn);
    if (n->vp_matrix_valid) {
        /* A nested <svg> built its viewport matrix above; the transform
         * attribute (if any) is applied first, then the viewport mapping. */
        n->tm = mat_mul(n->tm, n->vp_matrix);
        n->vp_matrix_valid = 0;
    }

    const char* id = xml_attr(xn, "id");
    if (id) {
        n->id = svg_strdup(id);
        idmap_put(ctx->ids, id, n);
    }
    const char* clip = xml_attr(xn, "clip-path");
    if (style_get_decl(xml_attr(xn, "style"), "clip-path", style_val, sizeof style_val))
        clip = style_val;
    if (clip && strncmp(clip, "url(#", 5) == 0) {
        const char* e = strchr(clip + 5, ')');
        if (e) n->clip_ref = svg_strndup(clip + 5, (size_t)(e - (clip + 5)));
    }

    if (n->type == NODE_GRADDEF) {
        if (parse_stops(xn, &n->fill,
                        ctx->lim ? ctx->lim->max_gradient_stops
                                 : SVG2IMG_LIMIT_MAX_GRADIENT_STOPS))
            ctx->limit_hit = 1;
        style_free(&st);
        return n;
    }

    for (int i = 0; i < xn->nkids; i++) {
        Node* c = build_node(ctx, xn->kids[i], &st, depth + 1);
        /* A child that could not be attached is discarded; unregister its
         * ids first or the map keeps a dangling pointer. */
        if (c && !node_add_child(n, c)) svg_free_detached(c, ctx->ids);
    }

    style_free(&st);
    (void)in_defs;
    return n;
}

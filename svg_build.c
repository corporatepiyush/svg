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

/* ------------------------------------------------------------------ */
/* <switch> conditions (SVG 1.1 §5.7 and Appendix O)                   */
/* ------------------------------------------------------------------ */

/* Feature strings whose fragment we actually implement.  A <switch> child
 * with requiredFeatures naming anything else is not selected. */
static int feature_supported(const char* uri) {
    static const char* const frag[] = {
        "SVG", "SVG-static", "Core", "BasicStructure", "Structure",
        "Shape", "BasicShape", "Text", "BasicText", "Gradient", "Pattern",
        "Clip", "BasicClip", "BasicPaint", "Color", "Linking",
        "Image", "Marker", "Mask", "Style", "CSS", NULL
    };
    if (!uri) return 1;
    if (!strcmp(uri, "org.w3c.svg") || !strcmp(uri, "org.w3c.svg.static"))
        return 1;
    const char* h = strrchr(uri, '#');
    const char* name = h ? h + 1 : uri;
    for (int i = 0; frag[i]; i++)
        if (!strcmp(name, frag[i])) return 1;
    return 0;
}

/* The user's language for systemLanguage matching.  A rasteriser has no
 * locale, so English is assumed; a tag matches when it is a prefix of the
 * user language on a language-range boundary. */
static int language_matches(const char* list) {
    const char* user = "en";
    size_t ul = strlen(user);
    const char* p = list;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        const char* e = p;
        while (*e && *e != ',') e++;
        const char* te = e;
        while (te > p && isspace((unsigned char)te[-1])) te--;
        size_t n = (size_t)(te - p);
        if (n >= ul && !strncmp(p, user, ul) && (n == ul || p[ul] == '-'))
            return 1;
        p = e;
    }
    return 0;
}

static int switch_child_ok(const XMLNode* xn) {
    const char* rf = xml_attr(xn, "requiredFeatures");
    if (rf && *rf) {
        const char* p = rf;
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            const char* e = p;
            while (*e && !isspace((unsigned char)*e)) e++;
            if (e > p) {
                char buf[256];
                size_t n = (size_t)(e - p);
                if (n >= sizeof buf) n = sizeof buf - 1;
                memcpy(buf, p, n);
                buf[n] = 0;
                if (!feature_supported(buf)) return 0;
            }
            p = e;
        }
    }
    const char* re = xml_attr(xn, "requiredExtensions");
    if (re) {
        const char* q = re;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q) return 0;              /* we implement no extensions */
    }
    const char* sl = xml_attr(xn, "systemLanguage");
    if (sl && *sl && !language_matches(sl)) return 0;
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
    /* Author stylesheet rules come before the inline style (which is a
     * later, more specific origin), except for !important declarations. */
    css_apply(ctx, xn, &st);
    style_apply_decls(ctx, &st, xml_attr(xn, "style"));
    css_apply_important(ctx, xn, &st);

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
    const XMLNode* switch_pick = NULL;
    float path_scale = 1.0f;

    if (!strcmp(name, "switch")) {
        /* Render only the first child whose conditions hold; the rest are
         * not part of the rendered tree (SVG 1.1 §5.7). */
        n = svg_node_new(NODE_GROUP);
        if (!n) goto oom;
        for (int i = 0; i < xn->nkids; i++)
            if (switch_child_ok(xn->kids[i])) { switch_pick = xn->kids[i]; break; }
    } else if (!strcmp(name, "svg") || !strcmp(name, "g") || !strcmp(name, "a")) {
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
            /* overflow: the initial value is hidden, so a nested viewport
             * clips unless it explicitly asks for `visible` (SVG 1.1 §14.3).
             * `overflow` is not inherited, so it is read on this element
             * only (presentation attribute or inline style). */
            const char* ov = xml_attr(xn, "overflow");
            char ovbuf[64];
            if (style_get_decl(xml_attr(xn, "style"), "overflow",
                               ovbuf, sizeof ovbuf))
                ov = ovbuf;
            n->vp_clip = !(ov && !strcmp(ov, "visible"));
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
        n->marker_ok = 1;
        n->path = path_from_line(len_attr(ctx, xn, "x1", 0, 0.0f),
                                 len_attr(ctx, xn, "y1", 1, 0.0f),
                                 len_attr(ctx, xn, "x2", 0, 0.0f),
                                 len_attr(ctx, xn, "y2", 1, 0.0f));
    } else if (!strcmp(name, "polyline")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        n->marker_ok = 1;
        n->path = path_from_points_lim(xml_attr(xn, "points"), 0, ctx->lim,
                                       &ctx->limit_hit);
    } else if (!strcmp(name, "polygon")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        n->marker_ok = 1;
        n->path = path_from_points_lim(xml_attr(xn, "points"), 1, ctx->lim,
                                       &ctx->limit_hit);
    } else if (!strcmp(name, "path")) {
        n = svg_node_new(NODE_SHAPE);
        if (!n) goto oom;
        n->marker_ok = 1;
        const char* d = xml_attr(xn, "d");
        n->path = path_parse_lim(d ? d : "", ctx->lim, &ctx->limit_hit);
        /* pathLength calibrates the author's coordinate system for dash
         * patterns and textPath distances: dash values are multiplied by
         * (actual length / declared length) (SVG 1.1 §8.3). */
        const char* pl = xml_attr(xn, "pathLength");
        if (pl && n->path) {
            float declared = strtof(pl, NULL);
            float actual = path_length(n->path);
            if (declared > 0.0f && actual > 0.0f) path_scale = actual / declared;
        }
    } else if (!strcmp(name, "text")) {
        n = svg_node_new(NODE_TEXT);
        if (!n) goto oom;
        n->x = len_attr(ctx, xn, "x", 0, 0.0f);
        n->y = len_attr(ctx, xn, "y", 1, 0.0f);
        n->text = gather_text(xn, ctx->lim ? ctx->lim->max_text_length
                                           : SVG2IMG_LIMIT_MAX_TEXT_LENGTH,
                              &ctx->limit_hit);
        gather_chunks(xn, ctx, n, &ctx->limit_hit);
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
    } else if (!strcmp(name, "pattern")) {
        n = build_pattern(ctx, xn);
    } else if (!strcmp(name, "mask")) {
        n = build_mask(ctx, xn);
    } else if (!strcmp(name, "marker")) {
        n = build_marker(ctx, xn);
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
    if (n->type != NODE_MARKERDEF) {
        n->marker_start_ref = st.marker_start; st.marker_start = NULL;
        n->marker_mid_ref   = st.marker_mid;   st.marker_mid = NULL;
        n->marker_end_ref   = st.marker_end;   st.marker_end = NULL;
    }
    if (path_scale != 1.0f && n->dash) {
        for (int i = 0; i < n->ndash; i++) n->dash[i] *= path_scale;
        n->dashoff *= path_scale;
    }

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
    const char* mk = xml_attr(xn, "mask");
    if (style_get_decl(xml_attr(xn, "style"), "mask", style_val, sizeof style_val))
        mk = style_val;
    if (mk && strncmp(mk, "url(#", 5) == 0) {
        const char* e = strchr(mk + 5, ')');
        if (e) n->mask_ref = svg_strndup(mk + 5, (size_t)(e - (mk + 5)));
    }

    if (n->type == NODE_GRADDEF) {
        if (parse_stops(xn, &n->fill,
                        ctx->lim ? ctx->lim->max_gradient_stops
                                 : SVG2IMG_LIMIT_MAX_GRADIENT_STOPS))
            ctx->limit_hit = 1;
        style_free(&st);
        return n;
    }

    if (switch_pick) {
        Node* c = build_node(ctx, switch_pick, &st, depth + 1);
        if (c && !node_add_child(n, c)) svg_free_detached(c, ctx->ids);
    } else {
        for (int i = 0; i < xn->nkids; i++) {
            Node* c = build_node(ctx, xn->kids[i], &st, depth + 1);
            /* A child that could not be attached is discarded; unregister its
             * ids first or the map keeps a dangling pointer. */
            if (c && !node_add_child(n, c)) svg_free_detached(c, ctx->ids);
        }
    }

    style_free(&st);
    (void)in_defs;
    return n;
}

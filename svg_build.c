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

    if (!strcmp(name, "svg") || !strcmp(name, "g") || !strcmp(name, "a") ||
        !strcmp(name, "symbol")) {
        n = svg_node_new(NODE_GROUP);
    } else if (!strcmp(name, "defs")) {
        n = svg_node_new(NODE_IGNORE);
        in_defs = 1;
    } else if (!strcmp(name, "rect")) {
        n = svg_node_new(NODE_SHAPE);
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
        float cx = len_attr(ctx, xn, "cx", 0, 0.0f);
        float cy = len_attr(ctx, xn, "cy", 1, 0.0f);
        float r  = len_attr(ctx, xn, "r",  2, 0.0f);
        n->path = path_from_ellipse(cx, cy, r, r);
    } else if (!strcmp(name, "ellipse")) {
        n = svg_node_new(NODE_SHAPE);
        float cx = len_attr(ctx, xn, "cx", 0, 0.0f);
        float cy = len_attr(ctx, xn, "cy", 1, 0.0f);
        float rx = len_attr(ctx, xn, "rx", 0, 0.0f);
        float ry = len_attr(ctx, xn, "ry", 1, 0.0f);
        n->path = path_from_ellipse(cx, cy, rx, ry);
    } else if (!strcmp(name, "line")) {
        n = svg_node_new(NODE_SHAPE);
        n->path = path_from_line(len_attr(ctx, xn, "x1", 0, 0.0f),
                                 len_attr(ctx, xn, "y1", 1, 0.0f),
                                 len_attr(ctx, xn, "x2", 0, 0.0f),
                                 len_attr(ctx, xn, "y2", 1, 0.0f));
    } else if (!strcmp(name, "polyline")) {
        n = svg_node_new(NODE_SHAPE);
        n->path = path_from_points(xml_attr(xn, "points"), 0);
    } else if (!strcmp(name, "polygon")) {
        n = svg_node_new(NODE_SHAPE);
        n->path = path_from_points(xml_attr(xn, "points"), 1);
    } else if (!strcmp(name, "path")) {
        n = svg_node_new(NODE_SHAPE);
        const char* d = xml_attr(xn, "d");
        n->path = path_parse(d ? d : "");
    } else if (!strcmp(name, "text")) {
        n = svg_node_new(NODE_TEXT);
        n->x = len_attr(ctx, xn, "x", 0, 0.0f);
        n->y = len_attr(ctx, xn, "y", 1, 0.0f);
        n->text = gather_text(xn);
        n->fontsize = st.fontsize;
        n->bold = st.bold;
        n->italic = st.italic;
        n->anchor = st.anchor;
    } else if (!strcmp(name, "linearGradient")) {
        n = build_gradient(ctx, xn, 0);
    } else if (!strcmp(name, "radialGradient")) {
        n = build_gradient(ctx, xn, 1);
    } else if (!strcmp(name, "clipPath")) {
        n = svg_node_new(NODE_CLIPDEF);
        const char* cu = xml_attr(xn, "clipPathUnits");
        n->clip_units = (cu && !strcmp(cu, "objectBoundingBox")) ? UNITS_BBOX : UNITS_USER;
        n->clip_rule = -1;
        const char* cr = xml_attr(xn, "clip-rule");
        if (cr) n->clip_rule = !strcmp(cr, "evenodd") ? 1 : 0;
    } else if (!strcmp(name, "use")) {
        n = svg_node_new(NODE_GROUP);
        const char* href = xml_attr(xn, "href");
        if (!href) href = xml_attr(xn, "xlink:href");
        if (href) {
            const char* h = href;
            if (*h == '#') h++;
            n->use_ref = svg_strdup(h);
        }
        n->use_x = len_attr(ctx, xn, "x", 0, 0.0f);
        n->use_y = len_attr(ctx, xn, "y", 1, 0.0f);
    } else {
        n = svg_node_new(NODE_IGNORE);
    }
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

    n->sw = st.stroke_width;
    n->linecap = st.linecap;
    n->linejoin = st.linejoin;
    n->miterlimit = st.miterlimit;
    n->dashoff = st.dashoff;
    n->fillrule = st.fillrule;
    n->clip_rule = st.cliprule;
    n->fillop = st.fillop;
    n->strokeop = st.strokeop;

    char style_val[256];
    const char* op = xml_attr(xn, "opacity");
    if (style_get_decl(xml_attr(xn, "style"), "opacity", style_val, sizeof style_val))
        op = style_val;
    if (op) {
        float v = strtof(op, NULL);
        n->opacity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    n->visible = !(st.display_none || st.visibility_hidden);
    n->tm = transform_attr(xn);

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
        parse_stops(xn, &n->fill);
        style_free(&st);
        return n;
    }

    for (int i = 0; i < xn->nkids; i++) {
        Node* c = build_node(ctx, xn->kids[i], &st, depth + 1);
        if (c && !node_add_child(n, c)) svg_free(c);
    }

    style_free(&st);
    (void)in_defs;
    return n;
}

/* svg_gradient.c - Gradient definitions and stop lists. */
#include "svg_internal.h"
#include <ctype.h>

static float grad_len(Ctx* ctx, const XMLNode* xn, const char* name, int axis,
                      int units, float def) {
    const char* s = xml_attr(xn, name);
    if (!s) return def;
    while (isspace((unsigned char)*s)) s++;
    char* end = NULL;
    float v = strtof(s, &end);
    if (end == s) return def;
    while (isspace((unsigned char)*end)) end++;
    if (*end == '%') {
        float f = v / 100.0f;
        if (units == UNITS_USER) {
            float ref = (axis == 0) ? ctx->vw : (axis == 1) ? ctx->vh
                                                            : (ctx->vw + ctx->vh) * 0.5f;
            return f * ref;
        }
        return f;
    }
    return v;
}

int parse_stops(const XMLNode* xn, Paint* p, int max_stops) {
    if (max_stops <= 0) max_stops = SVG2IMG_LIMIT_MAX_GRADIENT_STOPS;
    int truncated = 0;
    int cap = 0;
    for (int i = 0; i < xn->nkids; i++) {
        const XMLNode* c = xn->kids[i];
        if (strcmp(c->name, "stop")) continue;
        if (p->nstops >= max_stops) { truncated = 1; break; }

        float off = 0.0f;
        const char* so = xml_attr(c, "offset");
        if (so) {
            off = strtof(so, NULL);
            if (strchr(so, '%')) off /= 100.0f;
        }
        Color col = {0, 0, 0, 1};
        /* stop-color / stop-opacity are inherited from the gradient element
         * (SVG 1.1 §13.2), so a stop with no colour of its own takes the
         * gradient's colour.  Build the gradient's effective colour from its
         * own presentation attributes and inline style, then let the stop's
         * own attributes override it. */
        Color inherited = {0, 0, 0, 1};
        const char* gsc = xml_attr(xn, "stop-color");
        if (!gsc) gsc = xml_attr(xn, "color");   /* `color` feeds currentColor */
        if (gsc) color_parse(gsc, &inherited);
        const char* gsop = xml_attr(xn, "stop-opacity");
        if (gsop) inherited.a = strtof(gsop, NULL);
        char gsval[128];
        const char* gsdecl = xml_attr(xn, "style");
        if (style_get_decl(gsdecl, "stop-color", gsval, sizeof gsval)) {
            Color gc;
            if (color_parse(gsval, &gc)) inherited = gc;
        }
        if (style_get_decl(gsdecl, "stop-opacity", gsval, sizeof gsval))
            inherited.a = strtof(gsval, NULL);
        col = inherited;
        const char* sc = xml_attr(c, "stop-color");
        if (sc) color_parse(sc, &col);
        const char* sop = xml_attr(c, "stop-opacity");
        if (sop) col.a = strtof(sop, NULL);

        /* Inline style overrides the presentation attributes. */
        char sval[128];
        const char* sdecl = xml_attr(c, "style");
        if (style_get_decl(sdecl, "stop-color", sval, sizeof sval)) {
            Color cc;
            if (color_parse(sval, &cc)) col = cc;
        }
        if (style_get_decl(sdecl, "stop-opacity", sval, sizeof sval))
            col.a = strtof(sval, NULL);

        if (p->nstops == cap) {
            int ncap = cap ? cap * 2 : 4;
            if (ncap > max_stops) ncap = max_stops;
            Stop* ns = (Stop*)svg_realloc(p->stops, (size_t)ncap * sizeof(Stop));
            if (!ns) { p->nstops = 0; return 1; }
            p->stops = ns;
            cap = ncap;
        }
        p->stops[p->nstops].off = off;
        p->stops[p->nstops].c = col;
        p->nstops++;
    }

    /* Normalise offsets the way SVG requires: clamp to [0,1], then make each
     * offset at least as large as the previous one.  The stops are *not*
     * sorted -- document order is significant, so a later stop with a smaller
     * offset is clamped up to the running maximum (this is what WebKit does). */
    for (int i = 0; i < p->nstops; i++) {
        float off = p->stops[i].off;
        if (!(off > 0.0f)) off = 0.0f;        /* NaN, negative */
        if (off > 1.0f) off = 1.0f;
        if (i > 0 && off < p->stops[i - 1].off) off = p->stops[i - 1].off;
        p->stops[i].off = off;
    }
    return truncated;
}

Node* build_gradient(Ctx* ctx, const XMLNode* xn, int radial) {
    Node* n = svg_node_new(NODE_GRADDEF);
    if (!n) return NULL;
    Paint* p = &n->fill;

    p->type = radial ? PAINT_RADIAL : PAINT_LINEAR;
    p->units = UNITS_BBOX;
    /* color-interpolation is a property of the gradient element; the default
     * is sRGB, and linearRGB interpolates in linear light. */
    const char* ci = xml_attr(xn, "color-interpolation");
    char cival[64];
    if (style_get_decl(xml_attr(xn, "style"), "color-interpolation", cival,
                       sizeof cival))
        ci = cival;
    p->linear = (ci && !strcmp(ci, "linearRGB"));
    const char* gu = xml_attr(xn, "gradientUnits");
    if (gu && !strcmp(gu, "userSpaceOnUse")) p->units = UNITS_USER;

    p->spread = SPREAD_PAD;
    const char* sp = xml_attr(xn, "spreadMethod");
    if (sp) {
        if (!strcmp(sp, "reflect")) p->spread = SPREAD_REFLECT;
        else if (!strcmp(sp, "repeat")) p->spread = SPREAD_REPEAT;
    }

    if (radial) {
        p->cx = grad_len(ctx, xn, "cx", 0, p->units, 0.5f);
        p->cy = grad_len(ctx, xn, "cy", 1, p->units, 0.5f);
        p->r  = grad_len(ctx, xn, "r",  2, p->units, 0.5f);
        p->fx = grad_len(ctx, xn, "fx", 0, p->units, p->cx);
        p->fy = grad_len(ctx, xn, "fy", 1, p->units, p->cy);
    } else {
        p->x1 = grad_len(ctx, xn, "x1", 0, p->units, 0.0f);
        p->y1 = grad_len(ctx, xn, "y1", 1, p->units, 0.0f);
        p->x2 = grad_len(ctx, xn, "x2", 0, p->units, 1.0f);
        p->y2 = grad_len(ctx, xn, "y2", 1, p->units, 0.0f);
    }

    p->tm = mat_identity();
    const char* gt = xml_attr(xn, "gradientTransform");
    if (gt) p->tm = parse_transform(gt);

    const char* href = xml_attr(xn, "href");
    if (!href) href = xml_attr(xn, "xlink:href");
    if (href) {
        const char* h = href;
        if (*h == '#') h++;
        p->ref = svg_strdup(h);
    }
    return n;
}

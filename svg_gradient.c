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

void parse_stops(const XMLNode* xn, Paint* p) {
    int cap = 0;
    for (int i = 0; i < xn->nkids; i++) {
        const XMLNode* c = xn->kids[i];
        if (strcmp(c->name, "stop")) continue;

        float off = 0.0f;
        const char* so = xml_attr(c, "offset");
        if (so) {
            off = strtof(so, NULL);
            if (strchr(so, '%')) off /= 100.0f;
        }
        Color col = {0, 0, 0, 1};
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
            cap = cap ? cap * 2 : 4;
            p->stops = (Stop*)svg_realloc(p->stops, (size_t)cap * sizeof(Stop));
            if (!p->stops) { p->nstops = 0; return; }
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
}

Node* build_gradient(Ctx* ctx, const XMLNode* xn, int radial) {
    Node* n = svg_node_new(NODE_GRADDEF);
    if (!n) return NULL;
    Paint* p = &n->fill;

    p->type = radial ? PAINT_RADIAL : PAINT_LINEAR;
    p->units = UNITS_BBOX;
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

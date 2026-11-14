/* svg_pattern.c - <pattern> tile definitions (SVG 1.1 §13.3). */
#include "svg_internal.h"
#include <ctype.h>

static int par_align(const char* s) {
    if (!s) return 1;
    if (strstr(s, "none")) return 1;
    if (strstr(s, "xMin")) return 0;
    if (strstr(s, "xMax")) return 2;
    return 1;
}

static int par_valign(const char* s) {
    if (!s) return 1;
    if (strstr(s, "YMin")) return 0;
    if (strstr(s, "YMax")) return 2;
    return 1;
}

Node* build_pattern(Ctx* ctx, const XMLNode* xn) {
    Node* n = svg_node_new(NODE_PATTERNDEF);
    if (!n) return NULL;

    /* patternUnits defaults to objectBoundingBox; patternContentUnits to
     * userSpaceOnUse (SVG 1.1 §13.3.1). */
    n->pat_units = UNITS_BBOX;
    const char* pu = xml_attr(xn, "patternUnits");
    if (pu && !strcmp(pu, "userSpaceOnUse")) n->pat_units = UNITS_USER;

    n->pat_content_units = UNITS_USER;
    const char* pc = xml_attr(xn, "patternContentUnits");
    if (pc && !strcmp(pc, "objectBoundingBox")) n->pat_content_units = UNITS_BBOX;

    n->pat_x = len_attr(ctx, xn, "x", 0, 0.0f);
    n->pat_y = len_attr(ctx, xn, "y", 1, 0.0f);
    n->pat_w = len_attr(ctx, xn, "width", 0, 0.0f);
    n->pat_h = len_attr(ctx, xn, "height", 1, 0.0f);

    const char* pt = xml_attr(xn, "patternTransform");
    n->pat_tm = pt ? parse_transform(pt) : mat_identity();

    if (svg_parse_viewbox(xml_attr(xn, "viewBox"),
                          &n->pat_vb[0], &n->pat_vb[1], &n->pat_vb[2], &n->pat_vb[3]) &&
        n->pat_vb[2] > 0.0f && n->pat_vb[3] > 0.0f)
        n->pat_vb_valid = 1;
    n->pat_par_none = 0;
    n->pat_par_slice = 0;
    n->pat_par_xa = par_align(xml_attr(xn, "preserveAspectRatio"));
    n->pat_par_ya = par_valign(xml_attr(xn, "preserveAspectRatio"));
    const char* par = xml_attr(xn, "preserveAspectRatio");
    if (par && strstr(par, "none")) n->pat_par_none = 1;
    if (par && strstr(par, "slice")) n->pat_par_slice = 1;

    const char* href = xml_attr(xn, "href");
    if (!href) href = xml_attr(xn, "xlink:href");
    if (href) {
        const char* h = href;
        if (*h == '#') h++;
        n->fill.ref = svg_strdup(h);
    }
    return n;
}
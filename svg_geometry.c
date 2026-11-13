/* svg_geometry.c - Attribute lengths and primitive-shape paths. */
#include "svg_internal.h"
#include <ctype.h>

float len_attr(Ctx* ctx, const XMLNode* xn, const char* name,
                      int axis, float def) {
    const char* v = xml_attr(xn, name);
    if (!v) return def;
    return length_parse(v, ctx->vw, ctx->vh, ctx->em, axis, def);
}

Path* path_from_rect(float x, float y, float w, float h, float rx, float ry) {
    if (rx < 0.0f) rx = 0.0f;
    if (ry < 0.0f) ry = 0.0f;
    if (rx > w * 0.5f) rx = w * 0.5f;
    if (ry > h * 0.5f) ry = h * 0.5f;

    char buf[512];
    if (rx > 0.0f && ry > 0.0f) {
        snprintf(buf, sizeof(buf),
                 "M%g,%g H%g A%g,%g 0 0 1 %g,%g V%g A%g,%g 0 0 1 %g,%g H%g "
                 "A%g,%g 0 0 1 %g,%g V%g A%g,%g 0 0 1 %g,%g Z",
                 x + rx, y, x + w - rx, rx, ry, x + w, y + ry,
                 y + h - ry, rx, ry, x + w - rx, y + h, x + rx,
                 rx, ry, x, y + h - ry, y + ry, rx, ry, x + rx, y);
    } else {
        snprintf(buf, sizeof(buf), "M%g,%g H%g V%g H%g Z", x, y, x + w, y + h, x);
    }
    return path_parse(buf);
}

Path* path_from_ellipse(float cx, float cy, float rx, float ry) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) return path_parse("");
    char buf[256];
    snprintf(buf, sizeof(buf),
             "M%g,%g A%g,%g 0 1 0 %g,%g A%g,%g 0 1 0 %g,%g Z",
             cx - rx, cy, rx, ry, cx + rx, cy, rx, ry, cx - rx, cy);
    return path_parse(buf);
}

Path* path_from_line(float x1, float y1, float x2, float y2) {
    char buf[128];
    snprintf(buf, sizeof(buf), "M%g,%g L%g,%g", x1, y1, x2, y2);
    return path_parse(buf);
}

Path* path_from_points(const char* s, int close) {
    Path* p = (Path*)svg_malloc(sizeof(Path));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->n = 1;
    p->cap = 1;
    p->s = (SubPath*)svg_malloc(sizeof(SubPath));
    if (!p->s) { free(p); return NULL; }
    p->s[0].p = NULL;
    p->s[0].n = 0;
    p->s[0].closed = close;

    int cap = 0;
    const char* q = s;
    while (q && *q) {
        while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
        if (!*q) break;
        char* end = NULL;
        float x = strtof(q, &end);
        if (end == q) break;
        q = end;
        while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
        float y = strtof(q, &end);
        if (end == q) break;
        q = end;

        if (p->s[0].n == cap) {
            cap = cap ? cap * 2 : 8;
            p->s[0].p = (Pt*)svg_realloc(p->s[0].p, (size_t)cap * sizeof(Pt));
            if (!p->s[0].p) { p->s[0].n = 0; break; }
        }
        p->s[0].p[p->s[0].n].x = x;
        p->s[0].p[p->s[0].n].y = y;
        p->s[0].n++;
    }
    return p;
}

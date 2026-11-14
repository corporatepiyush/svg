/* svg_style.c - SVG style defaults and inline style="..." declarations.
 *
 * No CSS stylesheets, selectors or cascade: presentation attributes are
 * applied by svg_build.c, then the inline style attribute is applied last.
 */
#include "internal.h"
#include <ctype.h>
#include <strings.h>

Style style_default(void) {
    Style s;
    memset(&s, 0, sizeof(s));
    s.fill.type = PAINT_COLOR;
    s.fill.color = (Color){0, 0, 0, 1};
    s.fill.tm = mat_identity();
    s.stroke.type = PAINT_NONE;
    s.stroke.tm = mat_identity();
    s.stroke_width = 1.0f;
    s.miterlimit = 4.0f;
    s.cliprule = -1;
    s.fillop = s.strokeop = 1.0f;
    s.color = (Color){0, 0, 0, 1};
    s.fontsize = 16.0f;
    return s;
}

void style_copy(Style* dst, const Style* src) {
    *dst = *src;

    /* saw_fill / saw_stroke mean "this element specified it", not "it was
     * inherited", so they never propagate to a child. */
    dst->saw_fill = 0;
    dst->saw_stroke = 0;
    /* The child inherits computed values, not the "specified" bit. */
    dst->spec = 0;

    dst->dash = NULL;
    dst->ndash = 0;
    if (src->dash && src->ndash > 0) {
        dst->dash = (float*)svg_malloc((size_t)src->ndash * sizeof(float));
        if (dst->dash) {
            memcpy(dst->dash, src->dash, (size_t)src->ndash * sizeof(float));
            dst->ndash = src->ndash;
        }
    }

    /* Deep-copy the paint servers so children can override them freely. */
    for (int k = 0; k < 2; k++) {
        Paint* d = (k == 0) ? &dst->fill : &dst->stroke;
        const Paint* s = (k == 0) ? &src->fill : &src->stroke;
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
}

void style_free(Style* s) {
    free(s->dash);
    s->dash = NULL;
    s->ndash = 0;
    for (int k = 0; k < 2; k++) {
        Paint* p = (k == 0) ? &s->fill : &s->stroke;
        free(p->stops); p->stops = NULL; p->nstops = 0;
        free(p->ref);   p->ref = NULL;
    }
}
static void for_each_decl(const char* decls,
                          void (*fn)(void* ud, const char* k, const char* v),
                          void* ud) {
    if (!decls) return;
    char* copy = svg_strdup(decls);
    if (!copy) return;
    char* p = copy;
    while (p && *p) {
        char* semi = strchr(p, ';');
        if (semi) *semi = 0;
        char* colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            char* key = p;
            char* val = colon + 1;
            while (*key && isspace((unsigned char)*key)) key++;
            char* ke = key + strlen(key);
            while (ke > key && isspace((unsigned char)ke[-1])) *--ke = 0;
            while (*val && isspace((unsigned char)*val)) val++;
            char* ve = val + strlen(val);
            while (ve > val && isspace((unsigned char)ve[-1])) *--ve = 0;
            if (*key && *val) fn(ud, key, val);
        }
        p = semi ? semi + 1 : NULL;
    }
    free(copy);
}
struct ApplyCtx {
    Ctx*   ctx;
    Style* st;
};

static void apply_one(void* ud, const char* k, const char* v) {
    struct ApplyCtx* ac = (struct ApplyCtx*)ud;
    Ctx* ctx = ac->ctx;
    Style* st = ac->st;

    if (!strcmp(k, "fill")) {
        st->saw_fill = 1;
        free(st->fill.stops); st->fill.stops = NULL; st->fill.nstops = 0;
        free(st->fill.ref);   st->fill.ref = NULL;
        const char* p = v;
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "url(", 4) == 0) {
            const char* e = strchr(p + 4, ')');
            if (e) {
                char* ref = svg_strndup(p + 4, (size_t)(e - (p + 4)));
                if (ref) {
                    char* t = ref;
                    while (*t && isspace((unsigned char)*t)) t++;
                    char* te = t + strlen(t);
                    while (te > t && isspace((unsigned char)te[-1])) *--te = 0;
                    if (*t == '#') t++;
                    st->fill.type = PAINT_REF;
                    st->fill.ref = svg_strdup(t);
                    free(ref);
                    return;
                }
            }
        }
        if (!strcmp(v, "none")) {
            st->fill.type = PAINT_NONE;
        } else if (!strcasecmp(v, "currentcolor")) {
            st->fill.type = PAINT_CURRENT_COLOR;
        } else {
            Color c;
            if (color_parse(v, &c)) { st->fill.type = PAINT_COLOR; st->fill.color = c; }
            else st->fill.type = PAINT_NONE;
        }
    } else if (!strcmp(k, "stroke")) {
        st->saw_stroke = 1;
        free(st->stroke.stops); st->stroke.stops = NULL; st->stroke.nstops = 0;
        free(st->stroke.ref);   st->stroke.ref = NULL;
        const char* p = v;
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "url(", 4) == 0) {
            const char* e = strchr(p + 4, ')');
            if (e) {
                char* ref = svg_strndup(p + 4, (size_t)(e - (p + 4)));
                if (ref) {
                    char* t = ref;
                    while (*t && isspace((unsigned char)*t)) t++;
                    char* te = t + strlen(t);
                    while (te > t && isspace((unsigned char)te[-1])) *--te = 0;
                    if (*t == '#') t++;
                    st->stroke.type = PAINT_REF;
                    st->stroke.ref = svg_strdup(t);
                    free(ref);
                    return;
                }
            }
        }
        if (!strcmp(v, "none")) {
            st->stroke.type = PAINT_NONE;
        } else if (!strcasecmp(v, "currentcolor")) {
            st->stroke.type = PAINT_CURRENT_COLOR;
        } else {
            Color c;
            if (color_parse(v, &c)) { st->stroke.type = PAINT_COLOR; st->stroke.color = c; }
            else st->stroke.type = PAINT_NONE;
        }
    } else if (!strcmp(k, "stroke-width")) {
        st->stroke_width = length_parse(v, ctx->vw, ctx->vh, ctx->em, 2, 1.0f);
        if (st->stroke_width < 0.0f) st->stroke_width = 0.0f;
    } else if (!strcmp(k, "stroke-linecap")) {
        st->linecap = !strcmp(v, "round") ? 1 : !strcmp(v, "square") ? 2 : 0;
    } else if (!strcmp(k, "stroke-linejoin")) {
        st->linejoin = !strcmp(v, "round") ? 1 : !strcmp(v, "bevel") ? 2 : 0;
    } else if (!strcmp(k, "stroke-miterlimit")) {
        float f = strtof(v, NULL);
        st->miterlimit = f < 1.0f ? 1.0f : f;
    } else if (!strcmp(k, "stroke-dasharray")) {
        free(st->dash);
        st->dash = NULL;
        st->ndash = 0;
        if (strcmp(v, "none")) {
            int cap = 0;
            const char* q = v;
            while (*q) {
                while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
                if (!*q) break;
                char* end = NULL;
                float f = strtof(q, &end);
                if (end == q) break;
                q = end;
                if (st->ndash >= 512) break;   /* absurd dash list: ignore rest */
                if (st->ndash == cap) {
                    cap = cap ? cap * 2 : 4;
                    st->dash = (float*)svg_realloc(st->dash, (size_t)cap * sizeof(float));
                }
                if (st->dash) st->dash[st->ndash++] = f;
            }
        }
    } else if (!strcmp(k, "stroke-dashoffset")) {
        st->dashoff = length_parse(v, ctx->vw, ctx->vh, ctx->em, 2, 0.0f);
    } else if (!strcmp(k, "fill-rule")) {
        st->fillrule = !strcmp(v, "evenodd");
    } else if (!strcmp(k, "clip-rule")) {
        st->cliprule = !strcmp(v, "evenodd") ? 1 : 0;
    } else if (!strcmp(k, "fill-opacity")) {
        float f = strtof(v, NULL);
        st->fillop = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    } else if (!strcmp(k, "stroke-opacity")) {
        float f = strtof(v, NULL);
        st->strokeop = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    } else if (!strcmp(k, "color")) {
        Color c;
        if (color_parse(v, &c)) st->color = c;
    } else if (!strcmp(k, "font-size")) {
        st->spec |= SPEC_FONT_SIZE;
        if (!strcmp(v, "xx-small")) st->fontsize = 8.0f;
        else if (!strcmp(v, "x-small")) st->fontsize = 10.0f;
        else if (!strcmp(v, "small")) st->fontsize = 13.0f;
        else if (!strcmp(v, "medium")) st->fontsize = 16.0f;
        else if (!strcmp(v, "large")) st->fontsize = 18.0f;
        else if (!strcmp(v, "x-large")) st->fontsize = 24.0f;
        else if (!strcmp(v, "xx-large")) st->fontsize = 32.0f;
        else st->fontsize = length_parse(v, ctx->em, ctx->em, ctx->em, 2, 16.0f);
    } else if (!strcmp(k, "font-weight")) {
        st->spec |= SPEC_FONT_WEIGHT;
        if (!strcmp(v, "bold") || !strcmp(v, "bolder")) st->bold = 1;
        else if (!strcmp(v, "normal") || !strcmp(v, "lighter")) st->bold = 0;
        else st->bold = ((int)strtof(v, NULL) >= 600);
    } else if (!strcmp(k, "font-style")) {
        st->spec |= SPEC_FONT_STYLE;
        st->italic = (!strcmp(v, "italic") || !strcmp(v, "oblique"));
    } else if (!strcmp(k, "text-anchor")) {
        st->spec |= SPEC_TEXT_ANCHOR;
        st->anchor = !strcmp(v, "middle") ? 1 : !strcmp(v, "end") ? 2 : 0;
    } else if (!strcmp(k, "letter-spacing")) {
        st->spec |= SPEC_LETTER_SP;
        st->letterspacing = length_parse(v, ctx->vw, ctx->vh, ctx->em, 2, 0.0f);
    } else if (!strcmp(k, "display")) {
        /* `display` is not inherited; the value only matters on the element
         * it is written on.  "none" hides the element and its subtree
         * (raster_node.c stops the traversal), so nothing to reset. */
        if (!strcmp(v, "none")) st->display_none = 1;
    } else if (!strcmp(k, "visibility")) {
        /* `visibility` *is* inherited, but a descendant may override it, so
         * an explicit value must clear the inherited flag in both
         * directions (SVG 1.1 §11.4). */
        st->spec |= SPEC_VISIBILITY;
        st->visibility_hidden = (!strcmp(v, "hidden") || !strcmp(v, "collapse"));
    }
}

int style_get_decl(const char* decls, const char* key, char* out, size_t cap) {
    if (!decls || !key || !out || cap == 0) return 0;
    size_t kl = strlen(key);
    const char* p = decls;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        const char* ks = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { if (*p == ';') { p++; continue; } break; }
        const char* ke = p;
        while (ke > ks && isspace((unsigned char)ke[-1])) ke--;
        if ((size_t)(ke - ks) == kl && !strncmp(ks, key, kl)) {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            const char* vs = p;
            while (*p && *p != ';') p++;
            size_t vl = (size_t)(p - vs);
            while (vl > 0 && isspace((unsigned char)vs[vl - 1])) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, vs, vl);
            out[vl] = 0;
            return 1;
        }
        if (*p == ';') p++;
        else break;
    }
    return 0;
}

void style_apply_decls(Ctx* ctx, Style* st, const char* decls) {
    struct ApplyCtx ac = { ctx, st };
    for_each_decl(decls, apply_one, &ac);
}

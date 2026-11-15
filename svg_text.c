/* svg_text.c - Text-node content collection. */
#include "svg_internal.h"
#include <ctype.h>

/* Text subtrees can be nested arbitrarily deep in the source document; cap the
 * recursion so a hostile <tspan> chain cannot overflow the C stack. */
#define TEXT_MAX_DEPTH 512

static int collect_text(const XMLNode* xn, char** buf, size_t* len, size_t* cap,
                        int max_len, int depth) {
    if (!xn || depth > TEXT_MAX_DEPTH) return depth > TEXT_MAX_DEPTH;
    int truncated = 0;
    if (xn->text && *xn->text) {
        size_t tl = strlen(xn->text);
        if (*len < (size_t)max_len) {
            if (*len + tl > (size_t)max_len) tl = (size_t)max_len - *len;
            if (*len + tl + 1 > *cap) {
                size_t nc = (*len + tl + 1) * 2;
                char* nb = (char*)svg_realloc(*buf, nc);
                if (!nb) return 1;
                *buf = nb;
                *cap = nc;
            }
            memcpy(*buf + *len, xn->text, tl);
            *len += tl;
            (*buf)[*len] = 0;
            if (*len >= (size_t)max_len) truncated = 1;
        }
    }
    for (int i = 0; i < xn->nkids; i++)
        if (collect_text(xn->kids[i], buf, len, cap, max_len, depth + 1))
            truncated = 1;
    return truncated;
}

char* gather_text(const XMLNode* xn, int max_len, int* limit_hit) {
    if (limit_hit) *limit_hit = 0;
    if (max_len <= 0) max_len = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    char* buf = NULL;
    size_t len = 0, cap = 0;
    if (collect_text(xn, &buf, &len, &cap, max_len, 0) && limit_hit) *limit_hit = 1;
    if (!buf) return svg_strdup("");
    return buf;
}

/* ------------------------------------------------------------------ */
/* Positioned text runs (<text>, <tspan>, tref, textPath, altGlyph)    */
/* ------------------------------------------------------------------ */

static void push_chunk(Node* out, const char* s, float x, float y,
                       const Style* st, int max_len, int* limit_hit,
                       const FontFace* font) {
    if (!s || !*s) return;
    if (max_len <= 0) max_len = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    if (*limit_hit) return;
    TextChunk* nc = (TextChunk*)svg_realloc(
        out->chunks, (size_t)(out->nchunks + 1) * sizeof(TextChunk));
    if (!nc) return;
    out->chunks = nc;
    TextChunk* ch = &out->chunks[out->nchunks];
    memset(ch, 0, sizeof(*ch));
    size_t len = strlen(s);
    if ((int)len > max_len) { len = (size_t)max_len; *limit_hit = 1; }
    ch->text = (char*)svg_malloc(len + 1);
    if (!ch->text) return;
    memcpy(ch->text, s, len);
    ch->text[len] = 0;
    ch->x = x;
    ch->y = y;
    ch->fontsize = st->fontsize;
    ch->letterspacing = st->letterspacing;
    ch->bold = st->bold;
    ch->italic = st->italic;
    ch->anchor = st->anchor;
    ch->fill = st->fill;
    ch->textlen = st->textlen;
    ch->length_kind = st->length_kind;
    ch->writing = st->writing;
    ch->baseline = st->baseline;
    ch->font = font;
    if (st->rot && st->nrot > 0) {
        ch->rot = (float*)svg_malloc((size_t)st->nrot * sizeof(float));
        if (ch->rot) {
            memcpy(ch->rot, st->rot, (size_t)st->nrot * sizeof(float));
            ch->nrot = st->nrot;
        }
    }
    /* The chunk owns its paint copies; the style keeps its own. */
    ch->fill.stops = NULL;
    ch->fill.nstops = 0;
    ch->fill.ref = st->fill.ref ? svg_strdup(st->fill.ref) : NULL;
    if (st->fill.stops && st->fill.nstops > 0) {
        ch->fill.stops = (Stop*)svg_malloc((size_t)st->fill.nstops * sizeof(Stop));
        if (ch->fill.stops) {
            memcpy(ch->fill.stops, st->fill.stops,
                   (size_t)st->fill.nstops * sizeof(Stop));
            ch->fill.nstops = st->fill.nstops;
        }
    }
    out->nchunks++;
}

/* Concatenate all character data below an element (textPath content). */
static void collect_all_text(const XMLNode* xn, char** buf, size_t* len,
                             size_t* cap, int max_len, int depth) {
    if (!xn || depth > TEXT_MAX_DEPTH) return;
    if (xn->text && *xn->text) {
        size_t tl = strlen(xn->text);
        if (*len + tl > (size_t)max_len) tl = (size_t)max_len - *len;
        if (*len + tl + 1 > *cap) {
            size_t nc = (*len + tl + 1) * 2;
            char* nb = (char*)svg_realloc(*buf, nc);
            if (!nb) return;
            *buf = nb;
            *cap = nc;
        }
        memcpy(*buf + *len, xn->text, tl);
        *len += tl;
        (*buf)[*len] = 0;
    }
    for (int i = 0; i < xn->nkids; i++)
        collect_all_text(xn->kids[i], buf, len, cap, max_len, depth + 1);
}

static char* all_text(const XMLNode* xn, int max_len) {
    char* buf = NULL;
    size_t len = 0, cap = 0;
    collect_all_text(xn, &buf, &len, &cap, max_len, 0);
    if (!buf) return svg_strdup("");
    return buf;
}

/* Read the presentation attributes an element may set, plus inline style.
 * Returns 1 when the element is a text container this build renders. */
static int apply_text_style(Ctx* ctx, const XMLNode* xn, Style* st) {
    for (int a = 0; a < xn->nattrs; a++) {
        char decl[512];
        snprintf(decl, sizeof decl, "%s:%s", xn->attrs[a * 2], xn->attrs[a * 2 + 1]);
        style_apply_decls(ctx, st, decl);
    }
    style_apply_decls(ctx, st, xml_attr(xn, "style"));
    if (st->fill.type == PAINT_CURRENT_COLOR) {
        st->fill.type = PAINT_COLOR;
        st->fill.color = st->color;
    }
    return 1;
}

/* `rotate` is an attribute (not a CSS property): a list of angles. */
static void apply_rotate(Ctx* ctx, const XMLNode* xn, Style* st) {
    const char* r = xml_attr(xn, "rotate");
    if (!r) return;
    free(st->rot);
    st->rot = NULL;
    st->nrot = 0;
    const char* p = r;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char* end = NULL;
        float v = strtof(p, &end);
        if (end == p) break;
        float* nr = (float*)svg_realloc(st->rot, (size_t)(st->nrot + 1) * sizeof(float));
        if (!nr) return;
        st->rot = nr;
        st->rot[st->nrot++] = v;
        p = end;
    }
    (void)ctx;
}

static void walk_chunks(const XMLNode* xn, Ctx* ctx, Style* st,
                        float* cx, float* cy, Node* out, int* limit_hit,
                        int depth);

/* Place a child element's own text as one run. */
static void run_element(const XMLNode* xn, Ctx* ctx, Style* parent,
                        float* cx, float* cy, Node* out, int* limit_hit,
                        int depth) {
    if (depth > TEXT_MAX_DEPTH) { *limit_hit = 1; return; }
    Style child;
    style_copy(&child, parent);
    apply_text_style(ctx, xn, &child);
    apply_rotate(ctx, xn, &child);

    float nx = *cx, ny = *cy;
    const char* xs = xml_attr(xn, "x");
    const char* ys = xml_attr(xn, "y");
    const char* dxs = xml_attr(xn, "dx");
    const char* dys = xml_attr(xn, "dy");
    if (xs) nx = len_attr(ctx, xn, "x", 0, 0.0f);
    else if (dxs) nx += len_attr(ctx, xn, "dx", 0, 0.0f);
    if (ys) ny = len_attr(ctx, xn, "y", 1, 0.0f);
    else if (dys) ny += len_attr(ctx, xn, "dy", 1, 0.0f);

    if (xn->text && *xn->text)
        push_chunk(out, xn->text, nx, ny, &child,
                   ctx->lim ? ctx->lim->max_text_length
                            : SVG2IMG_LIMIT_MAX_TEXT_LENGTH, limit_hit,
                   ctx->font);
    float scale = child.fontsize / 7.0f;
    *cx = nx + (float)strlen(xn->text ? xn->text : "") * (6.0f * scale +
                                                          child.letterspacing);
    *cy = ny;
    walk_chunks(xn, ctx, &child, cx, cy, out, limit_hit, depth + 1);
    style_free(&child);
}

static void walk_chunks(const XMLNode* xn, Ctx* ctx, Style* st,
                        float* cx, float* cy, Node* out, int* limit_hit,
                        int depth) {
    if (!xn || depth > TEXT_MAX_DEPTH) {
        if (xn) *limit_hit = 1;
        return;
    }
    for (int i = 0; i < xn->nkids; i++) {
        const XMLNode* k = xn->kids[i];
        const char* nm = k->name;
        if (!strcmp(nm, "tspan") || !strcmp(nm, "a") ||
            !strcmp(nm, "altGlyph")) {
            run_element(k, ctx, st, cx, cy, out, limit_hit, depth + 1);
        } else if (!strcmp(nm, "tref")) {
            const char* href = xml_attr(k, "href");
            if (!href) href = xml_attr(k, "xlink:href");
            Style child;
            style_copy(&child, st);
            apply_text_style(ctx, k, &child);
            if (href && *href) {
                const char* h = (*href == '#') ? href + 1 : href;
                Node* ref = ctx->ids ? idmap_get(ctx->ids, h) : NULL;
                if (ref && ref->text && *ref->text) {
                    push_chunk(out, ref->text, *cx, *cy, &child,
                               ctx->lim ? ctx->lim->max_text_length
                                        : SVG2IMG_LIMIT_MAX_TEXT_LENGTH,
                               limit_hit, ctx->font);
                }
            }
            style_free(&child);
        } else if (!strcmp(nm, "textPath")) {
            Style child;
            style_copy(&child, st);
            apply_text_style(ctx, k, &child);
            float start = 0.0f;
            const char* so = xml_attr(k, "startOffset");
            if (so) {
                start = length_parse(so, ctx->vw, ctx->vh, ctx->em, 2, 0.0f);
            }
            char* text = all_text(k, ctx->lim ? ctx->lim->max_text_length
                                              : SVG2IMG_LIMIT_MAX_TEXT_LENGTH);
            if (text && *text) {
                push_chunk(out, text, *cx, *cy, &child,
                           ctx->lim ? ctx->lim->max_text_length
                                    : SVG2IMG_LIMIT_MAX_TEXT_LENGTH, limit_hit,
                           ctx->font);
                if (out->nchunks > 0) {
                    TextChunk* ch = &out->chunks[out->nchunks - 1];
                    const char* d = xml_attr(k, "path");
                    if (d && *d) ch->path = svg_strdup(d);
                    else {
                        const char* href = xml_attr(k, "href");
                        if (!href) href = xml_attr(k, "xlink:href");
                        if (href && *href) {
                            const char* h = (*href == '#') ? href + 1 : href;
                            ch->path_ref = svg_strdup(h);
                        }
                    }
                    ch->start_offset = start;
                }
            }
            free(text);
            style_free(&child);
        } else if (!strcmp(nm, "altGlyphDef") || !strcmp(nm, "altGlyphItem") ||
                   !strcmp(nm, "glyphRef")) {
            /* Definition only; the fallback is the altGlyph text. */
        } else {
            walk_chunks(k, ctx, st, cx, cy, out, limit_hit, depth + 1);
        }
    }
}

void gather_chunks(const XMLNode* xn, Ctx* ctx, Node* out, int* limit_hit) {
    if (!xn || !out) return;
    Style st = style_default();
    apply_text_style(ctx, xn, &st);
    apply_rotate(ctx, xn, &st);
    float x = len_attr(ctx, xn, "x", 0, 0.0f);
    float y = len_attr(ctx, xn, "y", 1, 0.0f);
    if (xn->text && *xn->text) {
        push_chunk(out, xn->text, x, y, &st,
                   ctx->lim ? ctx->lim->max_text_length
                            : SVG2IMG_LIMIT_MAX_TEXT_LENGTH, limit_hit,
                   ctx->font);
    }
    float scale = st.fontsize / 7.0f;
    float cx = x + (float)strlen(xn->text ? xn->text : "") *
                     (6.0f * scale + st.letterspacing);
    float cy = y;
    walk_chunks(xn, ctx, &st, &cx, &cy, out, limit_hit, 0);
    style_free(&st);
}

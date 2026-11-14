/* svg_text.c - Text-node content collection. */
#include "svg_internal.h"

static int collect_text(const XMLNode* xn, char** buf, size_t* len, size_t* cap,
                        int max_len) {
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
        if (collect_text(xn->kids[i], buf, len, cap, max_len)) truncated = 1;
    return truncated;
}

char* gather_text(const XMLNode* xn, int max_len, int* limit_hit) {
    if (limit_hit) *limit_hit = 0;
    if (max_len <= 0) max_len = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    char* buf = NULL;
    size_t len = 0, cap = 0;
    if (collect_text(xn, &buf, &len, &cap, max_len) && limit_hit) *limit_hit = 1;
    if (!buf) return svg_strdup("");
    return buf;
}

/* ------------------------------------------------------------------ */
/* Positioned text runs (<text>, <tspan>, dx/dy)                       */
/* ------------------------------------------------------------------ */

static void push_chunk(Node* out, const char* s, float x, float y,
                       const Style* st, int max_len, int* limit_hit) {
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

static void walk_chunks(const XMLNode* xn, Ctx* ctx, Style* st,
                        float* cx, float* cy, Node* out, int* limit_hit) {
    if (!xn) return;
    if (xn->text && *xn->text) {
        push_chunk(out, xn->text, *cx, *cy, st, ctx->lim ? ctx->lim->max_text_length
                                                         : SVG2IMG_LIMIT_MAX_TEXT_LENGTH,
                   limit_hit);
        float scale = st->fontsize / 7.0f;
        *cx += (float)strlen(xn->text) * (6.0f * scale + st->letterspacing);
    }
    for (int i = 0; i < xn->nkids; i++) {
        const XMLNode* k = xn->kids[i];
        if (!strcmp(k->name, "tspan") || !strcmp(k->name, "a")) {
            Style child;
            style_copy(&child, st);
            for (int a = 0; a < k->nattrs; a++) {
                char decl[512];
                snprintf(decl, sizeof decl, "%s:%s", k->attrs[a * 2],
                         k->attrs[a * 2 + 1]);
                style_apply_decls(ctx, &child, decl);
            }
            style_apply_decls(ctx, &child, xml_attr(k, "style"));
            if (child.fill.type == PAINT_CURRENT_COLOR) {
                child.fill.type = PAINT_COLOR;
                child.fill.color = child.color;
            }
            float nx = *cx, ny = *cy;
            const char* xs = xml_attr(k, "x");
            const char* ys = xml_attr(k, "y");
            const char* dxs = xml_attr(k, "dx");
            const char* dys = xml_attr(k, "dy");
            if (xs) nx = len_attr(ctx, k, "x", 0, 0.0f);
            else if (dxs) nx += len_attr(ctx, k, "dx", 0, 0.0f);
            if (ys) ny = len_attr(ctx, k, "y", 1, 0.0f);
            else if (dys) ny += len_attr(ctx, k, "dy", 1, 0.0f);
            walk_chunks(k, ctx, &child, &nx, &ny, out, limit_hit);
            *cx = nx;
            *cy = ny;
            style_free(&child);
        } else {
            walk_chunks(k, ctx, st, cx, cy, out, limit_hit);
        }
    }
}

void gather_chunks(const XMLNode* xn, Ctx* ctx, Node* out, int* limit_hit) {
    if (!xn || !out) return;
    Style st = style_default();
    float x = len_attr(ctx, xn, "x", 0, 0.0f);
    float y = len_attr(ctx, xn, "y", 1, 0.0f);
    /* Presentation attributes and inline style on <text> itself. */
    for (int a = 0; a < xn->nattrs; a++) {
        char decl[512];
        snprintf(decl, sizeof decl, "%s:%s", xn->attrs[a * 2], xn->attrs[a * 2 + 1]);
        style_apply_decls(ctx, &st, decl);
    }
    style_apply_decls(ctx, &st, xml_attr(xn, "style"));
    if (st.fill.type == PAINT_CURRENT_COLOR) {
        st.fill.type = PAINT_COLOR;
        st.fill.color = st.color;
    }
    walk_chunks(xn, ctx, &st, &x, &y, out, limit_hit);
    style_free(&st);
}

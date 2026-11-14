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

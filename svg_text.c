/* svg_text.c - Text-node content collection. */
#include "svg_internal.h"

static void collect_text(const XMLNode* xn, char** buf, size_t* len, size_t* cap) {
    if (xn->text && *xn->text) {
        size_t tl = strlen(xn->text);
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
    for (int i = 0; i < xn->nkids; i++) collect_text(xn->kids[i], buf, len, cap);
}

char* gather_text(const XMLNode* xn) {
    char* buf = NULL;
    size_t len = 0, cap = 0;
    collect_text(xn, &buf, &len, &cap);
    if (!buf) return svg_strdup("");
    return buf;
}

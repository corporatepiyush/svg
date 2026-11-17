/* xml_dom.c - DOM teardown and lookup for the SVG XML parser. */
#include "internal.h"

void xml_free(XMLNode* n) {
    if (!n) return;
    /* Iterative post-order teardown.  The parser accepts arbitrarily deep
     * nesting, so a recursive free would overflow the stack on adversarial
     * input even when rendering itself never recurses that far. */
    size_t cap = 64, cnt = 0;
    XMLNode** list = (XMLNode**)svg_malloc(cap * sizeof(*list));
    if (!list) return;
    list[cnt++] = n;
    for (size_t i = 0; i < cnt; i++) {
        XMLNode* cur = list[i];
        if (cur->nkids > 0) {
            size_t need = cnt + (size_t)cur->nkids;
            if (need > cap) {
                while (cap < need) cap *= 2;
                XMLNode** nl = (XMLNode**)svg_realloc(list, cap * sizeof(*list));
                if (!nl) { svg_mfree(list); return; }
                list = nl;
            }
            for (int k = 0; k < cur->nkids; k++) list[cnt++] = cur->kids[k];
        }
    }
    for (size_t i = cnt; i-- > 0; ) {
        XMLNode* cur = list[i];
        svg_mfree(cur->kids);
        /* name and attributes live in the element's own block. */
        svg_mfree(cur->text);
        svg_mfree(cur);
    }
    svg_mfree(list);
}

const char* xml_attr(const XMLNode* n, const char* name) {
    if (!n) return NULL;
    /* Exact match first (this is how qualified names such as xlink:href and
     * xml:space are stored). */
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i * 2], name)) return n->attrs[i * 2 + 1];
    /* An unprefixed lookup also matches a qualified attribute with the same
     * local name, so callers can ask for "href" or "space" without knowing
     * which prefix the document used. */
    for (int i = 0; i < n->nattrs; i++) {
        const char* a = n->attrs[i * 2];
        const char* colon = strchr(a, ':');
        if (colon && !strcmp(colon + 1, name)) return n->attrs[i * 2 + 1];
    }
    return NULL;
}

XMLNode* xml_child(const XMLNode* n, const char* name) {
    if (!n) return NULL;
    for (int i = 0; i < n->nkids; i++)
        if (!strcmp(n->kids[i]->name, name))
            return n->kids[i];
    return NULL;
}

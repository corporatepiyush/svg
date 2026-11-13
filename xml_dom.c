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
                if (!nl) { free(list); return; }
                list = nl;
            }
            for (int k = 0; k < cur->nkids; k++) list[cnt++] = cur->kids[k];
        }
    }
    for (size_t i = cnt; i-- > 0; ) {
        XMLNode* cur = list[i];
        free(cur->kids);
        for (int k = 0; k < cur->nattrs; k++) free(cur->attrs[k * 2]);
        free(cur->attrs);
        free(cur->name);
        free(cur->text);
        free(cur);
    }
    free(list);
}

const char* xml_attr(const XMLNode* n, const char* name) {
    if (!n) return NULL;
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i * 2], name)) return n->attrs[i * 2 + 1];
    return NULL;
}

XMLNode* xml_child(const XMLNode* n, const char* name) {
    if (!n) return NULL;
    for (int i = 0; i < n->nkids; i++)
        if (!strcmp(n->kids[i]->name, name))
            return n->kids[i];
    return NULL;
}

/* xml_parse.c - Minimal, lenient XML 1.0 parser producing a small DOM.
 *
 * Handles: elements, attributes (double/single quoted), text, comments,
 * processing instructions, CDATA, DOCTYPE, and the predefined plus
 * numeric character references.  Designed for SVG; not a general parser.
 * DOM teardown/lookup lives in xml_dom.c.
 */
#include "internal.h"
#include <ctype.h>

static XMLNode* new_elem(const char* name) {
    if (!name) return NULL;
    XMLNode* n = (XMLNode*)svg_malloc(sizeof(XMLNode));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->name = svg_strdup(name);
    if (!n->name) { free(n); return NULL; }   /* do not leave a nameless node */
    return n;
}

static int add_child(XMLNode* parent, XMLNode* child) {
    if (parent->nkids == parent->capkids) {
        int nc = parent->capkids ? parent->capkids * 2 : 4;
        XMLNode** nk = (XMLNode**)svg_realloc(parent->kids,
                                              (size_t)nc * sizeof(XMLNode*));
        if (!nk) return 0;
        parent->kids = nk;
        parent->capkids = nc;
    }
    child->parent = parent;
    parent->kids[parent->nkids++] = child;
    return 1;
}

static void add_attr(XMLNode* n, const char* name, const char* val) {
    /* Grow geometrically and pack both strings into one block, so a typical
     * element with several attributes costs two allocations in total rather
     * than one per attribute. */
    if (n->nattrs == n->capattrs) {
        int nc = n->capattrs ? n->capattrs * 2 : 4;
        char** na = (char**)svg_realloc(n->attrs, (size_t)nc * 2 * sizeof(char*));
        if (!na) return;
        n->attrs = na;
        n->capattrs = nc;
    }
    size_t nl = strlen(name), vl = strlen(val);
    char* block = (char*)svg_malloc(nl + vl + 2);
    if (!block) return;
    memcpy(block, name, nl + 1);
    char* vcopy = block + nl + 1;
    memcpy(vcopy, val, vl + 1);
    n->attrs[n->nattrs * 2]     = block;
    n->attrs[n->nattrs * 2 + 1] = vcopy;
    n->nattrs++;
}

/* Decode XML entities in the slice [s, s+len) into a freshly allocated,
 * NUL-terminated string.  Returns NULL on allocation failure. */
static char* decode_entities(const char* s, size_t len) {
    size_t cap = len * 6 + 1;
    char* out = (char*)svg_malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (s[i] == '&') {
            const char* semi = memchr(s + i, ';', len - i);
            if (semi && (size_t)(semi - (s + i)) <= 12) {
                char name[16];
                size_t nl = (size_t)(semi - (s + i + 1));
                if (nl < sizeof(name)) {
                    memcpy(name, s + i + 1, nl);
                    name[nl] = 0;
                    unsigned int cp = 0;
                    int ok = 0;
                    if (name[0] == '#') {
                        if (name[1] == 'x' || name[1] == 'X')
                            cp = (unsigned int)strtoul(name + 2, NULL, 16);
                        else
                            cp = (unsigned int)strtoul(name + 1, NULL, 10);
                        ok = 1;
                    } else if (!strcmp(name, "amp"))  { cp = '&';  ok = 1; }
                    else if (!strcmp(name, "lt"))   { cp = '<';  ok = 1; }
                    else if (!strcmp(name, "gt"))   { cp = '>';  ok = 1; }
                    else if (!strcmp(name, "quot")) { cp = '"';  ok = 1; }
                    else if (!strcmp(name, "apos")) { cp = '\''; ok = 1; }
                    else if (!strcmp(name, "nbsp")) { cp = 0xA0; ok = 1; }
                    if (ok) {
                        if (cp < 0x80) {
                            out[o++] = (char)cp;
                        } else if (cp < 0x800) {
                            out[o++] = (char)(0xC0 | (cp >> 6));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out[o++] = (char)(0xE0 | (cp >> 12));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[o++] = (char)(0xF0 | (cp >> 18));
                            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                        i = (size_t)(semi - s) + 1;
                        continue;
                    }
                }
            }
        }
        out[o++] = s[i++];
    }
    out[o] = 0;
    return out;
}

static void add_text(XMLNode* n, const char* s, size_t len) {
    /* Fast path: whitespace between elements (indentation, newlines) needs
     * no allocation and no storage. */
    int all_ws = 1;
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)s[i])) { all_ws = 0; break; }
    }
    if (all_ws) return;

    char* out = decode_entities(s, len);
    if (!out) return;
    size_t o = strlen(out);

    if (n->text) {
        size_t a = strlen(n->text);
        char* t = (char*)svg_malloc(a + o + 1);
        if (t) {
            memcpy(t, n->text, a);
            memcpy(t + a, out, o + 1);
            free(n->text);
            n->text = t;
        }
        free(out);
    } else {
        n->text = out;
    }
}

XMLNode* xml_parse(const char* data, size_t len) {
    if (!data) return NULL;          /* public API guard */
    XMLNode* doc = new_elem("#document");
    if (!doc) return NULL;

    XMLNode** stack = NULL;
    int sp = 0, scap = 0;
    size_t i = 0;

    /* skip UTF-8 BOM */
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        i = 3;

    XMLNode* cur = doc;

    while (i < len) {
        if (data[i] != '<') {
            size_t j = i;
            while (j < len && data[j] != '<') j++;
            add_text(cur, data + i, j - i);
            i = j;
            continue;
        }

        /* '<' */
        if (i + 1 >= len) break;

        if (data[i + 1] == '?') {
            size_t j = i + 2;
            while (j + 1 < len && !(data[j] == '?' && data[j + 1] == '>')) j++;
            i = (j + 1 < len) ? j + 2 : len;
            continue;
        }

        if (data[i + 1] == '!') {
            if (i + 3 < len && data[i + 2] == '-' && data[i + 3] == '-') {
                size_t j = i + 4;
                while (j + 2 < len &&
                       !(data[j] == '-' && data[j + 1] == '-' && data[j + 2] == '>')) j++;
                i = (j + 2 < len) ? j + 3 : len;
            } else if (i + 8 < len && !memcmp(data + i + 2, "[CDATA[", 7)) {
                size_t j = i + 9;
                while (j + 2 < len &&
                       !(data[j] == ']' && data[j + 1] == ']' && data[j + 2] == '>')) j++;
                add_text(cur, data + i + 9, j - (i + 9));
                i = (j + 2 < len) ? j + 3 : len;
            } else {
                /* DOCTYPE / declaration: skip to matching '>' */
                size_t j = i + 2;
                int depth = 0;
                while (j < len) {
                    if (data[j] == '[') depth++;
                    else if (data[j] == ']') { if (depth > 0) depth--; }
                    else if (data[j] == '>' && depth <= 0) break;
                    j++;
                }
                i = (j < len) ? j + 1 : len;
            }
            continue;
        }

        if (data[i + 1] == '/') {
            size_t ns = i + 2;
            size_t j = ns;
            while (j < len && !isspace((unsigned char)data[j]) && data[j] != '>') j++;
            size_t nlen = j - ns;
            while (j < len && data[j] != '>') j++;
            i = (j < len) ? j + 1 : len;
            while (sp > 0) {
                XMLNode* n = stack[--sp];
                if (n->name && strlen(n->name) == nlen &&
                    !strncmp(n->name, data + ns, nlen))
                    break;
            }
            cur = (sp > 0) ? stack[sp - 1] : doc;
            continue;
        }

        /* opening tag */
        {
            size_t ns = i + 1;
            size_t j = ns;
            while (j < len && !isspace((unsigned char)data[j]) &&
                   data[j] != '/' && data[j] != '>') j++;
            char* name = svg_strndup(data + ns, j - ns);
            XMLNode* el = new_elem(name);
            free(name);
            if (!el) break;   /* allocation failure: stop parsing cleanly */

            int selfclose = 0;
            while (j < len) {
                while (j < len && isspace((unsigned char)data[j])) j++;
                if (j >= len) break;
                if (data[j] == '>') { j++; break; }
                if (data[j] == '/') {
                    if (j + 1 < len && data[j + 1] == '>') { j += 2; selfclose = 1; break; }
                    j++;
                    continue;
                }
                size_t as = j;
                while (j < len && !isspace((unsigned char)data[j]) &&
                       data[j] != '=' && data[j] != '>' && data[j] != '/') j++;
                size_t anlen = j - as;
                while (j < len && isspace((unsigned char)data[j])) j++;

                /* Locate the value slice without copying it. */
                const char* vs = NULL;
                size_t vlen = 0;
                if (j < len && data[j] == '=') {
                    j++;
                    while (j < len && isspace((unsigned char)data[j])) j++;
                    if (j < len && (data[j] == '"' || data[j] == '\'')) {
                        char q = data[j++];
                        vs = data + j;
                        while (j < len && data[j] != q) j++;
                        vlen = (size_t)(data + j - vs);
                        if (j < len) j++;
                    } else {
                        vs = data + j;
                        while (j < len && !isspace((unsigned char)data[j]) &&
                               data[j] != '>') j++;
                        vlen = (size_t)(data + j - vs);
                    }
                }
                if (anlen > 0) {
                    char* aname = svg_strndup(data + as, anlen);
                    if (aname) {
                        if (vs) {
                            char* dec = decode_entities(vs, vlen);
                            add_attr(el, aname, dec ? dec : "");
                            free(dec);
                        } else {
                            add_attr(el, aname, "");
                        }
                        free(aname);
                    }
                }
            }
            i = j;

            if (!add_child(cur, el)) { xml_free(el); break; }   /* OOM */
            if (!selfclose) {
                if (sp == scap) {
                    int nscap = scap ? scap * 2 : 16;
                    XMLNode** nsx = (XMLNode**)svg_realloc(stack, (size_t)nscap * sizeof(XMLNode*));
                    if (!nsx) {
                        /* el is already linked into the tree; freeing it here
                         * would leave a dangling child.  Stop parsing: the
                         * document keeps what was built so far and is still
                         * structurally valid. */
                        break;
                    }
                    stack = nsx; scap = nscap;
                }
                stack[sp++] = el;
                cur = el;
            }
        }
    }

    free(stack);
    return doc;
}

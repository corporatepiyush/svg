/* svg_css.c - A small CSS subset for <style> elements (SVG 1.1 §6).
 *
 * Supported: type, class, id and universal selectors, descendant and child
 * combinators, comma-separated selector lists, and !important.  There is no
 * at-rule, pseudo-class, attribute selector, specificity beyond the (id,
 * class, type) triple, or media query support: the document is static, so
 * media queries would only add ways to be wrong.
 *
 * Rules are collected once, before the tree is built, so a <style> element
 * may appear after the elements it styles.
 */
#include "svg_internal.h"
#include <ctype.h>

static char* xstrdup(const char* s) {
    return s ? svg_strdup(s) : NULL;
}

static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static char* strip_comments(const char* s) {
    size_t cap = strlen(s) + 1, len = 0;
    char* out = (char*)svg_malloc(cap);
    if (!out) return NULL;
    for (size_t i = 0; s[i]; ) {
        if (s[i] == '/' && s[i + 1] == '*') {
            const char* e = strstr(s + i + 2, "*/");
            if (!e) break;
            i = (size_t)(e - s) + 2;
            continue;
        }
        out[len++] = s[i++];
    }
    out[len] = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* Selector matching                                                   */
/* ------------------------------------------------------------------ */

/* A compound selector: tag, #id, .class and * in any combination. */
static int match_compound(const XMLNode* xn, const char* sel) {
    char buf[256];
    size_t n = strlen(sel);
    if (n >= sizeof buf) return 0;
    memcpy(buf, sel, n + 1);

    char* tag = NULL;
    char* id = NULL;
    char* classes[16];
    int nclass = 0;

    char* p = buf;
    if (*p == '*') {
        p++;
    } else if (*p != '#' && *p != '.') {
        char* e = p;
        while (*e && *e != '#' && *e != '.') e++;
        tag = p;
        if (*e) *e++ = 0;
        p = e;
    }
    while (*p) {
        char kind = *p++;
        char* e = p;
        while (*e && *e != '#' && *e != '.') e++;
        if (*e) *e++ = 0;
        if (kind == '#') id = p;
        else if (nclass < 16) classes[nclass++] = p;
        p = e;
    }

    if (tag && strcmp(tag, xn->name)) return 0;
    const char* xid = xml_attr(xn, "id");
    if (id && (!xid || strcmp(id, xid))) return 0;
    if (nclass) {
        const char* cls = xml_attr(xn, "class");
        if (!cls) return 0;
        for (int i = 0; i < nclass; i++) {
            const char* q = cls;
            int found = 0;
            while (*q) {
                while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
                const char* e = q;
                while (*e && !isspace((unsigned char)*e) && *e != ',') e++;
                if (e == q) break;
                size_t len = (size_t)(e - q);
                if (len == strlen(classes[i]) && !strncmp(q, classes[i], len))
                    found = 1;
                q = e;
            }
            if (!found) return 0;
        }
    }
    return 1;
}

typedef struct { char comp[128]; char comb; } SelTok;

static int tokenize_selector(const char* sel, SelTok* t, int max) {
    int n = 0;
    const char* p = sel;
    char comb = 0;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '>') { comb = '>'; p++; continue; }
        const char* e = p;
        while (*e && !isspace((unsigned char)*e) && *e != '>') e++;
        size_t len = (size_t)(e - p);
        if (len >= sizeof t[0].comp) len = sizeof t[0].comp - 1;
        memcpy(t[n].comp, p, len);
        t[n].comp[len] = 0;
        t[n].comb = comb;
        comb = ' ';
        n++;
        p = e;
    }
    return n;
}

static int match_tokens(const XMLNode* xn, SelTok* t, int n) {
    if (!xn || n <= 0) return 0;
    if (!match_compound(xn, t[n - 1].comp)) return 0;
    if (n == 1) return 1;
    if (t[n - 1].comb == '>')
        return xn->parent && match_tokens(xn->parent, t, n - 1);
    for (const XMLNode* p = xn->parent; p; p = p->parent)
        if (match_tokens(p, t, n - 1)) return 1;
    return 0;
}

static int selector_matches(const char* sel, const XMLNode* xn) {
    SelTok t[32];
    int n = tokenize_selector(sel, t, 32);
    if (n == 0) return 0;
    return match_tokens(xn, t, n);
}

static int specificity_of(const char* sel) {
    int ids = 0, cls = 0, types = 0;
    for (const char* p = sel; *p; p++) {
        if (*p == '#') ids++;
        else if (*p == '.') cls++;
        else if (isalpha((unsigned char)*p)) {
            /* A type at the start of a compound (not part of an identifier). */
            if (p == sel || isspace((unsigned char)p[-1]) || p[-1] == '>' ||
                p[-1] == ',')
                types++;
        }
    }
    return ids * 10000 + cls * 100 + types;
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

static void add_rule(Ctx* ctx, char* sel, char* decls, char* decls_imp) {
    if (ctx->nrules == ctx->caprules) {
        int cap = ctx->caprules ? ctx->caprules * 2 : 16;
        CssRule* r = (CssRule*)svg_realloc(ctx->rules, (size_t)cap * sizeof(CssRule));
        if (!r) return;
        ctx->rules = r;
        ctx->caprules = cap;
    }
    CssRule* r = &ctx->rules[ctx->nrules++];
    r->sel = xstrdup(trim(sel));
    r->decls = decls;
    r->decls_imp = decls_imp;
    r->spec = specificity_of(r->sel ? r->sel : "");
}

/* Split a declaration block into normal and !important declaration lists. */
static void parse_declarations(const char* body, char** out_normal,
                               char** out_imp) {
    size_t cap = strlen(body) + 1;
    char* normal = (char*)svg_malloc(cap);
    char* imp = (char*)svg_malloc(cap);
    if (!normal || !imp) { free(normal); free(imp); *out_normal = NULL; *out_imp = NULL; return; }
    normal[0] = 0;
    imp[0] = 0;
    const char* p = body;
    while (*p) {
        const char* e = p;
        while (*e && *e != ';') e++;
        char decl[512];
        size_t len = (size_t)(e - p);
        if (len >= sizeof decl) len = sizeof decl - 1;
        memcpy(decl, p, len);
        decl[len] = 0;
        char* d = trim(decl);
        if (*d) {
            char* bang = strstr(d, "!");
            int important = 0;
            if (bang) {
                char* q = bang + 1;
                while (*q && isspace((unsigned char)*q)) q++;
                if (!strncmp(q, "important", 9)) {
                    important = 1;
                    *bang = 0;
                    char* de = trim(d);
                    d = de;
                }
            }
            if (*d) {
                char* colon = strchr(d, ':');
                if (colon) {
                    size_t dl = strlen(d);
                    char* dst = important ? imp : normal;
                    size_t dl0 = strlen(dst);
                    memcpy(dst + dl0, d, dl);
                    dst[dl0 + dl] = ';';
                    dst[dl0 + dl + 1] = 0;
                }
            }
        }
        p = (*e == ';') ? e + 1 : e;
    }
    *out_normal = normal;
    *out_imp = imp;
}

static void collect_style_text(Ctx* ctx, const char* css) {
    char* src = strip_comments(css);
    if (!src) return;
    const char* p = src;
    while (*p) {
        const char* brace = strchr(p, '{');
        if (!brace) break;
        const char* end = strchr(brace, '}');
        if (!end) break;
        char sel[1024];
        size_t sl = (size_t)(brace - p);
        if (sl >= sizeof sel) sl = sizeof sel - 1;
        memcpy(sel, p, sl);
        sel[sl] = 0;

        char body[4096];
        size_t bl = (size_t)(end - brace - 1);
        if (bl >= sizeof body) bl = sizeof body - 1;
        memcpy(body, brace + 1, bl);
        body[bl] = 0;

        /* Comma-separated selector lists: each part is its own rule so
         * specificity is computed per selector. */
        char* list = trim(sel);
        char* comma = list;
        while (comma) {
            char* next = strchr(comma, ',');
            if (next) *next++ = 0;
            char* one = trim(comma);
            if (*one) {
                char* normal = NULL, *imp = NULL;
                parse_declarations(body, &normal, &imp);
                add_rule(ctx, one, normal, imp);
            }
            comma = next;
        }
        p = end + 1;
    }
    free(src);
}

void css_collect(Ctx* ctx, const XMLNode* root) {
    if (!ctx || !root) return;
    for (int i = 0; i < root->nkids; i++) {
        const XMLNode* k = root->kids[i];
        if (!strcmp(k->name, "style") && k->text && *k->text)
            collect_style_text(ctx, k->text);
        css_collect(ctx, k);
    }
}

void css_free(Ctx* ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->nrules; i++) {
        free(ctx->rules[i].sel);
        free(ctx->rules[i].decls);
        free(ctx->rules[i].decls_imp);
    }
    free(ctx->rules);
    ctx->rules = NULL;
    ctx->nrules = ctx->caprules = 0;
}

/* ------------------------------------------------------------------ */
/* Application                                                         */
/* ------------------------------------------------------------------ */

/* style_apply_decls needs a Style*, so the helpers take it directly. */
static void apply_to(Ctx* ctx, const XMLNode* xn, Style* st, int important) {
    int idx[128];
    int n = 0;
    for (int i = 0; i < ctx->nrules && n < 128; i++) {
        if (!selector_matches(ctx->rules[i].sel, xn)) continue;
        idx[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && ctx->rules[idx[j]].spec > ctx->rules[v].spec) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
    for (int i = 0; i < n; i++) {
        const char* d = important ? ctx->rules[idx[i]].decls_imp
                                  : ctx->rules[idx[i]].decls;
        if (d && *d) style_apply_decls(ctx, st, d);
    }
}

void css_apply(Ctx* ctx, const XMLNode* xn, Style* st) {
    if (ctx && xn && st) apply_to(ctx, xn, st, 0);
}

void css_apply_important(Ctx* ctx, const XMLNode* xn, Style* st) {
    if (ctx && xn && st) apply_to(ctx, xn, st, 1);
}
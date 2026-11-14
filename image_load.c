/* image_load.c - Decode <image> references.
 *
 * Data URIs are always available.  External files are loaded only when the
 * caller supplied a base path: a relative reference is joined to it, and an
 * absolute one is used as is.  PNG and baseline JPEG are supported.
 */
#include "internal.h"
#include <ctype.h>

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned char* b64decode(const char* s, size_t* outn) {
    size_t cap = strlen(s) / 4 * 3 + 4;
    unsigned char* out = (unsigned char*)svg_malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    int acc = 0, bits = 0;
    for (const char* p = s; *p; p++) {
        int v;
        if (*p >= 'A' && *p <= 'Z') v = *p - 'A';
        else if (*p >= 'a' && *p <= 'z') v = *p - 'a' + 26;
        else if (*p >= '0' && *p <= '9') v = *p - '0' + 52;
        else if (*p == '+') v = 62;
        else if (*p == '/') v = 63;
        else continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) out[n++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    *outn = n;
    return out;
}

/* Decode %XX escapes; '+' becomes a space (the data URI convention). */
static char* pctdecode(const char* s, size_t len) {
    char* out = (char*)svg_malloc(len + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[n++] = (char)(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') out[n++] = ' ';
        else out[n++] = s[i];
    }
    out[n] = 0;
    return out;
}

static unsigned char* read_file(const char* path, size_t* out_n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (long)(256u << 20)) { fclose(f); return NULL; }
    rewind(f);
    unsigned char* buf = (unsigned char*)svg_malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_n = got;
    return buf;
}

/* Decode an in-memory image by sniffing its signature. */
static unsigned char* decode_bytes(const unsigned char* d, size_t n,
                                   int* w, int* h) {
    if (n >= 8 && d[0] == 137 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return png_decode_rgba(d, n, w, h);
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8)
        return jpeg_decode_rgba(d, n, w, h);
    return NULL;
}

int image_load_any(const char* href, const char* base_path, int aa,
                   unsigned char** out, int* out_w, int* out_h) {
    *out = NULL;
    *out_w = *out_h = 0;
    if (!href || !*href) return 0;

    if (!strncmp(href, "data:", 5)) {
        const char* comma = strchr(href, ',');
        if (!comma) return 0;
        size_t meta_len = (size_t)(comma - href);
        const char* data = comma + 1;
        size_t data_len = strlen(data);
        int is_b64 = meta_len >= 7 && !strncmp(comma - 7, ";base64", 7);
        if (strstr(href, "svg")) {
            /* Embedded SVG: render it with the normal pipeline. */
            char* svg = NULL;
            size_t slen = 0;
            if (is_b64) {
                svg = (char*)b64decode(data, &slen);
            } else {
                svg = pctdecode(data, data_len);
                if (svg) slen = strlen(svg);
            }
            if (!svg) return 0;
            svg2img_options o;
            memset(&o, 0, sizeof(o));
            o.aa = aa;
            o.scale = 1.0f;
            svg2img_limits lim;
            svg2img_default_limits(&lim);
            int hit = 0;
            Node* root = svg_parse(svg, slen, &o, &hit);
            free(svg);
            if (!root || !root->vp.valid || root->vp.w <= 0 || root->vp.h <= 0) {
                svg_free(root);
                return 0;
            }
            Canvas* src = canvas_create(root->vp.w, root->vp.h);
            if (!src) { svg_free(root); return 0; }
            render_document(src, root, mat_identity(), &o, &lim);
            int w = root->vp.w, h = root->vp.h;
            unsigned char* px = (unsigned char*)svg_malloc((size_t)w * (size_t)h * 4);
            if (!px) { canvas_free(src); svg_free(root); return 0; }
            size_t npix = (size_t)w * (size_t)h;
            for (size_t i = 0; i < npix; i++) {
                const float* s = src->px + i * 4;
                unsigned char* d = px + i * 4;
                d[0] = (unsigned char)(s[0] * 255.0f + 0.5f);
                d[1] = (unsigned char)(s[1] * 255.0f + 0.5f);
                d[2] = (unsigned char)(s[2] * 255.0f + 0.5f);
                d[3] = (unsigned char)(s[3] * 255.0f + 0.5f);
            }
            canvas_free(src);
            svg_free(root);
            *out = px;
            *out_w = w;
            *out_h = h;
            return 1;
        }
        unsigned char* raw = NULL;
        size_t rawn = 0;
        if (is_b64) raw = b64decode(data, &rawn);
        else {
            char* dec = pctdecode(data, data_len);
            if (dec) { raw = b64decode(dec, &rawn); free(dec); }
        }
        if (!raw) return 0;
        unsigned char* px = decode_bytes(raw, rawn, out_w, out_h);
        free(raw);
        if (!px) return 0;
        *out = px;
        return 1;
    }

    /* External file. */
    if (!base_path || !*base_path) return 0;
    char path[2048];
    if (href[0] == '/') {
        snprintf(path, sizeof path, "%s", href);
    } else {
        size_t bl = strlen(base_path);
        size_t hl = strlen(href);
        if (bl + hl + 2 > sizeof path) return 0;
        memcpy(path, base_path, bl);
        if (bl > 0 && base_path[bl - 1] != '/') path[bl++] = '/';
        memcpy(path + bl, href, hl + 1);
    }
    size_t n = 0;
    unsigned char* raw = read_file(path, &n);
    if (!raw) return 0;
    unsigned char* px = decode_bytes(raw, n, out_w, out_h);
    free(raw);
    if (!px) return 0;
    *out = px;
    return 1;
}
/* raster_image.c - <image> and feImage decoding.
 *
 * Embedded data URIs are supported: `data:image/svg+xml` (parsed and rendered
 * recursively) and `data:image/png` (decoded by image_png.c).  External files
 * are not loaded: the library API takes a document, not a base path, so a
 * relative reference cannot be resolved safely.  Such images are skipped.
 */
#include "raster_internal.h"

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

/* Decode %XX escapes and '+' as space (the data URI convention). */
static char* pctdecode(const char* s, size_t len) {
    char* out = (char*)svg_malloc(len + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out[n++] = (char)(hi * 16 + lo); i += 2; continue; }
        }
        if (s[i] == '+') out[n++] = ' ';
        else out[n++] = s[i];
    }
    out[n] = 0;
    return out;
}

/* Decode a data URI into an RGBA8 buffer.  Returns NULL when the URI is not
 * an embedded image this build understands.  The caller frees *out. */
int image_decode_href(const char* href, int aa, unsigned char** out,
                      int* out_w, int* out_h) {
    *out = NULL;
    *out_w = *out_h = 0;
    if (!href || strncmp(href, "data:", 5)) return 0;
    const char* comma = strchr(href, ',');
    if (!comma) return 0;
    size_t meta_len = (size_t)(comma - href);
    const char* data = comma + 1;
    size_t data_len = strlen(data);
    int is_b64 = meta_len >= 7 && !strncmp(comma - 7, ";base64", 7);
    int is_svg = strstr(href, "svg") != NULL;
    int is_png = strstr(href, "png") != NULL;
    if (!is_svg && !is_png) return 0;

    if (is_png) {
        unsigned char* raw = NULL;
        size_t rawn = 0;
        if (is_b64) raw = b64decode(data, &rawn);
        else {
            char* dec = pctdecode(data, data_len);
            if (dec) { raw = b64decode(dec, &rawn); free(dec); }
        }
        if (!raw) return 0;
        int w = 0, h = 0;
        unsigned char* px = png_decode_rgba(raw, rawn, &w, &h);
        free(raw);
        if (!px) return 0;
        *out = px;
        *out_w = w;
        *out_h = h;
        return 1;
    }

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
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const float* s = src->px + i * 4;
        unsigned char* d = px + i * 4;
        float a = s[3];
        float r = s[0], g = s[1], b = s[2];
        /* Store premultiplied-equivalent values so scaling stays correct. */
        d[0] = (unsigned char)(r * 255.0f + 0.5f);
        d[1] = (unsigned char)(g * 255.0f + 0.5f);
        d[2] = (unsigned char)(b * 255.0f + 0.5f);
        d[3] = (unsigned char)(a * 255.0f + 0.5f);
    }
    canvas_free(src);
    svg_free(root);
    *out = px;
    *out_w = w;
    *out_h = h;
    return 1;
}

/* Nearest-neighbour RGBA8 blit into a straight-alpha RGBA float canvas. */
void image_blit_rgba8(Canvas* dst, int dx, int dy, int dw, int dh,
                      const unsigned char* src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            const unsigned char* s = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            float* d = dst->px + ((size_t)ty * (size_t)dst->w + (size_t)tx) * 4;
            d[0] = s[0] / 255.0f;
            d[1] = s[1] / 255.0f;
            d[2] = s[2] / 255.0f;
            d[3] = s[3] / 255.0f;
        }
    }
}

/* Nearest-neighbour blit into an FBuf (feImage). */
void image_blit_fbuf(FBuf* dst, int dx, int dy, int dw, int dh,
                     const unsigned char* src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            const unsigned char* s = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            float* d = dst->px + ((size_t)ty * (size_t)dst->w + (size_t)tx) * 4;
            d[0] = s[0] / 255.0f;
            d[1] = s[1] / 255.0f;
            d[2] = s[2] / 255.0f;
            d[3] = s[3] / 255.0f;
        }
    }
}

void render_image(Canvas* c, Node* n, Mat world, float opacity,
                  Mask* clip, int aa) {
    if (!n || !n->image_href || !(n->img_w > 0.0f) || !(n->img_h > 0.0f)) return;

    /* Device rectangle of the image viewport. */
    Pt q[4] = { {n->img_x, n->img_y},
                {n->img_x + n->img_w, n->img_y},
                {n->img_x + n->img_w, n->img_y + n->img_h},
                {n->img_x, n->img_y + n->img_h} };
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt p = mat_apply(world, q[i]);
        if (p.x < x0) x0 = p.x;
        if (p.y < y0) y0 = p.y;
        if (p.x > x1) x1 = p.x;
        if (p.y > y1) y1 = p.y;
    }
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0);
    int ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > c->w) ix1 = c->w;
    if (iy1 > c->h) iy1 = c->h;
    if (ix0 >= ix1 || iy0 >= iy1) return;
    int dw = ix1 - ix0, dh = iy1 - iy0;

    Canvas* img = canvas_create(dw, dh);
    if (!img) return;
    unsigned char* raw = NULL;
    int sw = 0, sh = 0;
    if (!image_decode_href(n->image_href, aa, &raw, &sw, &sh)) {
        canvas_free(img);
        return;
    }
    image_blit_rgba8(img, 0, 0, dw, dh, raw, sw, sh);
    free(raw);

    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            const float* s = img->px + ((size_t)y * (size_t)dw + (size_t)x) * 4;
            float a = s[3] * opacity;
            if (clip) a *= mask_get(clip, ix0 + x, iy0 + y);
            if (a <= 0.0f) continue;
            blend(c, ix0 + x, iy0 + y, (Color){s[0], s[1], s[2], s[3]}, a);
        }
    }
    canvas_free(img);
}
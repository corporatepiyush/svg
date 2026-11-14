/* raster_image.c - <image> rendering.
 *
 * Decoding lives in image_load.c (data URIs and, with a base path, external
 * files).  A file image is decoded once at build time and carried on the node;
 * this file scales it into the image viewport and composites it.
 */
#include "raster_internal.h"

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

    unsigned char* raw = n->img_px;
    int sw = n->img_iw, sh = n->img_ih;
    int owned = 0;
    if (!raw) {
        if (!image_load_any(n->image_href, NULL, aa, &raw, &sw, &sh)) return;
        owned = 1;
    }
    Canvas* img = canvas_create(dw, dh);
    if (!img) { if (owned) free(raw); return; }
    image_blit_rgba8(img, 0, 0, dw, dh, raw, sw, sh);
    if (owned) free(raw);

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
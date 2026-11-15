/* raster_image.c - <image> rendering.
 *
 * Decoding lives in image_load.c (data URIs and, with a base path, external
 * files).  A file image is decoded once at build time and carried on the node;
 * this file scales it into the image viewport and composites it.
 */
#include "raster_internal.h"


/* Bilinear RGBA8 sampling with premultiplied interpolation, which avoids the
 * dark or light fringes a straight-alpha interpolation would produce. */
static void sample_bilinear8(const unsigned char* src, int sw, int sh, float fx,
                             float fy, float* out) {
    if (sw <= 0 || sh <= 0) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fx > (float)(sw - 1)) fx = (float)(sw - 1);
    if (fy > (float)(sh - 1)) fy = (float)(sh - 1);
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    int x1 = (x0 + 1 < sw) ? x0 + 1 : sw - 1;
    int y1 = (y0 + 1 < sh) ? y0 + 1 : sh - 1;
    float tx = fx - (float)x0, ty = fy - (float)y0;
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++) {
        int yy = j ? y1 : y0;
        float wy = j ? ty : 1.0f - ty;
        for (int i = 0; i < 2; i++) {
            int xx = i ? x1 : x0;
            float w = (i ? tx : 1.0f - tx) * wy;
            const unsigned char* p = src + ((size_t)yy * (size_t)sw + (size_t)xx) * 4;
            float a = (float)p[3] / 255.0f;
            acc[0] += (float)p[0] / 255.0f * a * w;
            acc[1] += (float)p[1] / 255.0f * a * w;
            acc[2] += (float)p[2] / 255.0f * a * w;
            acc[3] += a * w;
        }
    }
    if (acc[3] > 1e-6f) {
        out[0] = acc[0] / acc[3];
        out[1] = acc[1] / acc[3];
        out[2] = acc[2] / acc[3];
    } else {
        out[0] = out[1] = out[2] = 0.0f;
    }
    out[3] = acc[3];
}

/* RGBA8 blit into a straight-alpha RGBA float canvas. */
void image_blit_rgba8(Canvas* dst, int dx, int dy, int dw, int dh,
                      const unsigned char* src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            float c[4];
            sample_bilinear8(src, sw, sh, fx, fy, c);
            float* d = dst->px + ((size_t)ty * (size_t)dst->w + (size_t)tx) * 4;
            d[0] = c[0];
            d[1] = c[1];
            d[2] = c[2];
            d[3] = c[3];
        }
    }
}

/* Nearest-neighbour blit into an FBuf (feImage). */
void image_blit_fbuf(FBuf* dst, int dx, int dy, int dw, int dh,
                     const unsigned char* src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            float c[4];
            sample_bilinear8(src, sw, sh, fx, fy, c);
            float* d = dst->px + ((size_t)ty * (size_t)dst->w + (size_t)tx) * 4;
            d[0] = c[0];
            d[1] = c[1];
            d[2] = c[2];
            d[3] = c[3];
        }
    }
}

void render_image(Canvas* c, const Node* n, Mat world, float opacity,
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
    int ix0 = svg_f2i(floorf(x0), -1, c->w + 1, 0);
    int iy0 = svg_f2i(floorf(y0), -1, c->h + 1, 0);
    int ix1 = svg_f2i(ceilf(x1), -1, c->w + 1, 0);
    int iy1 = svg_f2i(ceilf(y1), -1, c->h + 1, 0);
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
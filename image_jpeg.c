/* image_jpeg.c - Baseline JPEG decoder for <image> (ITU T.81).
 *
 * Supports 8-bit baseline sequential DCT (SOF0) with Huffman coding, the four
 * common chroma subsampling layouts, restart intervals and both grayscale and
 * YCbCr colour.  Progressive, arithmetic-coded and 12-bit JPEG are rejected.
 * The decoder is self-contained: no external dependency.
 */
#include "internal.h"

/* ------------------------------------------------------------------ */
/* Bit reader over entropy-coded data                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char* d;
    size_t n, pos;
    unsigned buf;
    int cnt;
    int marker;              /* set when a marker was hit */
} JBits;

static int jb_byte(JBits* b) {
    if (b->pos >= b->n) { b->marker = 1; return 0; }
    return b->d[b->pos++];
}

static int jb_bit(JBits* b) {
    if (b->cnt == 0) {
        int c = jb_byte(b);
        if (c < 0) return 0;
        if (c == 0xFF) {
            int c2 = jb_byte(b);
            if (c2 == 0x00) {
                /* byte stuffing: 0xFF 0x00 carries a literal 0xFF */
            } else if (c2 >= 0xD0 && c2 <= 0xD7) {
                b->marker = c2;          /* restart marker */
                return 0;
            } else {
                b->marker = c2;
                return 0;
            }
        }
        b->buf = (unsigned)c;
        b->cnt = 8;
    }
    b->cnt--;
    return (b->buf >> b->cnt) & 1;
}

static int jb_bits(JBits* b, int n) {
    int v = 0;
    while (n-- > 0) v = (v << 1) | jb_bit(b);
    return v;
}

/* Extend a `n`-bit two's-complement value (JPEG's DIFF coding). */
static int jb_extend(int v, int n) {
    if (n == 0) return 0;
    return (v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

/* ------------------------------------------------------------------ */
/* Huffman tables                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char bits[17];              /* count of codes per length */
    unsigned char vals[256];
    int nvals;
    /* derived */
    int mincode[17], maxcode[18], valptr[17];
    int present;
} JHuff;

static void huff_build(JHuff* h) {
    int code = 0;
    int p = 0;
    for (int l = 1; l <= 16; l++) {
        if (h->bits[l]) {
            h->valptr[l] = p;
            h->mincode[l] = code;
            p += h->bits[l];
            h->maxcode[l] = code + h->bits[l] - 1;
            code += h->bits[l];
        } else {
            h->maxcode[l] = -1;
        }
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

static int huff_decode(JBits* b, const JHuff* h) {
    if (!h->present) return 0;
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | jb_bit(b);
        if (b->marker) return 0;
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l])
            return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Decoder state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int id, h, v, tq;
    int td, ta, pred;
    int pw, ph;                          /* plane size in samples */
    unsigned char* plane;
} JComp;

typedef struct {
    int width, height, ncomp;
    JComp comp[4];
    unsigned short qt[4][64];
    JHuff dc[4], ac[4];
    int restart_interval;
    int progressive;
} JDec;

/* zig-zag order */
static const int ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

/* Separable float IDCT, 8x8 in place.  K[x][u] = C(u) * cos((2x+1)u*pi/16)
 * with C(0) = 1/sqrt(2); the two passes together give the 1/4 factor. */
static float K[8][8];
static int K_ready;

static void idct_init(void) {
    if (K_ready) return;
    for (int x = 0; x < 8; x++) {
        for (int u = 0; u < 8; u++) {
            float c = (u == 0) ? 0.707106781f : 1.0f;
            K[x][u] = c * cosf((float)(2 * x + 1) * (float)u * (float)SVG_PI /
                               16.0f);
        }
    }
    K_ready = 1;
}

static void idct8(float* b) {
    idct_init();
    float tmp[64];
    /* Rows: transform along u for each row of coefficients. */
    for (int y = 0; y < 8; y++) {
        const float* s = b + y * 8;
        float* d = tmp + y * 8;
        int zero = 1;
        for (int u = 1; u < 8; u++)
            if (s[u] != 0.0f) { zero = 0; break; }
        if (zero) {
            float v = s[0] * 0.353553391f;      /* 0.5 * C(0) */
            for (int x = 0; x < 8; x++) d[x] = v;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            float sum = 0.0f;
            for (int u = 0; u < 8; u++) sum += K[x][u] * s[u];
            d[x] = sum * 0.5f;
        }
    }
    /* Columns: transform along v. */
    for (int x = 0; x < 8; x++) {
        float col[8];
        for (int y = 0; y < 8; y++) col[y] = tmp[y * 8 + x];
        int zero = 1;
        for (int v = 1; v < 8; v++)
            if (col[v] != 0.0f) { zero = 0; break; }
        if (zero) {
            float v = col[0] * 0.353553391f;    /* 0.5 * C(0) */
            for (int y = 0; y < 8; y++) b[y * 8 + x] = v;
            continue;
        }
        for (int y = 0; y < 8; y++) {
            float sum = 0.0f;
            for (int v = 0; v < 8; v++) sum += K[y][v] * col[v];
            b[y * 8 + x] = sum * 0.5f;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Marker parsing                                                      */
/* ------------------------------------------------------------------ */

static int rd_u8(const unsigned char* d, size_t n, size_t* p) {
    if (*p >= n) return 0;
    return d[(*p)++];
}

static int rd_u16(const unsigned char* d, size_t n, size_t* p) {
    int a = rd_u8(d, n, p), b = rd_u8(d, n, p);
    return (a << 8) | b;
}

static int parse_dqt(JDec* j, const unsigned char* d, size_t n, size_t* p,
                     int len) {
    size_t end = *p + (size_t)len;
    while (*p < end && *p < n) {
        int pq = d[*p] >> 4, tq = d[*p] & 15;
        (*p)++;
        if (tq > 3) return -1;
        for (int i = 0; i < 64; i++) {
            int v = pq ? rd_u16(d, n, p) : rd_u8(d, n, p);
            j->qt[tq][ZZ[i]] = (unsigned short)v;
        }
    }
    return 0;
}

static int parse_sof(JDec* j, const unsigned char* d, size_t n, size_t* p,
                     int len) {
    (void)len;
    rd_u8(d, n, p);                      /* precision */
    j->height = rd_u16(d, n, p);
    j->width = rd_u16(d, n, p);
    j->ncomp = rd_u8(d, n, p);
    if (j->ncomp < 1 || j->ncomp > 4) return -1;
    if (j->width <= 0 || j->height <= 0) return -1;
    for (int i = 0; i < j->ncomp; i++) {
        j->comp[i].id = rd_u8(d, n, p);
        int hv = rd_u8(d, n, p);
        j->comp[i].h = hv >> 4;
        j->comp[i].v = hv & 15;
        j->comp[i].tq = rd_u8(d, n, p);
        if (j->comp[i].h < 1 || j->comp[i].h > 4) j->comp[i].h = 1;
        if (j->comp[i].v < 1 || j->comp[i].v > 4) j->comp[i].v = 1;
        j->comp[i].pred = 0;
    }
    return 0;
}

static int parse_dht(JDec* j, const unsigned char* d, size_t n, size_t* p,
                     int len) {
    size_t end = *p + (size_t)len;
    while (*p < end && *p < n) {
        int tc = d[*p] >> 4, th = d[*p] & 15;
        (*p)++;
        if (th > 3) return -1;
        JHuff* h = tc ? &j->ac[th] : &j->dc[th];
        memset(h, 0, sizeof(*h));
        int total = 0;
        for (int l = 1; l <= 16; l++) {
            h->bits[l] = d[*p];
            (*p)++;
            total += h->bits[l];
        }
        if (total > 256) return -1;
        for (int i = 0; i < total; i++) h->vals[i] = d[(*p)++];
        h->nvals = total;
        huff_build(h);
        h->present = 1;
    }
    return 0;
}

/* Decode one 8x8 block of coefficients into `out` (already zeroed). */
static int decode_block(JBits* b, JDec* j, JComp* c, float* out) {
    int t = huff_decode(b, &j->dc[c->td]);
    int diff = 0;
    if (t) {
        int v = jb_bits(b, t);
        diff = jb_extend(v, t);
    }
    c->pred += diff;
    out[0] = (float)c->pred * (float)j->qt[c->tq][0];
    int k = 1;
    while (k < 64) {
        int rs = huff_decode(b, &j->ac[c->ta]);
        int s = rs & 15, r = rs >> 4;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k > 63) break;
        int v = jb_bits(b, s);
        /* out is in natural order; the quant table is stored in natural order
         * too, so the coefficient's natural index selects the quantiser. */
        out[ZZ[k]] = (float)jb_extend(v, s) * (float)j->qt[c->tq][ZZ[k]];
        k++;
    }
    return 0;
}

static void store_block(JComp* c, int bx, int by, const float* blk) {
    for (int y = 0; y < 8; y++) {
        int py = by + y;
        if (py >= c->ph) break;
        for (int x = 0; x < 8; x++) {
            int px = bx + x;
            if (px >= c->pw) break;
            float v = blk[y * 8 + x] + 128.0f;
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            c->plane[(size_t)py * (size_t)c->pw + (size_t)px] =
                (unsigned char)(v + 0.5f);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

unsigned char* jpeg_decode_rgba(const unsigned char* d, size_t n,
                                int* out_w, int* out_h) {
    *out_w = *out_h = 0;
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return NULL;

    JDec j;
    memset(&j, 0, sizeof(j));
    size_t p = 2;
    int have_sof = 0, have_sos = 0;
    JBits bits;
    memset(&bits, 0, sizeof(bits));

    while (p + 1 < n) {
        if (d[p] != 0xFF) { p++; continue; }
        int m = d[p + 1];
        p += 2;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;
        if (p + 2 > n) break;
        int len = (d[p] << 8) | d[p + 1];
        if (len < 2 || p + (size_t)len > n) break;
        size_t body = p + 2;
        int blen = len - 2;

        if (m == 0xDB) {
            if (parse_dqt(&j, d, n, &body, blen)) goto fail;
        } else if (m == 0xC4) {
            if (parse_dht(&j, d, n, &body, blen)) goto fail;
        } else if (m == 0xDD) {
            j.restart_interval = (d[body] << 8) | d[body + 1];
        } else if (m == 0xC0 || m == 0xC1) {
            if (parse_sof(&j, d, n, &body, blen)) goto fail;
            have_sof = 1;
        } else if (m == 0xC2 || (m >= 0xC3 && m <= 0xCF && m != 0xC4 &&
                                 m != 0xC8 && m != 0xCC)) {
            /* progressive / lossless / arithmetic: unsupported */
            goto fail;
        } else if (m == 0xDA) {
            if (!have_sof) goto fail;
            /* SOS header */
            int ns = d[body];
            size_t q = body + 1;
            for (int i = 0; i < ns; i++) {
                int cid = d[q];
                int tbl = d[q + 1];
                q += 2;
                for (int c = 0; c < j.ncomp; c++) {
                    if (j.comp[c].id == cid) {
                        j.comp[c].td = tbl >> 4;
                        j.comp[c].ta = tbl & 15;
                    }
                }
            }
            /* Entropy-coded data starts after the SOS header. */
            size_t data_start = p + len;
            bits.d = d;
            bits.n = n;
            bits.pos = data_start;
            bits.buf = 0;
            bits.cnt = 0;
            bits.marker = 0;
            have_sos = 1;

            /* Allocate planes and decode the scan. */
            int max_h = 1, max_v = 1;
            for (int c = 0; c < j.ncomp; c++) {
                if (j.comp[c].h > max_h) max_h = j.comp[c].h;
                if (j.comp[c].v > max_v) max_v = j.comp[c].v;
            }
            int mcux = (j.width + 8 * max_h - 1) / (8 * max_h);
            int mcuy = (j.height + 8 * max_v - 1) / (8 * max_v);
            for (int c = 0; c < j.ncomp; c++) {
                JComp* cc = &j.comp[c];
                cc->pw = mcux * 8 * cc->h;
                cc->ph = mcuy * 8 * cc->v;
                if ((size_t)cc->pw > (size_t)-1 / (size_t)cc->ph) goto fail;
                cc->plane = (unsigned char*)svg_malloc((size_t)cc->pw *
                                                       (size_t)cc->ph);
                if (!cc->plane) goto fail;
                memset(cc->plane, 0, (size_t)cc->pw * (size_t)cc->ph);
            }

            float blk[64];
            int mcu_count = 0;
            for (int my = 0; my < mcuy; my++) {
                for (int mx = 0; mx < mcux; mx++) {
                    if (j.restart_interval && mcu_count &&
                        mcu_count % j.restart_interval == 0) {
                        /* Skip to the next RSTn marker and reset predictors. */
                        bits.cnt = 0;
                        bits.marker = 0;
                        while (bits.pos + 1 < bits.n) {
                            if (bits.d[bits.pos] == 0xFF) {
                                int mm = bits.d[bits.pos + 1];
                                if (mm >= 0xD0 && mm <= 0xD7) {
                                    bits.pos += 2;
                                    break;
                                }
                            }
                            bits.pos++;
                        }
                        for (int c = 0; c < j.ncomp; c++) j.comp[c].pred = 0;
                    }
                    for (int c = 0; c < j.ncomp; c++) {
                        JComp* cc = &j.comp[c];
                        for (int by = 0; by < cc->v; by++) {
                            for (int bx = 0; bx < cc->h; bx++) {
                                memset(blk, 0, sizeof blk);
                                if (decode_block(&bits, &j, cc, blk)) goto fail;
                                idct8(blk);
                                store_block(cc, (mx * cc->h + bx) * 8,
                                            (my * cc->v + by) * 8, blk);
                            }
                        }
                    }
                    mcu_count++;
                }
            }

            /* Convert to RGBA. */
            if (j.width <= 0 || j.height <= 0) goto fail;
            if ((size_t)j.width > (size_t)-1 / (size_t)j.height / 4) goto fail;
            unsigned char* img = (unsigned char*)svg_malloc(
                (size_t)j.width * (size_t)j.height * 4);
            if (!img) goto fail;
            for (int y = 0; y < j.height; y++) {
                for (int x = 0; x < j.width; x++) {
                    unsigned char* o = img + ((size_t)y * j.width + x) * 4;
                    if (j.ncomp == 1) {
                        JComp* c0 = &j.comp[0];
                        int sx = x * c0->h / max_h;
                        int sy = y * c0->v / max_v;
                        if (sx >= c0->pw) sx = c0->pw - 1;
                        if (sy >= c0->ph) sy = c0->ph - 1;
                        int Y = c0->plane[(size_t)sy * c0->pw + sx];
                        o[0] = o[1] = o[2] = (unsigned char)Y;
                        o[3] = 255;
                    } else if (j.ncomp >= 3) {
                        JComp *c0 = &j.comp[0], *c1 = &j.comp[1], *c2 = &j.comp[2];
                        int x0 = x * c0->h / max_h, y0 = y * c0->v / max_v;
                        int x1 = x * c1->h / max_h, y1 = y * c1->v / max_v;
                        int x2 = x * c2->h / max_h, y2 = y * c2->v / max_v;
                        if (x0 >= c0->pw) x0 = c0->pw - 1;
                        if (y0 >= c0->ph) y0 = c0->ph - 1;
                        if (x1 >= c1->pw) x1 = c1->pw - 1;
                        if (y1 >= c1->ph) y1 = c1->ph - 1;
                        if (x2 >= c2->pw) x2 = c2->pw - 1;
                        if (y2 >= c2->ph) y2 = c2->ph - 1;
                        float Y = (float)c0->plane[(size_t)y0 * c0->pw + x0];
                        float Cb = (float)c1->plane[(size_t)y1 * c1->pw + x1] - 128.0f;
                        float Cr = (float)c2->plane[(size_t)y2 * c2->pw + x2] - 128.0f;
                        float r = Y + 1.402f * Cr;
                        float g = Y - 0.344136f * Cb - 0.714136f * Cr;
                        float b = Y + 1.772f * Cb;
                        if (r < 0) r = 0; if (r > 255) r = 255;
                        if (g < 0) g = 0; if (g > 255) g = 255;
                        if (b < 0) b = 0; if (b > 255) b = 255;
                        o[0] = (unsigned char)(r + 0.5f);
                        o[1] = (unsigned char)(g + 0.5f);
                        o[2] = (unsigned char)(b + 0.5f);
                        o[3] = 255;
                    } else {
                        o[0] = o[1] = o[2] = 0;
                        o[3] = 255;
                    }
                }
            }
            for (int c = 0; c < j.ncomp; c++) free(j.comp[c].plane);
            *out_w = j.width;
            *out_h = j.height;
            return img;
        }
        p += len;
    }
    (void)have_sos;
fail:
    for (int c = 0; c < j.ncomp; c++) {
        free(j.comp[c].plane);
        j.comp[c].plane = NULL;
    }
    return NULL;
}
/* png_write.c - PNG container writing, checksums and quantisation. */
#include "internal.h"
#include "png_internal.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */

static unsigned crc32_buf(const unsigned char* buf, size_t len) {
    static unsigned table[256];
    static int init = 0;
    if (!init) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = 1;
    }
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static unsigned adler32_buf(const unsigned char* buf, size_t len) {
    unsigned a = 1, b = 0;
    size_t i = 0;
    while (i < len) {
        size_t chunk = len - i;
        if (chunk > 5552) chunk = 5552;
        for (size_t k = 0; k < chunk; k++) {
            a += buf[i + k];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        i += chunk;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------------ */
/* PNG writing                                                         */
/* ------------------------------------------------------------------ */

static void put_u32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

static int write_chunk(FILE* f, const char* type, const unsigned char* data, size_t len) {
    unsigned char hdr[4];
    put_u32(hdr, (unsigned)len);
    if (fwrite(hdr, 1, 4, f) != 4) return 0;
    if (fwrite(type, 1, 4, f) != 4) return 0;
    if (len && fwrite(data, 1, len, f) != len) return 0;
    unsigned char* tmp = (unsigned char*)svg_malloc(len + 4);
    if (!tmp) return 0;
    memcpy(tmp, type, 4);
    if (len) memcpy(tmp + 4, data, len);
    unsigned crc = crc32_buf(tmp, len + 4);
    free(tmp);
    unsigned char cb[4];
    put_u32(cb, crc);
    if (fwrite(cb, 1, 4, f) != 4) return 0;
    return 1;
}

static inline unsigned char to8(float v) {
    /* fminf/fmaxf return the non-NaN operand, so NaN maps to 0 without a
     * branch, and the result is in [0,1] which makes the cast safe.  This
     * runs four times per pixel, so keeping it branch-free matters. */
    float c = fminf(fmaxf(v, 0.0f), 1.0f);
    return (unsigned char)(c * 255.0f + 0.5f);
}

int png_save(const char* path, const float* rgba, int w, int h) {
    if (!path || !rgba || w <= 0 || h <= 0) return 0;

    size_t stride = (size_t)w * 4;
    size_t rawlen = (size_t)h * (1 + stride);
    unsigned char* raw = (unsigned char*)svg_malloc(rawlen);
    if (!raw) return 0;

    for (int y = 0; y < h; y++) {
        unsigned char* row = raw + (size_t)y * (1 + stride);
        row[0] = 0; /* filter: none */
        for (int x = 0; x < w; x++) {
            const float* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            unsigned char* o = row + 1 + (size_t)x * 4;
            o[0] = to8(p[0]);
            o[1] = to8(p[1]);
            o[2] = to8(p[2]);
            o[3] = to8(p[3]);
        }
    }

    BW bw;
    bw_init(&bw);
    bw_push(&bw, 0x78);
    bw_push(&bw, 0x01);
    deflate_fixed(&bw, raw, rawlen);
    unsigned ad = adler32_buf(raw, rawlen);
    bw_push(&bw, (unsigned char)((ad >> 24) & 0xFF));
    bw_push(&bw, (unsigned char)((ad >> 16) & 0xFF));
    bw_push(&bw, (unsigned char)((ad >> 8) & 0xFF));
    bw_push(&bw, (unsigned char)(ad & 0xFF));
    free(raw);

    if (!bw.buf || bw.err) { free(bw.buf); return 0; }

    FILE* f = fopen(path, "wb");
    if (!f) { free(bw.buf); return 0; }

    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    int ok = 1;
    if (fwrite(sig, 1, 8, f) != 8) ok = 0;

    unsigned char ihdr[13];
    put_u32(ihdr, (unsigned)w);
    put_u32(ihdr + 4, (unsigned)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* colour type: RGBA */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    if (ok && !write_chunk(f, "IHDR", ihdr, 13)) ok = 0;

    for (size_t off = 0; off < bw.len; off += 65535) {
        size_t n = bw.len - off;
        if (n > 65535) n = 65535;
        if (ok && !write_chunk(f, "IDAT", bw.buf + off, n)) ok = 0;
    }
    if (ok && !write_chunk(f, "IEND", NULL, 0)) ok = 0;

    fclose(f);
    free(bw.buf);
    return ok ? 1 : 0;
}

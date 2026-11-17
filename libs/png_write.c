/* png_write.c - PNG container writing, checksums and quantisation. */
#include "internal.h"
#include "png_internal.h"
#include <stdio.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */

/* CRC-32 (PNG / zlib polynomial), slicing-by-8.  The table lives in the
 * caller's frame, built once per png_save(): no shared mutable state, so
 * concurrent conversions stay data-race free, and IDAT data is checksummed
 * eight bytes per step instead of one. */
typedef struct { uint32_t t[8][256]; } CrcTable;

static void crc_table_init(CrcTable* ct) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        ct->t[0][i] = c;
    }
    for (int k = 1; k < 8; k++)
        for (int i = 0; i < 256; i++)
            ct->t[k][i] = (ct->t[k - 1][i] >> 8) ^ ct->t[0][ct->t[k - 1][i] & 0xFF];
}

/* Continue a CRC: pass 0xFFFFFFFF to start and xor the final value. */
static uint32_t crc32_update(const CrcTable* ct, uint32_t c,
                             const unsigned char* p, size_t len) {
    while (len >= 8) {
        uint32_t lo = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        c = ct->t[7][lo & 0xFF] ^ ct->t[6][(lo >> 8) & 0xFF] ^
            ct->t[5][(lo >> 16) & 0xFF] ^ ct->t[4][lo >> 24] ^
            ct->t[3][p[4]] ^ ct->t[2][p[5]] ^ ct->t[1][p[6]] ^ ct->t[0][p[7]];
        p += 8;
        len -= 8;
    }
    while (len--) c = ct->t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}

static unsigned adler32_buf(const unsigned char* buf, size_t len) {
    uint32_t a = 1;
    uint64_t b = 0;               /* 64-bit so a full chunk cannot wrap */
    size_t i = 0;
    while (i < len) {
        size_t chunk = len - i;
        if (chunk > 5552) chunk = 5552;
        size_t end = i + chunk;
#if defined(SVG2IMG_NEON)
        /* Sixteen bytes at a time.  a += sum(x); b += 16*a_prev + the
         * weighted sum 16*x0 + 15*x1 + ... + x15, which is exactly what
         * the scalar loop computes for one 16-byte block. */
        static const unsigned char wl[8] = {16, 15, 14, 13, 12, 11, 10, 9};
        static const unsigned char wh[8] = {8, 7, 6, 5, 4, 3, 2, 1};
        const uint8x8_t wlo = vld1_u8(wl);
        const uint8x8_t whi = vld1_u8(wh);
        while (i + 16 <= end) {
            uint8x16_t v = vld1q_u8(buf + i);
            uint16x8_t lo = vmull_u8(vget_low_u8(v), wlo);
            uint16x8_t hi = vmull_u8(vget_high_u8(v), whi);
            unsigned ws = vaddvq_u32(vpaddlq_u16(vaddq_u16(lo, hi)));
            unsigned s = vaddlvq_u8(v);
            b += (uint64_t)16 * a + ws;
            a += s;
            i += 16;
        }
#endif
        for (; i < end; i++) {
            a += buf[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return ((unsigned)b << 16) | a;
}

/* Adler-32 of A||B from adler(A), adler(B) and len(B) (zlib's
 * adler32_combine), so segments can be checksummed independently. */
static unsigned adler32_combine(unsigned a1, unsigned a2, size_t len2) {
    const unsigned BASE = 65521u;
    unsigned rem = (unsigned)(len2 % BASE);
    unsigned long s1 = a1 & 0xFFFFu;
    unsigned long s2 = ((unsigned long)rem * s1) % BASE;
    s1 += (a2 & 0xFFFFu) + BASE - 1;
    s2 += ((a1 >> 16) & 0xFFFFu) + ((a2 >> 16) & 0xFFFFu) + BASE - rem;
    if (s1 >= BASE) s1 -= BASE;
    if (s1 >= BASE) s1 -= BASE;
    if (s2 >= (unsigned long)BASE << 1) s2 -= (unsigned long)BASE << 1;
    if (s2 >= BASE) s2 -= BASE;
    return (unsigned)(s1 | (s2 << 16));
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

static int write_chunk(FILE* f, const CrcTable* ct, const char* type,
                       const unsigned char* data, size_t len) {
    unsigned char hdr[4];
    put_u32(hdr, (unsigned)len);
    if (fwrite(hdr, 1, 4, f) != 4) return 0;
    if (fwrite(type, 1, 4, f) != 4) return 0;
    if (len && fwrite(data, 1, len, f) != len) return 0;
    uint32_t crc = crc32_update(ct, 0xFFFFFFFFu, (const unsigned char*)type, 4);
    if (len) crc = crc32_update(ct, crc, data, len);
    unsigned char cb[4];
    put_u32(cb, crc ^ 0xFFFFFFFFu);
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

/* Apply the PNG "Up" filter in place, bottom-up.  Row y predicts from row
 * y-1, which is still unfiltered when rows are visited in reverse, so no
 * second buffer is needed.  The filter is lossless; it mainly turns the
 * repeated background of large images into long zero runs for the LZ77
 * stage.  The first row has no predictor and keeps filter type 0. */
static void filter_up_inplace(unsigned char* raw, size_t stride, int h) {
    for (int y = h - 1; y >= 1; y--) {
        unsigned char* row = raw + (size_t)y * (stride + 1);
        const unsigned char* prev = row - (stride + 1);
        row[0] = 2;
        size_t j = 0;
#if defined(SVG2IMG_NEON)
        for (; j + 16 <= stride; j += 16) {
            uint8x16_t a = vld1q_u8(row + 1 + j);
            uint8x16_t b = vld1q_u8(prev + 1 + j);
            vst1q_u8(row + 1 + j, vsubq_u8(a, b));
        }
#endif
        for (; j < stride; j++) row[1 + j] = (unsigned char)(row[1 + j] - prev[1 + j]);
    }
    raw[0] = 0;
}

/* The zlib stream is cut into fixed-size segments of filtered image data,
 * each compressed as an independent DEFLATE block ending on a byte boundary.
 * The segmentation does not depend on the thread count, so serial and
 * parallel encodes produce identical files; at 256 KiB the ratio cost of
 * restarting the 32 KiB window is negligible. */
#define PNG_SEGMENT ((size_t)256 * 1024)

/* Quantise row y to 8-bit RGBA and apply the Up filter against row y-1,
 * reading only the float input: rows are independent, so any worker can
 * produce any row.  The bytes equal the serial in-place path's. */
static void quantise_filter_row(const float* rgba, unsigned char* raw, int w,
                                int y) {
    size_t stride = (size_t)w * 4;
    unsigned char* row = raw + (size_t)y * (1 + stride);
    const float* p = rgba + (size_t)y * stride;
    for (size_t j = 0; j < stride; j++) row[1 + j] = to8(p[j]);
    if (y == 0) { row[0] = 0; return; }
    const float* q = p - stride;
    row[0] = 2;
    for (size_t j = 0; j < stride; j++)
        row[1 + j] = (unsigned char)(row[1 + j] - to8(q[j]));
}

typedef struct {
    const float* rgba;
    unsigned char* raw;
    int w, h;
    size_t rawlen, nseg;
    BW* seg;                /* one writer per segment */
    unsigned* adler;        /* Adler-32 per segment */
    atomic_int next_rows;
    atomic_size_t next_seg;
} PngJob;

#define PNG_ROW_CHUNK 16

static void png_rows_worker(void* arg, int k) {
    PngJob* j = (PngJob*)arg;
    (void)k;
    for (;;) {
        int y0 = atomic_fetch_add(&j->next_rows, PNG_ROW_CHUNK);
        if (y0 >= j->h) break;
        int y1 = y0 + PNG_ROW_CHUNK < j->h ? y0 + PNG_ROW_CHUNK : j->h;
        for (int y = y0; y < y1; y++) quantise_filter_row(j->rgba, j->raw, j->w, y);
    }
}

static void png_seg_worker(void* arg, int k) {
    PngJob* j = (PngJob*)arg;
    (void)k;
    for (;;) {
        size_t s = atomic_fetch_add(&j->next_seg, 1);
        if (s >= j->nseg) break;
        size_t off = s * PNG_SEGMENT;
        size_t n = j->rawlen - off < PNG_SEGMENT ? j->rawlen - off : PNG_SEGMENT;
        bw_init(&j->seg[s]);
        deflate_block(&j->seg[s], j->raw + off, n, s + 1 == j->nseg);
        j->adler[s] = adler32_buf(j->raw + off, n);
    }
}

/* Parallel encode into *out (zlib header + segments + Adler-32).  Returns 0
 * if the extra buffers cannot be allocated; the float input is untouched
 * then, so the caller can fall back to the in-place serial path. */
static int png_encode_parallel(const float* rgba, int w, int h, int threads,
                               BW* out) {
    size_t rawlen = (size_t)h * (1 + (size_t)w * 4);
    PngJob j;
    j.rgba = rgba;
    j.w = w;
    j.h = h;
    j.rawlen = rawlen;
    j.nseg = (rawlen + PNG_SEGMENT - 1) / PNG_SEGMENT;
    j.raw = (unsigned char*)svg_malloc(rawlen);
    j.seg = (BW*)svg_calloc(j.nseg * sizeof(BW));
    j.adler = (unsigned*)svg_malloc(j.nseg * sizeof(unsigned));
    if (!j.raw || !j.seg || !j.adler) {
        svg_mfree(j.raw); svg_mfree(j.seg); svg_mfree(j.adler);
        return 0;
    }
    atomic_init(&j.next_rows, 0);
    atomic_init(&j.next_seg, 0);
    int nt = threads;
    if ((size_t)nt > j.nseg) nt = (int)j.nseg;
    svg_parallel_run(threads, png_rows_worker, &j);
    svg_parallel_run(nt, png_seg_worker, &j);

    size_t total = 6;
    int err = 0;
    for (size_t s = 0; s < j.nseg; s++) {
        if (j.seg[s].err || !j.seg[s].buf) err = 1;
        total += j.seg[s].len;
    }
    bw_init(out);
    if (!err) {
        out->buf = (unsigned char*)svg_malloc(total);
        if (!out->buf) err = 1;
    }
    if (!err) {
        unsigned char* o = out->buf;
        *o++ = 0x78;
        *o++ = 0x01;
        unsigned ad = 1;
        for (size_t s = 0; s < j.nseg; s++) {
            memcpy(o, j.seg[s].buf, j.seg[s].len);
            o += j.seg[s].len;
            size_t off = s * PNG_SEGMENT;
            size_t n = rawlen - off < PNG_SEGMENT ? rawlen - off : PNG_SEGMENT;
            ad = adler32_combine(ad, j.adler[s], n);
        }
        *o++ = (unsigned char)((ad >> 24) & 0xFF);
        *o++ = (unsigned char)((ad >> 16) & 0xFF);
        *o++ = (unsigned char)((ad >> 8) & 0xFF);
        *o++ = (unsigned char)(ad & 0xFF);
        out->len = out->cap = total;
    } else {
        out->err = 1;
    }
    for (size_t s = 0; s < j.nseg; s++) svg_mfree(j.seg[s].buf);
    svg_mfree(j.seg);
    svg_mfree(j.adler);
    svg_mfree(j.raw);
    return 1;
}

/* Serial encode, quantising in place over the float canvas. */
static void png_encode_serial(float* rgba, int w, int h, BW* out) {
    size_t stride = (size_t)w * 4;
    size_t rawlen = (size_t)h * (1 + stride);
    /* Each output row occupies 4*w+1 bytes, which is always behind the input
     * row it reads (16*w bytes per row), so by the time row y is written all
     * bytes it can touch belong to rows already consumed.  The one exception
     * is the filter byte of the first row, which aliases the first float; it
     * is handled after loading that pixel's channels.  This removes a
     * full-size raw image buffer from peak RSS. */
    unsigned char* raw = (unsigned char*)rgba;

    for (int y = 0; y < h; y++) {
        unsigned char* row = raw + (size_t)y * (1 + stride);
        int x0 = 0;
        if (y == 0) {
            /* row[0] aliases rgba[0]; load the pixel before overwriting. */
            float r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
            row[0] = 0; /* filter: none */
            row[1] = to8(r);
            row[2] = to8(g);
            row[3] = to8(b);
            row[4] = to8(a);
            x0 = 1;
        } else {
            row[0] = 0; /* filter: none */
        }
        for (int x = x0; x < w; x++) {
            const float* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            /* Load all channels before storing: the first pixel's output
             * overlaps its own input bytes. */
            float r = p[0], g = p[1], b = p[2], a = p[3];
            unsigned char* o = row + 1 + (size_t)x * 4;
            o[0] = to8(r);
            o[1] = to8(g);
            o[2] = to8(b);
            o[3] = to8(a);
        }
    }

    filter_up_inplace(raw, stride, h);

    bw_init(out);
    bw_push(out, 0x78);
    bw_push(out, 0x01);
    for (size_t off = 0; off < rawlen && !out->err; off += PNG_SEGMENT) {
        size_t n = rawlen - off < PNG_SEGMENT ? rawlen - off : PNG_SEGMENT;
        deflate_block(out, raw + off, n, off + n == rawlen);
    }
    unsigned ad = adler32_buf(raw, rawlen);
    bw_push(out, (unsigned char)((ad >> 24) & 0xFF));
    bw_push(out, (unsigned char)((ad >> 16) & 0xFF));
    bw_push(out, (unsigned char)((ad >> 8) & 0xFF));
    bw_push(out, (unsigned char)(ad & 0xFF));
    bw_flush(out);
}

int png_save(const char* path, float* rgba, int w, int h, int threads) {
    if (!path || !rgba || w <= 0 || h <= 0) return 0;

    BW bw;
    size_t rawlen = (size_t)h * (1 + (size_t)w * 4);
    /* Parallel only pays off with at least two segments to share. */
    if (threads < 2 || rawlen <= PNG_SEGMENT ||
        !png_encode_parallel(rgba, w, h, threads, &bw))
        png_encode_serial(rgba, w, h, &bw);

    if (!bw.buf || bw.err) { svg_mfree(bw.buf); return 0; }

    FILE* f = fopen(path, "wb");
    if (!f) { svg_mfree(bw.buf); return 0; }

    CrcTable* ct = (CrcTable*)svg_malloc(sizeof(CrcTable));
    if (!ct) { fclose(f); svg_mfree(bw.buf); return 0; }
    crc_table_init(ct);

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
    if (ok && !write_chunk(f, ct, "IHDR", ihdr, 13)) ok = 0;

    for (size_t off = 0; off < bw.len; off += 65535) {
        size_t n = bw.len - off;
        if (n > 65535) n = 65535;
        if (ok && !write_chunk(f, ct, "IDAT", bw.buf + off, n)) ok = 0;
    }
    if (ok && !write_chunk(f, ct, "IEND", NULL, 0)) ok = 0;

    if (fclose(f) != 0) ok = 0;   /* buffered data may fail to flush */
    svg_mfree(ct);
    svg_mfree(bw.buf);
    return ok ? 1 : 0;
}

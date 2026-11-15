/* png_deflate.c - PNG bit writer and DEFLATE compressor
 * (fixed Huffman + LZ77).  No external dependencies.
 */
#include "internal.h"
#include "png_internal.h"

/* ------------------------------------------------------------------ */
/* Bit writer                                                          */
/* ------------------------------------------------------------------ */

void bw_init(BW* w) {
    w->buf = NULL; w->len = 0; w->cap = 0; w->bitbuf = 0; w->bitcnt = 0;
    w->err = 0;
}

void bw_push(BW* w, unsigned char b) {
    if (w->err) return;
    if (w->len == w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 4096;
        unsigned char* nb = (unsigned char*)svg_realloc(w->buf, nc);
        if (!nb) { w->err = 1; return; }
        w->buf = nb;
        w->cap = nc;
    }
    if (w->len < w->cap) w->buf[w->len++] = b;
}

static void bw_bits(BW* w, unsigned code, int n) {
    for (int i = n - 1; i >= 0; i--) {
        w->bitbuf |= (unsigned)(((code >> i) & 1u) << w->bitcnt);
        if (++w->bitcnt == 8) {
            bw_push(w, (unsigned char)(w->bitbuf & 0xFF));
            w->bitbuf = 0;
            w->bitcnt = 0;
        }
    }
}

static void bw_bits_lsb(BW* w, unsigned value, int n) {
    for (int i = 0; i < n; i++) {
        w->bitbuf |= (unsigned)((value & 1u) << w->bitcnt);
        value >>= 1;
        if (++w->bitcnt == 8) {
            bw_push(w, (unsigned char)(w->bitbuf & 0xFF));
            w->bitbuf = 0;
            w->bitcnt = 0;
        }
    }
}

/* Flush any remaining bits; the byte is already zero-padded in the
 * high bits because the stream fills from the least-significant bit. */
void bw_flush(BW* w) {
    if (w->bitcnt > 0) {
        bw_push(w, (unsigned char)(w->bitbuf & 0xFF));
        w->bitbuf = 0;
        w->bitcnt = 0;
    }
}

/* Fixed-Huffman literal/length code (also used for symbols 256..287). */
static void bw_symbol(BW* w, int v) {
    if (v <= 143)      bw_bits(w, (unsigned)(0x30 + v), 8);
    else if (v <= 255) bw_bits(w, (unsigned)(0x190 + (v - 144)), 9);
    else if (v <= 279) bw_bits(w, (unsigned)(v - 256), 7);
    else               bw_bits(w, (unsigned)(0xC0 + (v - 280)), 8);
}

/* ------------------------------------------------------------------ */
/* DEFLATE: fixed Huffman block with LZ77                              */
/* ------------------------------------------------------------------ */

typedef struct { unsigned short base; unsigned char extra; } LenCode;
typedef struct { unsigned short base; unsigned char extra; } DistCode;

static const LenCode len_table[29] = {
    {3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},
    {11,1},{13,1},{15,1},{17,1},
    {19,2},{23,2},{27,2},{31,2},
    {35,3},{43,3},{51,3},{59,3},
    {67,4},{83,4},{99,4},{115,4},
    {131,5},{163,5},{195,5},{227,5},
    {258,0}
};

static const DistCode dist_table[30] = {
    {1,0},{2,0},{3,0},{4,0},{5,1},{7,1},{9,2},{13,2},{17,3},{25,3},
    {33,4},{49,4},{65,5},{97,5},{129,6},{193,6},{257,7},{385,7},
    {513,8},{769,8},{1025,9},{1537,9},{2049,10},{3073,10},
    {4097,11},{6145,11},{8193,12},{12289,12},{16385,13},{24577,13}
};

#define LZ_HASH(p) (((data[p] << 10) ^ (data[(p) + 1] << 5) ^ data[(p) + 2]) & 32767)

void deflate_fixed(BW* w, const unsigned char* data, size_t n) {
    bw_bits_lsb(w, 1, 1);   /* BFINAL */
    bw_bits_lsb(w, 1, 2);   /* BTYPE = 01 (fixed Huffman), packed LSB-first */

    int* head = (int*)svg_malloc(32768 * sizeof(int));
    int* prev = (int*)svg_malloc(32768 * sizeof(int));
    if (!head || !prev) { free(head); free(prev); return; }
    for (int i = 0; i < 32768; i++) head[i] = -1;

    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            unsigned h = LZ_HASH(i);
            int cur = head[h];
            prev[i & 32767] = cur;
            head[h] = (int)i;
            int chain = 0;
            int maxlen = (int)((n - i) < 258 ? (n - i) : 258);
            while (cur >= 0 && chain < 32) {
                size_t dist = i - (size_t)cur;
                if (dist > 32768) break;
                int l = 0;
                while (l < maxlen && data[(size_t)cur + (size_t)l] == data[i + (size_t)l]) l++;
                if (l > best_len) { best_len = l; best_dist = (int)dist; }
                if (best_len >= maxlen) break;
                cur = prev[(size_t)cur & 32767];
                chain++;
            }
        }
        if (best_len >= 3) {
            int li = 0;
            while (li < 28 && len_table[li + 1].base <= (unsigned short)best_len) li++;
            bw_symbol(w, 257 + li);
            if (len_table[li].extra)
                bw_bits_lsb(w, (unsigned)(best_len - len_table[li].base), len_table[li].extra);
            int di = 0;
            while (di < 29 && dist_table[di + 1].base <= (unsigned short)best_dist) di++;
            bw_bits(w, (unsigned)di, 5);
            if (dist_table[di].extra)
                bw_bits_lsb(w, (unsigned)(best_dist - dist_table[di].base), dist_table[di].extra);
            for (int k = 1; k < best_len; k++) {
                if (i + (size_t)k + 3 <= n) {
                    unsigned h2 = LZ_HASH(i + (size_t)k);
                    prev[(i + (size_t)k) & 32767] = head[h2];
                    head[h2] = (int)(i + (size_t)k);
                }
            }
            i += (size_t)best_len;
        } else {
            bw_symbol(w, data[i]);
            i++;
        }
    }
    bw_symbol(w, 256); /* end of block */
    bw_flush(w);
    free(head);
    free(prev);
}

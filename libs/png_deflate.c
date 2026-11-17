/* png_deflate.c - PNG bit writer and DEFLATE compressor
 * (fixed Huffman + LZ77).  No external dependencies.
 */
#include "internal.h"
#include "png_internal.h"

/* ------------------------------------------------------------------ */
/* Bit writer                                                          */
/* ------------------------------------------------------------------ */

static const unsigned short rev9[512] = {
    0, 256, 128, 384, 64, 320, 192, 448, 32, 288, 160, 416, 96, 352, 224, 480,
    16, 272, 144, 400, 80, 336, 208, 464, 48, 304, 176, 432, 112, 368, 240, 496,
    8, 264, 136, 392, 72, 328, 200, 456, 40, 296, 168, 424, 104, 360, 232, 488,
    24, 280, 152, 408, 88, 344, 216, 472, 56, 312, 184, 440, 120, 376, 248, 504,
    4, 260, 132, 388, 68, 324, 196, 452, 36, 292, 164, 420, 100, 356, 228, 484,
    20, 276, 148, 404, 84, 340, 212, 468, 52, 308, 180, 436, 116, 372, 244, 500,
    12, 268, 140, 396, 76, 332, 204, 460, 44, 300, 172, 428, 108, 364, 236, 492,
    28, 284, 156, 412, 92, 348, 220, 476, 60, 316, 188, 444, 124, 380, 252, 508,
    2, 258, 130, 386, 66, 322, 194, 450, 34, 290, 162, 418, 98, 354, 226, 482,
    18, 274, 146, 402, 82, 338, 210, 466, 50, 306, 178, 434, 114, 370, 242, 498,
    10, 266, 138, 394, 74, 330, 202, 458, 42, 298, 170, 426, 106, 362, 234, 490,
    26, 282, 154, 410, 90, 346, 218, 474, 58, 314, 186, 442, 122, 378, 250, 506,
    6, 262, 134, 390, 70, 326, 198, 454, 38, 294, 166, 422, 102, 358, 230, 486,
    22, 278, 150, 406, 86, 342, 214, 470, 54, 310, 182, 438, 118, 374, 246, 502,
    14, 270, 142, 398, 78, 334, 206, 462, 46, 302, 174, 430, 110, 366, 238, 494,
    30, 286, 158, 414, 94, 350, 222, 478, 62, 318, 190, 446, 126, 382, 254, 510,
    1, 257, 129, 385, 65, 321, 193, 449, 33, 289, 161, 417, 97, 353, 225, 481,
    17, 273, 145, 401, 81, 337, 209, 465, 49, 305, 177, 433, 113, 369, 241, 497,
    9, 265, 137, 393, 73, 329, 201, 457, 41, 297, 169, 425, 105, 361, 233, 489,
    25, 281, 153, 409, 89, 345, 217, 473, 57, 313, 185, 441, 121, 377, 249, 505,
    5, 261, 133, 389, 69, 325, 197, 453, 37, 293, 165, 421, 101, 357, 229, 485,
    21, 277, 149, 405, 85, 341, 213, 469, 53, 309, 181, 437, 117, 373, 245, 501,
    13, 269, 141, 397, 77, 333, 205, 461, 45, 301, 173, 429, 109, 365, 237, 493,
    29, 285, 157, 413, 93, 349, 221, 477, 61, 317, 189, 445, 125, 381, 253, 509,
    3, 259, 131, 387, 67, 323, 195, 451, 35, 291, 163, 419, 99, 355, 227, 483,
    19, 275, 147, 403, 83, 339, 211, 467, 51, 307, 179, 435, 115, 371, 243, 499,
    11, 267, 139, 395, 75, 331, 203, 459, 43, 299, 171, 427, 107, 363, 235, 491,
    27, 283, 155, 411, 91, 347, 219, 475, 59, 315, 187, 443, 123, 379, 251, 507,
    7, 263, 135, 391, 71, 327, 199, 455, 39, 295, 167, 423, 103, 359, 231, 487,
    23, 279, 151, 407, 87, 343, 215, 471, 55, 311, 183, 439, 119, 375, 247, 503,
    15, 271, 143, 399, 79, 335, 207, 463, 47, 303, 175, 431, 111, 367, 239, 495,
    31, 287, 159, 415, 95, 351, 223, 479, 63, 319, 191, 447, 127, 383, 255, 511,
};

void bw_init(BW* w) {
    w->buf = NULL; w->len = 0; w->cap = 0; w->bitbuf = 0; w->bitcnt = 0;
    w->err = 0;
}

/* Grow the output buffer so at least `need` bytes fit.  The bit accumulator
 * can emit at most four bytes per bw_put(), so callers reserve eight and the
 * hot path never rechecks capacity. */
static void bw_grow(BW* w, size_t need) {
    if (w->err) return;
    size_t nc = w->cap ? w->cap : 4096;
    while (nc < need) {
        if (nc > (size_t)-1 / 2) { nc = need; break; }
        nc *= 2;
    }
    unsigned char* nb = (unsigned char*)svg_realloc(w->buf, nc);
    if (!nb) { w->err = 1; return; }
    w->buf = nb;
    w->cap = nc;
}

static inline void bw_reserve(BW* w, size_t need) {
    if (w->cap - w->len < need) bw_grow(w, w->len + need);
}

/* Append n bits (n <= 32), least-significant bit first.  Full 32-bit words
 * are drained in one store instead of one byte per eight bits. */
static inline void bw_put(BW* w, uint64_t v, int n) {
    if (w->err) return;
    w->bitbuf |= (v & (((uint64_t)1 << n) - 1)) << w->bitcnt;
    w->bitcnt += n;
    if (w->bitcnt >= 32) {
        w->buf[w->len + 0] = (unsigned char)(w->bitbuf);
        w->buf[w->len + 1] = (unsigned char)(w->bitbuf >> 8);
        w->buf[w->len + 2] = (unsigned char)(w->bitbuf >> 16);
        w->buf[w->len + 3] = (unsigned char)(w->bitbuf >> 24);
        w->len += 4;
        w->bitbuf >>= 32;
        w->bitcnt -= 32;
    }
}

void bw_push(BW* w, unsigned char b) {
    bw_reserve(w, 8);
    bw_put(w, b, 8);
}

void bw_flush(BW* w) {
    if (w->err) return;
    int nbytes = (w->bitcnt + 7) / 8;
    bw_reserve(w, (size_t)nbytes);
    for (int i = 0; i < nbytes; i++)
        w->buf[w->len++] = (unsigned char)(w->bitbuf >> (8 * i));
    w->bitbuf = 0;
    w->bitcnt = 0;
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

/* Length 3..258 -> index into len_table.  The previous linear scan walked
 * up to 28 entries for every match; the table makes it one load. */
static const unsigned char len_index_tab[256] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  8,  9,  9, 10, 10, 11, 11,
    12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15,
    16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17,
    18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 28
};

/* Distance 1..32768 -> index into dist_table.  Codes double in width every
 * two entries; clz recovers the pair and the high bit selects within it. */
static inline int dist_index(unsigned d) {
    if (d <= 2) return (int)d - 1;
    unsigned x = d - 1;                 /* 2..32767 */
    int l = 31 - __builtin_clz(x);
    return 2 * l + (int)((x >> (l - 1)) & 1u);
}

static inline unsigned lz_hash4(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> 17;   /* 15 bits */
}
#define LZ_HASH(p) lz_hash4(data + (p))

/* First byte at which two 8-byte words differ, endian-independent. */
static inline int first_diff_byte(uint64_t d) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_clzll(d) >> 3;
#else
    return __builtin_ctzll(d) >> 3;
#endif
}

/* Length of the common prefix of a[] and b[], at most maxlen bytes. */
static inline int common_prefix(const unsigned char* a, const unsigned char* b,
                                int maxlen) {
    int l = 0;
    while (l + 8 <= maxlen) {
        uint64_t x, y;
        memcpy(&x, a + l, 8);
        memcpy(&y, b + l, 8);
        uint64_t d = x ^ y;
        if (d) return l + first_diff_byte(d);
        l += 8;
    }
    while (l < maxlen && a[l] == b[l]) l++;
    return l;
}

/* Length of the run of byte p[0] starting at p[0], at most maxlen bytes. */
static inline int same_run(const unsigned char* p, int maxlen) {
    uint64_t pat = 0x0101010101010101ull * (uint64_t)p[0];
    int l = 1;
    while (l + 8 <= maxlen) {
        uint64_t x;
        memcpy(&x, p + l, 8);
        if (x != pat) return l + first_diff_byte(x ^ pat);
        l += 8;
    }
    while (l < maxlen && p[l] == p[0]) l++;
    return l;
}

/* Fixed-Huffman literal/length codes, bit-reversed for LSB-first output:
 * code in the low 16 bits, length in the high bits.  Built once per call
 * (288 entries) so no shared mutable state is needed. */
static void fixed_lit_table(uint32_t* t) {
    for (int v = 0; v < 288; v++) {
        unsigned code;
        int n;
        if (v <= 143)      { code = (unsigned)(0x30 + v); n = 8; }
        else if (v <= 255) { code = (unsigned)(0x190 + (v - 144)); n = 9; }
        else if (v <= 279) { code = (unsigned)(v - 256); n = 7; }
        else               { code = (unsigned)(0xC0 + (v - 280)); n = 8; }
        t[v] = ((uint32_t)rev9[code] >> (9 - n)) | ((uint32_t)n << 16);
    }
}

/* The emit loop keeps the bit accumulator and output cursor in locals: the
 * byte stores go through `unsigned char*`, which may alias anything, so
 * state held in *w would be reloaded after every store.  Capacity is
 * reserved per token (a match emits at most 9+5+5+13 = 32 bits). */
typedef struct {
    unsigned char* buf;
    size_t len, cap;
    uint64_t bits;
    int nbits;
} BitCur;

static inline void cur_put(BitCur* c, uint64_t v, int n) {
    c->bits |= v << c->nbits;
    c->nbits += n;
    if (c->nbits >= 32) {
        unsigned char* o = c->buf + c->len;
        o[0] = (unsigned char)(c->bits);
        o[1] = (unsigned char)(c->bits >> 8);
        o[2] = (unsigned char)(c->bits >> 16);
        o[3] = (unsigned char)(c->bits >> 24);
        c->len += 4;
        c->bits >>= 32;
        c->nbits -= 32;
    }
}

static inline void cur_sym(BitCur* c, const uint32_t* lit, int v) {
    uint32_t e = lit[v];
    cur_put(c, e & 0xFFFFu, (int)(e >> 16));
}

static void cur_load(BitCur* c, const BW* w) {
    c->buf = w->buf; c->len = w->len; c->cap = w->cap;
    c->bits = w->bitbuf; c->nbits = w->bitcnt;
}

static void cur_store(const BitCur* c, BW* w) {
    w->buf = c->buf; w->len = c->len; w->cap = c->cap;
    w->bitbuf = c->bits; w->bitcnt = c->nbits;
}

/* Make room for 16 more bytes; 0 (with w->err set) on failure. */
static int cur_reserve(BitCur* c, BW* w) {
    if (c->cap - c->len >= 16) return 1;
    cur_store(c, w);
    bw_reserve(w, 16);
    cur_load(c, w);
    return !w->err && w->cap - w->len >= 16;
}

/* Compress `data` as one fixed-Huffman block that starts at the writer's
 * current bit position and ends on a byte boundary.  A non-final block is
 * followed by an empty stored block (a zlib "sync flush"), so independently
 * compressed segments can be concatenated byte-wise.  Matches never reach
 * outside `data`, which is what makes segments independent. */
void deflate_block(BW* w, const unsigned char* data, size_t n, int final) {
    bw_reserve(w, 8);
    bw_put(w, final ? 1u : 0u, 1);  /* BFINAL */
    bw_put(w, 1, 2);        /* BTYPE = 01 (fixed Huffman), packed LSB-first */
    if (w->err) return;

    int* head = (int*)svg_malloc(32768 * sizeof(int));
    int* prev = (int*)svg_malloc(32768 * sizeof(int));
    if (!head || !prev) {
        svg_mfree(head); svg_mfree(prev);
        w->err = 1;                 /* a truncated stream must not "succeed" */
        return;
    }
    for (int i = 0; i < 32768; i++) head[i] = -1;
    uint32_t lit[288];
    fixed_lit_table(lit);

    BitCur c;
    cur_load(&c, w);
    size_t i = 0;
    while (i < n) {
        if (!cur_reserve(&c, w)) break;
        int best_len = 0, best_dist = 0;
        if (i + 4 <= n) {
            unsigned h = LZ_HASH(i);
            int cur = head[h];
            prev[i & 32767] = cur;
            head[h] = (int)i;
            int chain = 0;
            int maxlen = (int)((n - i) < 258 ? (n - i) : 258);
            /* Cheap RLE path: a repeated byte is a distance-1 match and
             * needs neither a hash probe nor chain walk. */
            if (i > 0 && data[i] == data[i - 1]) {
                int l = same_run(data + i, maxlen);
                if (l >= 3) { best_len = l; best_dist = 1; }
            }
            if (best_len < 3) {
                while (cur >= 0 && chain < 32) {
                    size_t dist = i - (size_t)cur;
                    if (dist > 32768) break;
                    /* A candidate can only win if it also matches at the
                     * current best length (zlib's quick reject); skipping the
                     * full compare otherwise leaves the result unchanged. */
                    if (best_len > 0 &&
                        data[(size_t)cur + (size_t)best_len] != data[i + (size_t)best_len]) {
                        cur = prev[(size_t)cur & 32767];
                        chain++;
                        continue;
                    }
                    int l = common_prefix(data + (size_t)cur, data + i, maxlen);
                    if (l > best_len) { best_len = l; best_dist = (int)dist; }
                    if (best_len >= maxlen) break;
                    cur = prev[(size_t)cur & 32767];
                    chain++;
                }
            }
        }
        if (best_len >= 3) {
            int li = len_index_tab[best_len - 3];
            cur_sym(&c, lit, 257 + li);
            if (len_table[li].extra)
                cur_put(&c, (unsigned)(best_len - len_table[li].base), len_table[li].extra);
            int di = dist_index((unsigned)best_dist);
            cur_put(&c, (uint64_t)rev9[di] >> 4, 5);
            if (dist_table[di].extra)
                cur_put(&c, (unsigned)(best_dist - dist_table[di].base), dist_table[di].extra);
            i += (size_t)best_len;
        } else {
            cur_sym(&c, lit, data[i]);
            i++;
        }
    }
    if (!w->err && cur_reserve(&c, w)) cur_sym(&c, lit, 256);  /* end of block */
    cur_store(&c, w);
    if (!final) {
        bw_reserve(w, 8);
        bw_put(w, 0, 3);    /* BFINAL=0, BTYPE=00: stored */
        bw_flush(w);        /* pad to a byte boundary */
        bw_reserve(w, 8);
        if (!w->err) {      /* LEN = 0, NLEN = ~0 */
            w->buf[w->len++] = 0x00; w->buf[w->len++] = 0x00;
            w->buf[w->len++] = 0xFF; w->buf[w->len++] = 0xFF;
        }
    }
    bw_flush(w);
    svg_mfree(head);
    svg_mfree(prev);
}

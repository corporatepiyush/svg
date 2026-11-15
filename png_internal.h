/* png_internal.h - PNG bit writer / DEFLATE interface. */
#ifndef PNG_INTERNAL_H
#define PNG_INTERNAL_H

#include "internal.h"

typedef struct {
    unsigned char* buf;
    size_t len, cap;
    unsigned bitbuf;
    int bitcnt;
    int err;              /* set once a buffer allocation fails */
} BW;

void bw_init(BW* w);
void bw_push(BW* w, unsigned char b);
void bw_flush(BW* w);
void deflate_fixed(BW* w, const unsigned char* data, size_t n);

#endif

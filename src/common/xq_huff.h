/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_HUFF_H
#define XQ_HUFF_H

#include <stddef.h>
#include <stdint.h>
#include "xq.h"

#define XQ_HUFF_SYMBOLS   256
#define XQ_HUFF_MAX_BITS  15
#define XQ_HUFF_TABLE_BYTES (XQ_HUFF_SYMBOLS / 2)

typedef struct {
    uint8_t  *buf;
    size_t    cap;
    size_t    pos;
    uint64_t  acc;
    int       nbits;
    int       overflow;
} xq_bitw;

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    uint64_t       acc;
    int            nbits;
    int            bad;
} xq_bitr;

void xq_bitw_init(xq_bitw *w, uint8_t *buf, size_t cap);
void xq_bitw_put(xq_bitw *w, uint32_t value, int bits);
int  xq_bitw_flush(xq_bitw *w, size_t *out_len);

void xq_bitr_init(xq_bitr *r, const uint8_t *buf, size_t len);

typedef struct {
    uint8_t  len[XQ_HUFF_SYMBOLS];
    uint16_t code[XQ_HUFF_SYMBOLS];
    int      max_len;
    int      used;
} xq_huff_enc;

typedef struct {
    uint16_t sym[1 << XQ_HUFF_MAX_BITS];
    uint8_t  len[1 << XQ_HUFF_MAX_BITS];
    int      max_len;
} xq_huff_dec;

int  xq_huff_build(xq_huff_enc *e, const uint32_t *freq);
void xq_huff_store(const xq_huff_enc *e, uint8_t *dst);
int  xq_huff_load(xq_huff_dec *d, const uint8_t *src);

static inline void xq_huff_emit(xq_bitw *w, const xq_huff_enc *e, unsigned sym)
{
    xq_bitw_put(w, e->code[sym], e->len[sym]);
}

int xq_huff_decode(xq_bitr *r, const xq_huff_dec *d);

#endif

/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "xq_codec.h"
#include "xq_codec_lzb.h"
#include "xq_huff.h"
#include "xq_bits.h"
#include "xq_checked.h"

#define LZE_MODE_RAW  0u
#define LZE_MODE_HUFF 1u

typedef struct {
    void    *lzb;
    uint8_t *mid;
    size_t   mid_cap;
} lze_cctx;

typedef struct {
    uint8_t *mid;
    size_t   mid_cap;
} lze_dctx;

static size_t lze_bound(size_t src_size)
{
    return xq_lzb_bound(src_size) + XQ_HUFF_TABLE_BYTES + 32;
}

static void *lze_cctx_new(void)
{
    lze_cctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->lzb = xq_lzb_cctx_new();
    if (!c->lzb) { free(c); return NULL; }
    return c;
}

static void lze_cctx_free(void *p)
{
    lze_cctx *c = (lze_cctx *)p;
    if (!c) return;
    xq_lzb_cctx_free(c->lzb);
    free(c->mid);
    free(c);
}

static void *lze_dctx_new(void) { return calloc(1, sizeof(lze_dctx)); }

static void lze_dctx_free(void *p)
{
    lze_dctx *d = (lze_dctx *)p;
    if (!d) return;
    free(d->mid);
    free(d);
}

static int reserve(uint8_t **buf, size_t *cap, size_t need)
{
    if (*cap >= need) return 1;
    free(*buf);
    *buf = malloc(need);
    if (!*buf) { *cap = 0; return 0; }
    *cap = need;
    return 1;
}

static int put_varint(uint8_t *dst, size_t cap, size_t *pos, uint64_t v)
{
    while (v >= 0x80) {
        if (*pos >= cap) return 0;
        dst[(*pos)++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    if (*pos >= cap) return 0;
    dst[(*pos)++] = (uint8_t)v;
    return 1;
}

static int get_varint(const uint8_t *src, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        if (*pos >= len) return 0;
        uint8_t b = src[(*pos)++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) return 0;
    }
    *out = v;
    return 1;
}

static xq_status lze_compress(void *cctxp, void *dstv, size_t dst_cap, size_t *out_len,
                              const void *srcv, size_t src_len, int level,
                              const void *cdict)
{
    lze_cctx *c = (lze_cctx *)cctxp;
    if (!c) return XQ_ERR_INTERNAL;
    if (src_len == 0) { *out_len = 0; return XQ_OK; }

    uint8_t *dst = (uint8_t *)dstv;

    size_t need = xq_lzb_bound(src_len) + 32;
    if (!reserve(&c->mid, &c->mid_cap, need)) return XQ_ERR_OOM;

    size_t mlen = 0;
    xq_status st = xq_lzb_compress(c->lzb, c->mid, c->mid_cap, &mlen,
                                   srcv, src_len, level, cdict);
    if (st != XQ_OK) return st;

    uint32_t freq[XQ_HUFF_SYMBOLS];
    memset(freq, 0, sizeof freq);
    for (size_t i = 0; i < mlen; i++) freq[c->mid[i]]++;

    xq_huff_enc enc;
    size_t op = 0;

    if (mlen >= 64 && xq_huff_build(&enc, freq)) {
        size_t hdr = op;
        if (op >= dst_cap) return XQ_ERR_DST_TOO_SMALL;
        dst[op++] = LZE_MODE_HUFF;
        if (!put_varint(dst, dst_cap, &op, mlen)) return XQ_ERR_DST_TOO_SMALL;
        if (op + XQ_HUFF_TABLE_BYTES > dst_cap) return XQ_ERR_DST_TOO_SMALL;
        xq_huff_store(&enc, dst + op);
        op += XQ_HUFF_TABLE_BYTES;

        xq_bitw w;
        xq_bitw_init(&w, dst + op, dst_cap - op);
        for (size_t i = 0; i < mlen; i++) xq_huff_emit(&w, &enc, c->mid[i]);
        size_t blen = 0;
        if (xq_bitw_flush(&w, &blen)) {
            op += blen;

            if (op < mlen + 1) { *out_len = op; return XQ_OK; }
        }
        op = hdr;
    }

    if (op + 1 + mlen > dst_cap) return XQ_ERR_DST_TOO_SMALL;
    dst[op++] = LZE_MODE_RAW;
    memcpy(dst + op, c->mid, mlen);
    op += mlen;
    *out_len = op;
    return XQ_OK;
}

static xq_status lze_decompress(void *dctxp, void *dstv, size_t dst_cap, size_t *out_len,
                                const void *srcv, size_t src_len, size_t expected_len,
                                const void *ddict)
{
    lze_dctx *d = (lze_dctx *)dctxp;
    if (!d) return XQ_ERR_INTERNAL;
    if (src_len == 0) { *out_len = 0; return XQ_OK; }

    const uint8_t *src = (const uint8_t *)srcv;
    size_t ip = 0;
    uint8_t mode = src[ip++];

    if (mode == LZE_MODE_RAW)
        return xq_lzb_decompress(NULL, dstv, dst_cap, out_len,
                                 src + ip, src_len - ip, expected_len, ddict);

    if (mode != LZE_MODE_HUFF) return XQ_ERR_CORRUPT_BLOCK;

    uint64_t mlen64;
    if (!get_varint(src, src_len, &ip, &mlen64)) return XQ_ERR_CORRUPT_BLOCK;

    if (mlen64 > (uint64_t)XQ_BLOCK_SIZE_MAX + (XQ_BLOCK_SIZE_MAX / 255) + 64)
        return XQ_ERR_CORRUPT_BLOCK;
    size_t mlen = (size_t)mlen64;

    if (ip + XQ_HUFF_TABLE_BYTES > src_len) return XQ_ERR_CORRUPT_BLOCK;
    xq_huff_dec dec;
    if (!xq_huff_load(&dec, src + ip)) return XQ_ERR_CORRUPT_BLOCK;
    ip += XQ_HUFF_TABLE_BYTES;

    if (!reserve(&d->mid, &d->mid_cap, mlen ? mlen : 1)) return XQ_ERR_OOM;

    xq_bitr r;
    xq_bitr_init(&r, src + ip, src_len - ip);
    for (size_t i = 0; i < mlen; i++) {
        int s = xq_huff_decode(&r, &dec);
        if (s < 0) return XQ_ERR_CORRUPT_BLOCK;
        d->mid[i] = (uint8_t)s;
    }

    return xq_lzb_decompress(NULL, dstv, dst_cap, out_len,
                             d->mid, mlen, expected_len, ddict);
}

const xq_codec_vt xq_codec_lze_vt = {
    XQ_CODEC_LZE, "lze",
    lze_bound,
    lze_cctx_new, lze_cctx_free,
    lze_dctx_new, lze_dctx_free,
    xq_lzb_cdict_new, xq_lzb_cdict_free,
    xq_lzb_ddict_new, xq_lzb_ddict_free,
    lze_compress, lze_decompress
};

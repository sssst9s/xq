/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "xq_codec.h"
#include "xq_codec_lzb.h"
#include "xq_bits.h"
#include "xq_checked.h"

typedef struct {
    const uint8_t *data;
    size_t         len;
    uint32_t      *head;
    uint32_t      *chain;
    int            hlog;
} lzb_cdict;

typedef struct {
    const uint8_t *data;
    size_t         len;
} lzb_ddict;

typedef struct {
    uint32_t *head;
    uint32_t *chain;
    size_t    chain_cap;
    int       hlog;
} lzb_cctx;

XQ_WRAPS
static uint32_t hash4(uint32_t v, int hlog)
{
    return (v * 2654435761u) >> (32 - hlog);
}

static uint32_t read32(const uint8_t *p) { return xq_ld32le(p); }

static int depth_for(int level)
{
    if (level <= 1) return 4;
    if (level <= 3) return 8;
    if (level <= 5) return 16;
    if (level <= 7) return 32;
    if (level <= 9) return 64;
    return 128;
}

static int hlog_for(size_t n)
{
    int h = LZB_HLOG_MIN;
    while (h < LZB_HLOG_MAX && ((size_t)1 << h) < n) h++;
    return h;
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
        if (shift > 63) return 0;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) return 0;
    }
    *out = v;
    return 1;
}

void *xq_lzb_cdict_new(const void *bytes, size_t len, int level, uint32_t block_size)
{
    (void)level; (void)block_size;
    if (!bytes || len < LZB_MIN_MATCH) return NULL;

    lzb_cdict *d = calloc(1, sizeof *d);
    if (!d) return NULL;

    d->data = (const uint8_t *)bytes;
    d->len = len;
    d->hlog = hlog_for(len);

    size_t hsize = (size_t)1 << d->hlog;
    d->head  = malloc(hsize * sizeof *d->head);
    d->chain = malloc(len * sizeof *d->chain);
    if (!d->head || !d->chain) { xq_lzb_cdict_free(d); return NULL; }

    memset(d->head, 0, hsize * sizeof *d->head);

    for (size_t i = 0; i + LZB_MIN_MATCH <= len; i++) {
        uint32_t h = hash4(read32(d->data + i), d->hlog);
        d->chain[i] = d->head[h];
        d->head[h] = (uint32_t)(i + 1);
    }
    return d;
}

void xq_lzb_cdict_free(void *p)
{
    lzb_cdict *d = (lzb_cdict *)p;
    if (!d) return;
    free(d->head);
    free(d->chain);
    free(d);
}

void *xq_lzb_ddict_new(const void *bytes, size_t len)
{
    lzb_ddict *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->data = (const uint8_t *)bytes;
    d->len = len;
    return d;
}

void xq_lzb_ddict_free(void *p) { free(p); }

void *xq_lzb_cctx_new(void) { return calloc(1, sizeof(lzb_cctx)); }

void xq_lzb_cctx_free(void *p)
{
    lzb_cctx *c = (lzb_cctx *)p;
    if (!c) return;
    free(c->head);
    free(c->chain);
    free(c);
}

static int cctx_prepare(lzb_cctx *c, size_t n)
{
    int hlog = hlog_for(n);
    if (!c->head || c->hlog != hlog) {
        free(c->head);
        c->head = malloc(((size_t)1 << hlog) * sizeof *c->head);
        if (!c->head) return 0;
        c->hlog = hlog;
    }
    if (c->chain_cap < n) {
        free(c->chain);
        c->chain = malloc(n * sizeof *c->chain);
        if (!c->chain) return 0;
        c->chain_cap = n;
    }
    memset(c->head, 0, ((size_t)1 << c->hlog) * sizeof *c->head);
    return 1;
}

static size_t match_len(const uint8_t *a, const uint8_t *b, size_t limit)
{
    size_t i = 0;
    while (i + 8 <= limit && xq_ld64le(a + i) == xq_ld64le(b + i)) i += 8;
    while (i < limit && a[i] == b[i]) i++;
    return i;
}

typedef struct {
    const uint8_t *src;
    size_t         len;
    lzb_cctx      *c;
    const lzb_cdict *d;
    int            depth;
} searcher;

static size_t find_match(const searcher *s, size_t pos, size_t *best_dist)
{
    size_t limit = s->len - pos;
    if (limit < LZB_MIN_MATCH) return 0;
    if (limit > 65535) limit = 65535;

    uint32_t v = read32(s->src + pos);
    size_t best = 0;
    size_t bestd = 0;
    int tries = s->depth;

    uint32_t h = hash4(v, s->c->hlog);
    uint32_t cand = s->c->head[h];
    while (cand && tries--) {
        size_t p = cand - 1;
        if (p >= pos) break;
        size_t m = match_len(s->src + p, s->src + pos, limit);
        if (m > best) { best = m; bestd = pos - p; if (m == limit) break; }
        cand = s->c->chain[p];
    }

    if (s->d && s->d->len >= LZB_MIN_MATCH && best < limit) {
        uint32_t dh = hash4(v, s->d->hlog);
        uint32_t dc = s->d->head[dh];
        int dtries = s->depth;
        while (dc && dtries--) {
            size_t p = dc - 1;
            size_t dlimit = s->d->len - p;
            if (dlimit > limit) dlimit = limit;
            size_t m = match_len(s->d->data + p, s->src + pos, dlimit);
            if (m > best) {
                best = m;
                bestd = pos + (s->d->len - p);
                if (m == limit) break;
            }
            dc = s->d->chain[p];
        }
    }

    if (best < LZB_MIN_MATCH) return 0;
    *best_dist = bestd;
    return best;
}

size_t xq_lzb_bound(size_t src_size)
{
    return src_size + src_size / 255 + 32;
}

static int emit_seq(uint8_t *dst, size_t cap, size_t *op,
                    const uint8_t *lits, size_t litlen,
                    size_t dist, size_t mlen)
{
    size_t litcode = litlen < 15 ? litlen : 15;
    size_t mcode;

    if (mlen == 0) mcode = 0;
    else {
        size_t adj = mlen - LZB_MIN_MATCH;
        mcode = (adj < 14) ? adj + 1 : 15;
    }

    if (*op >= cap) return 0;
    dst[(*op)++] = (uint8_t)((litcode << 4) | mcode);

    if (litcode == 15 && !put_varint(dst, cap, op, litlen - 15)) return 0;

    if (litlen) {
        if (*op + litlen > cap) return 0;
        memcpy(dst + *op, lits, litlen);
        *op += litlen;
    }

    if (mlen) {
        if (!put_varint(dst, cap, op, dist)) return 0;
        if (mcode == 15 && !put_varint(dst, cap, op, mlen - LZB_MIN_MATCH - 14)) return 0;
    }
    return 1;
}

xq_status xq_lzb_compress(void *cctxp, void *dstv, size_t dst_cap, size_t *out_len,
                          const void *srcv, size_t src_len, int level,
                          const void *cdictp)
{
    lzb_cctx *c = (lzb_cctx *)cctxp;
    if (!c) return XQ_ERR_INTERNAL;
    if (src_len == 0) { *out_len = 0; return XQ_OK; }

    const uint8_t *src = (const uint8_t *)srcv;
    uint8_t *dst = (uint8_t *)dstv;

    if (!cctx_prepare(c, src_len)) return XQ_ERR_OOM;

    searcher s;
    s.src = src;
    s.len = src_len;
    s.c = c;
    s.d = (const lzb_cdict *)cdictp;
    s.depth = depth_for(level);

    size_t op = 0;
    size_t anchor = 0;
    size_t pos = 0;

    while (pos + LZB_MIN_MATCH <= src_len) {
        size_t dist = 0;
        size_t m = find_match(&s, pos, &dist);

        if (m == 0) {
            uint32_t h = hash4(read32(src + pos), c->hlog);
            c->chain[pos] = c->head[h];
            c->head[h] = (uint32_t)(pos + 1);
            pos++;
            continue;
        }

        if (pos + 1 + LZB_MIN_MATCH <= src_len) {
            uint32_t h = hash4(read32(src + pos), c->hlog);
            c->chain[pos] = c->head[h];
            c->head[h] = (uint32_t)(pos + 1);

            size_t d2 = 0;
            size_t m2 = find_match(&s, pos + 1, &d2);
            if (m2 > m + 1) {
                pos++;
                m = m2;
                dist = d2;
            }
        }

        if (!emit_seq(dst, dst_cap, &op, src + anchor, pos - anchor, dist, m))
            return XQ_ERR_DST_TOO_SMALL;

        for (size_t i = pos; i < pos + m && i + LZB_MIN_MATCH <= src_len; i++) {
            uint32_t h = hash4(read32(src + i), c->hlog);
            c->chain[i] = c->head[h];
            c->head[h] = (uint32_t)(i + 1);
        }
        pos += m;
        anchor = pos;
    }

    if (!emit_seq(dst, dst_cap, &op, src + anchor, src_len - anchor, 0, 0))
        return XQ_ERR_DST_TOO_SMALL;

    *out_len = op;
    return XQ_OK;
}

xq_status xq_lzb_decompress(void *dctx, void *dstv, size_t dst_cap, size_t *out_len,
                            const void *srcv, size_t src_len, size_t expected_len,
                            const void *ddictp)
{
    (void)dctx; (void)expected_len;

    const uint8_t *src = (const uint8_t *)srcv;
    uint8_t *dst = (uint8_t *)dstv;
    const lzb_ddict *d = (const lzb_ddict *)ddictp;
    const uint8_t *dd = d ? d->data : NULL;
    size_t dlen = d ? d->len : 0;

    size_t ip = 0, op = 0;

    if (src_len == 0) { *out_len = 0; return XQ_OK; }

    while (ip < src_len) {
        uint8_t token = src[ip++];
        size_t litlen = token >> 4;
        size_t mcode  = token & 0x0F;

        if (litlen == 15) {
            uint64_t ext;
            if (!get_varint(src, src_len, &ip, &ext)) return XQ_ERR_CORRUPT_BLOCK;
            if (ext > SIZE_MAX - 15) return XQ_ERR_CORRUPT_BLOCK;
            litlen += (size_t)ext;
        }

        if (litlen) {
            if (litlen > src_len - ip) return XQ_ERR_CORRUPT_BLOCK;
            if (litlen > dst_cap - op) return XQ_ERR_DST_TOO_SMALL;
            memcpy(dst + op, src + ip, litlen);
            ip += litlen;
            op += litlen;
        }

        if (mcode == 0) break;

        uint64_t dist64;
        if (!get_varint(src, src_len, &ip, &dist64)) return XQ_ERR_CORRUPT_BLOCK;
        if (dist64 == 0) return XQ_ERR_CORRUPT_BLOCK;

        size_t mlen = mcode - 1 + LZB_MIN_MATCH;
        if (mcode == 15) {
            uint64_t ext;
            if (!get_varint(src, src_len, &ip, &ext)) return XQ_ERR_CORRUPT_BLOCK;
            if (ext > SIZE_MAX - mlen) return XQ_ERR_CORRUPT_BLOCK;
            mlen = LZB_MIN_MATCH + 14 + (size_t)ext;
        }

        if (dist64 > (uint64_t)op + dlen) return XQ_ERR_CORRUPT_BLOCK;
        size_t dist = (size_t)dist64;
        if (mlen > dst_cap - op) return XQ_ERR_DST_TOO_SMALL;

        if (dist > op) {
            size_t back = dist - op;
            if (back > dlen) return XQ_ERR_CORRUPT_BLOCK;
            size_t from_dict = back < mlen ? back : mlen;
            memcpy(dst + op, dd + (dlen - back), from_dict);
            op += from_dict;
            mlen -= from_dict;
            dist = op;
        }

        const uint8_t *m = dst + op - dist;
        for (size_t i = 0; i < mlen; i++) dst[op + i] = m[i];
        op += mlen;
    }

    *out_len = op;
    return XQ_OK;
}

const xq_codec_vt xq_codec_lzb_vt = {
    XQ_CODEC_LZB, "lzb",
    xq_lzb_bound,
    xq_lzb_cctx_new, xq_lzb_cctx_free,
    NULL, NULL,
    xq_lzb_cdict_new, xq_lzb_cdict_free,
    xq_lzb_ddict_new, xq_lzb_ddict_free,
    xq_lzb_compress, xq_lzb_decompress
};

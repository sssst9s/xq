/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "xq_huff.h"
#include "xq_bits.h"

void xq_bitw_init(xq_bitw *w, uint8_t *buf, size_t cap)
{
    w->buf = buf; w->cap = cap; w->pos = 0;
    w->acc = 0; w->nbits = 0; w->overflow = 0;
}

void xq_bitw_put(xq_bitw *w, uint32_t value, int bits)
{
    if (bits <= 0) return;
    w->acc |= (uint64_t)value << w->nbits;
    w->nbits += bits;
    while (w->nbits >= 8) {
        if (w->pos >= w->cap) { w->overflow = 1; w->nbits = 0; w->acc = 0; return; }
        w->buf[w->pos++] = (uint8_t)(w->acc & 0xFF);
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

int xq_bitw_flush(xq_bitw *w, size_t *out_len)
{
    while (w->nbits > 0) {
        if (w->pos >= w->cap) { w->overflow = 1; break; }
        w->buf[w->pos++] = (uint8_t)(w->acc & 0xFF);
        w->acc >>= 8;
        w->nbits -= 8;
    }
    if (w->overflow) return 0;
    *out_len = w->pos;
    return 1;
}

void xq_bitr_init(xq_bitr *r, const uint8_t *buf, size_t len)
{
    r->buf = buf; r->len = len; r->pos = 0;
    r->acc = 0; r->nbits = 0; r->bad = 0;
}

static void bitr_fill(xq_bitr *r)
{
    while (r->nbits <= 56) {
        if (r->pos >= r->len) break;
        r->acc |= (uint64_t)r->buf[r->pos++] << r->nbits;
        r->nbits += 8;
    }
}

int xq_huff_build(xq_huff_enc *e, const uint32_t *freq)
{
    memset(e, 0, sizeof *e);

    uint32_t f[XQ_HUFF_SYMBOLS];
    int used = 0;
    for (int i = 0; i < XQ_HUFF_SYMBOLS; i++) {
        f[i] = freq[i];
        if (f[i]) used++;
    }
    if (used == 0) return 0;

    if (used == 1) {
        for (int i = 0; i < XQ_HUFF_SYMBOLS; i++)
            if (f[i]) { e->len[i] = 1; e->code[i] = 0; }
        e->max_len = 1;
        e->used = 1;
        return 1;
    }

    for (int attempt = 0; attempt < 12; attempt++) {
        int    parent[2 * XQ_HUFF_SYMBOLS];
        uint64_t weight[2 * XQ_HUFF_SYMBOLS];
        int    node[XQ_HUFF_SYMBOLS];
        int    nnodes = 0;

        for (int i = 0; i < XQ_HUFF_SYMBOLS; i++) {
            if (!f[i]) continue;
            weight[nnodes] = f[i];
            parent[nnodes] = -1;
            node[nnodes] = i;
            nnodes++;
        }

        int live[XQ_HUFF_SYMBOLS];
        int nlive = nnodes;
        for (int i = 0; i < nnodes; i++) live[i] = i;

        int total = nnodes;
        while (nlive > 1) {
            int a = -1, b = -1;
            for (int i = 0; i < nlive; i++) {
                if (a < 0 || weight[live[i]] < weight[live[a]]) { b = a; a = i; }
                else if (b < 0 || weight[live[i]] < weight[live[b]]) b = i;
            }
            int na = live[a], nb = live[b];
            weight[total] = weight[na] + weight[nb];
            parent[total] = -1;
            parent[na] = total;
            parent[nb] = total;

            int hi = a > b ? a : b, lo = a < b ? a : b;
            live[lo] = total;
            live[hi] = live[nlive - 1];
            nlive--;
            total++;
        }

        int maxlen = 0;
        for (int i = 0; i < nnodes; i++) {
            int len = 0;
            int p = parent[i];
            while (p >= 0) { len++; p = parent[p]; }
            if (len > XQ_HUFF_MAX_BITS) { maxlen = len; break; }
            e->len[node[i]] = (uint8_t)len;
            if (len > maxlen) maxlen = len;
        }

        if (maxlen <= XQ_HUFF_MAX_BITS) {
            e->max_len = maxlen;
            e->used = used;
            break;
        }

        memset(e->len, 0, sizeof e->len);
        for (int i = 0; i < XQ_HUFF_SYMBOLS; i++)
            if (f[i]) f[i] = (f[i] >> 1) + 1;
    }

    if (e->max_len == 0) return 0;

    uint16_t next[XQ_HUFF_MAX_BITS + 1];
    uint16_t count[XQ_HUFF_MAX_BITS + 1];
    memset(count, 0, sizeof count);
    for (int i = 0; i < XQ_HUFF_SYMBOLS; i++) count[e->len[i]]++;
    count[0] = 0;

    uint16_t code = 0;
    for (int b = 1; b <= XQ_HUFF_MAX_BITS; b++) {
        code = (uint16_t)((code + count[b - 1]) << 1);
        next[b] = code;
    }
    for (int i = 0; i < XQ_HUFF_SYMBOLS; i++) {
        int l = e->len[i];
        if (!l) continue;
        uint16_t c = next[l]++;

        uint16_t rev = 0;
        for (int b = 0; b < l; b++) rev = (uint16_t)((rev << 1) | ((c >> b) & 1));
        e->code[i] = rev;
    }
    return 1;
}

void xq_huff_store(const xq_huff_enc *e, uint8_t *dst)
{
    for (int i = 0; i < XQ_HUFF_SYMBOLS; i += 2)
        dst[i / 2] = (uint8_t)(e->len[i] | (e->len[i + 1] << 4));
}

int xq_huff_load(xq_huff_dec *d, const uint8_t *src)
{
    uint8_t len[XQ_HUFF_SYMBOLS];
    int maxlen = 0, used = 0;

    for (int i = 0; i < XQ_HUFF_SYMBOLS; i += 2) {
        len[i]     = src[i / 2] & 0x0F;
        len[i + 1] = src[i / 2] >> 4;
    }
    for (int i = 0; i < XQ_HUFF_SYMBOLS; i++) {
        if (!len[i]) continue;
        used++;
        if (len[i] > maxlen) maxlen = len[i];
    }
    if (used == 0 || maxlen == 0 || maxlen > XQ_HUFF_MAX_BITS) return 0;

    uint16_t count[XQ_HUFF_MAX_BITS + 1];
    memset(count, 0, sizeof count);
    for (int i = 0; i < XQ_HUFF_SYMBOLS; i++) count[len[i]]++;
    count[0] = 0;

    if (used == 1) {
        if (maxlen != 1) return 0;
    } else {
        uint32_t avail = 1;
        for (int b = 1; b <= maxlen; b++) {
            avail <<= 1;
            if (count[b] > avail) return 0;
            avail -= count[b];
        }
        if (avail != 0) return 0;
    }

    uint16_t next[XQ_HUFF_MAX_BITS + 1];
    uint16_t code = 0;
    for (int b = 1; b <= XQ_HUFF_MAX_BITS; b++) {
        code = (uint16_t)((code + count[b - 1]) << 1);
        next[b] = code;
    }

    d->max_len = maxlen;
    size_t tsize = (size_t)1 << maxlen;
    memset(d->len, 0, tsize);

    for (int i = 0; i < XQ_HUFF_SYMBOLS; i++) {
        int l = len[i];
        if (!l) continue;
        uint16_t c = next[l]++;
        uint16_t rev = 0;
        for (int b = 0; b < l; b++) rev = (uint16_t)((rev << 1) | ((c >> b) & 1));

        for (size_t f = rev; f < tsize; f += ((size_t)1 << l)) {
            d->sym[f] = (uint16_t)i;
            d->len[f] = (uint8_t)l;
        }
    }
    return 1;
}

int xq_huff_decode(xq_bitr *r, const xq_huff_dec *d)
{
    if (r->nbits < XQ_HUFF_MAX_BITS) bitr_fill(r);
    uint32_t idx = (uint32_t)(r->acc & (((uint64_t)1 << d->max_len) - 1));
    uint8_t l = d->len[idx];
    if (l == 0 || l > r->nbits) { r->bad = 1; return -1; }
    r->acc >>= l;
    r->nbits -= l;
    return d->sym[idx];
}

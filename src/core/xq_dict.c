/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "xq_dict.h"
#include "xq_bits.h"
#include "xq_checked.h"
#include "xq_xxh64.h"

static void *dalloc(const xq_allocator *a, size_t n)
{
    return a ? a->alloc(a->ctx, n) : malloc(n);
}
static void dfree(const xq_allocator *a, void *p)
{
    if (!p) return;
    if (a) a->free(a->ctx, p); else free(p);
}

uint32_t xq_dict_size_for(uint64_t raw_size, uint32_t requested)
{
    if (requested == 0) return 0;
    if (requested > XQ_DICT_SIZE_MAX) requested = XQ_DICT_SIZE_MAX;

    uint64_t cap = raw_size / 4;
    if (requested > cap) requested = (uint32_t)cap;

    if (requested < 64u * 1024u) return 0;

    return requested;
}

xq_status xq_dict_build_sampled(xq_file *f, uint64_t size, uint32_t want,
                                const xq_allocator *a, xq_dict *out)
{
    if (!f || !out || want == 0) return XQ_ERR_PARAM;
    memset(out, 0, sizeof *out);
    if (!f->seekable) return XQ_ERR_IO;

    const uint32_t WIN = 64u * 1024u;

    uint32_t nsamp = want / WIN;
    if (nsamp == 0) nsamp = 1;

    if (size < (uint64_t)nsamp * WIN) {
        uint32_t n = (uint32_t)(size < want ? size : want);
        uint8_t *buf = dalloc(a, n);
        if (!buf) return XQ_ERR_OOM;
        size_t got = 0;
        xq_status st = xq_file_pread(f, buf, n, 0, &got);
        if (st != XQ_OK || got != n) { dfree(a, buf); return st == XQ_OK ? XQ_ERR_IO : st; }
        out->bytes = buf; out->len = n; out->kind = XQ_DICT_KIND_PREFIX; out->id = 1;
        return XQ_OK;
    }

    size_t total = (size_t)nsamp * WIN;
    uint8_t *buf = dalloc(a, total);
    if (!buf) return XQ_ERR_OOM;

    uint64_t span = size - WIN;
    for (uint32_t i = 0; i < nsamp; i++) {
        uint64_t off = (nsamp == 1) ? 0 : (span * i) / (nsamp - 1);
        size_t got = 0;
        xq_status st = xq_file_pread(f, buf + (size_t)i * WIN, WIN, off, &got);
        if (st != XQ_OK || got != WIN) { dfree(a, buf); return st == XQ_OK ? XQ_ERR_IO : st; }
    }

    out->bytes = buf;
    out->len = total;
    out->kind = XQ_DICT_KIND_SAMPLED;
    out->id = 1;
    return XQ_OK;
}

xq_status xq_dict_build_prefix(const void *src, size_t len, uint32_t want,
                               const xq_allocator *a, xq_dict *out)
{
    if (!out || want == 0) return XQ_ERR_PARAM;
    memset(out, 0, sizeof *out);

    size_t n = len < want ? len : want;
    if (n == 0) return XQ_ERR_PARAM;

    uint8_t *buf = dalloc(a, n);
    if (!buf) return XQ_ERR_OOM;
    memcpy(buf, src, n);

    out->bytes = buf;
    out->len = n;
    out->kind = XQ_DICT_KIND_PREFIX;
    out->id = 1;
    return XQ_OK;
}

void xq_dict_free(xq_dict *d, const xq_allocator *a)
{
    if (!d) return;
    if (!d->borrowed) dfree(a, d->bytes);
    memset(d, 0, sizeof *d);
}

size_t xq_dict_payload_size(size_t stored_len)
{
    return XQ_DICT_PAYLOAD_HEADER + stored_len;
}

void xq_dict_payload_store(uint8_t *buf, const xq_dict *d,
                           uint8_t stored_codec, size_t stored_len)
{
    xq_st8   (buf + 0, d->kind);
    xq_st8   (buf + 1, stored_codec);
    xq_st32le(buf + 2, d->id);
    xq_st64le(buf + 6, (uint64_t)d->len);
    xq_st64le(buf + 14, (uint64_t)stored_len);
    xq_st64le(buf + 22, xq_xxh64(d->bytes, d->len, 0));
}

xq_status xq_dict_payload_parse(const uint8_t *buf, size_t len, xq_dict_info *out)
{
    if (!buf || !out) return XQ_ERR_PARAM;
    if (len < XQ_DICT_PAYLOAD_HEADER) return XQ_ERR_CORRUPT_RECORD;

    out->kind         = xq_ld8(buf + 0);
    out->stored_codec = xq_ld8(buf + 1);
    out->id           = xq_ld32le(buf + 2);
    out->raw_size     = xq_ld64le(buf + 6);
    out->stored_size  = xq_ld64le(buf + 14);
    out->xxh64        = xq_ld64le(buf + 22);

    if (out->id == 0) return XQ_ERR_CORRUPT_RECORD;
    if (out->raw_size == 0 || out->raw_size > XQ_DICT_SIZE_MAX) return XQ_ERR_CORRUPT_RECORD;
    if (out->stored_size == 0) return XQ_ERR_CORRUPT_RECORD;
    if (out->stored_size != len - XQ_DICT_PAYLOAD_HEADER) return XQ_ERR_CORRUPT_RECORD;
    if (out->kind > XQ_DICT_KIND_TRAINED) return XQ_ERR_UNSUPPORTED_FEATURE;

    return XQ_OK;
}

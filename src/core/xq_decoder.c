/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "xq_decoder.h"
#include "xq_bits.h"
#include "xq_checked.h"
#include "xq_crc32c.h"
#include "xq_xxh64.h"
#include "xq_dict.h"

#define XQ_RBUF_SIZE (256u * 1024u)

struct xq_decoder {
    xq_src_fn    src;
    void        *src_ctx;
    xq_params    p;

    xq_file_header hdr;
    const xq_codec_vt *codec;
    void        *dctx;
    void        *ddict;
    uint8_t     *dict_bytes;
    size_t       checksum_len;
    uint32_t     max_block;

    uint8_t     *rbuf;
    size_t       rpos, rlen;
    int          src_eof;

    uint8_t     *raw;
    size_t       raw_cap, raw_len, raw_pos;
    uint8_t     *cbuf;
    size_t       cbuf_cap;

    uint64_t     raw_offset;
    xq_xxh64_state stream_ck;
    int          saw_eob;
    xq_status    sticky;
};

static void *emalloc(const xq_params *p, size_t n)
{
    if (p->alloc) return p->alloc->alloc(p->alloc->ctx, n);
    return malloc(n);
}
static void efree(const xq_params *p, void *ptr)
{
    if (!ptr) return;
    if (p->alloc) p->alloc->free(p->alloc->ctx, ptr);
    else free(ptr);
}

static xq_status rbuf_fill(xq_decoder *d)
{
    if (d->src_eof) return XQ_OK;

    if (d->rpos) {
        memmove(d->rbuf, d->rbuf + d->rpos, d->rlen - d->rpos);
        d->rlen -= d->rpos;
        d->rpos = 0;
    }
    size_t got = 0;
    xq_status st = d->src(d->src_ctx, d->rbuf + d->rlen, XQ_RBUF_SIZE - d->rlen, &got);
    if (st != XQ_OK) return st;
    if (got == 0) d->src_eof = 1;
    d->rlen += got;
    return XQ_OK;
}

static xq_status need(xq_decoder *d, size_t n, const uint8_t **out)
{
    if (n > XQ_RBUF_SIZE) return XQ_ERR_INTERNAL;
    while (d->rlen - d->rpos < n) {
        if (d->src_eof) return XQ_ERR_TRUNCATED;
        xq_status st = rbuf_fill(d);
        if (st != XQ_OK) return st;
    }
    *out = d->rbuf + d->rpos;
    return XQ_OK;
}

static void consume(xq_decoder *d, size_t n) { d->rpos += n; }

static xq_status read_exact(xq_decoder *d, uint8_t *dst, size_t n)
{
    size_t avail = d->rlen - d->rpos;
    size_t take = n < avail ? n : avail;
    if (take) {
        memcpy(dst, d->rbuf + d->rpos, take);
        d->rpos += take;
        dst += take;
        n -= take;
    }
    while (n) {
        if (d->src_eof) return XQ_ERR_TRUNCATED;
        size_t got = 0;
        xq_status st = d->src(d->src_ctx, dst, n, &got);
        if (st != XQ_OK) return st;
        if (got == 0) { d->src_eof = 1; return XQ_ERR_TRUNCATED; }
        dst += got;
        n -= got;
    }
    return XQ_OK;
}

static xq_status read_dict_record(xq_decoder *d, const uint8_t *rhdr,
                                  const xq_record_header *r);

static xq_status read_file_header(xq_decoder *d)
{
    const uint8_t *p;
    xq_status st = need(d, XQ_FILE_HEADER_SIZE, &p);
    if (st != XQ_OK) return st == XQ_ERR_TRUNCATED ? XQ_ERR_TRUNCATED : st;

    st = xq_fmt_header_parse(p, XQ_FILE_HEADER_SIZE, &d->hdr);
    if (st != XQ_OK) return st;
    consume(d, XQ_FILE_HEADER_SIZE);

    if (d->hdr.header_size > XQ_FILE_HEADER_SIZE) {
        size_t extra = d->hdr.header_size - XQ_FILE_HEADER_SIZE;
        const uint8_t *skip;
        if ((st = need(d, extra, &skip)) != XQ_OK) return st;
        consume(d, extra);
    }

    d->codec = xq_codec_get(d->hdr.codec_id);
    if (!d->codec) return XQ_ERR_UNSUPPORTED_CODEC;

    if (d->codec->dctx_new) {
        d->dctx = d->codec->dctx_new();
        if (!d->dctx) return XQ_ERR_OOM;
    }

    d->checksum_len = xq_fmt_checksum_size(d->hdr.checksum_id);

    uint32_t bs = d->hdr.block_size_log ? (1u << d->hdr.block_size_log)
                                        : XQ_BLOCK_SIZE_MAX;
    if (d->p.mem_limit && bs > d->p.mem_limit) return XQ_ERR_MEMLIMIT;
    if (bs > d->max_block) return XQ_ERR_MEMLIMIT;

    d->raw_cap  = bs;
    d->cbuf_cap = d->codec->bound(bs);
    d->raw  = emalloc(&d->p, d->raw_cap);
    d->cbuf = emalloc(&d->p, d->cbuf_cap);
    if (!d->raw || !d->cbuf) return XQ_ERR_OOM;

    return XQ_OK;
}

static xq_status read_dict_record(xq_decoder *d, const uint8_t *rhdr,
                                  const xq_record_header *r)
{
    if (d->ddict) return XQ_ERR_CORRUPT_RECORD;

    if (r->size < XQ_DICT_PAYLOAD_HEADER) return XQ_ERR_CORRUPT_RECORD;
    if (r->size > (uint64_t)XQ_DICT_SIZE_MAX + XQ_DICT_PAYLOAD_HEADER)
        return XQ_ERR_CORRUPT_RECORD;
    if (d->p.mem_limit && r->size > d->p.mem_limit) return XQ_ERR_MEMLIMIT;

    size_t plen = (size_t)r->size;
    uint8_t *payload = emalloc(&d->p, plen);
    if (!payload) return XQ_ERR_OOM;

    xq_status st = read_exact(d, payload, plen);
    if (st != XQ_OK) goto done;

    st = xq_fmt_record_verify(rhdr, payload, plen);
    if (st != XQ_OK) goto done;

    xq_dict_info info;
    if ((st = xq_dict_payload_parse(payload, plen, &info)) != XQ_OK) goto done;

    if (d->p.mem_limit && info.raw_size > d->p.mem_limit) { st = XQ_ERR_MEMLIMIT; goto done; }

    uint8_t *raw = emalloc(&d->p, (size_t)info.raw_size);
    if (!raw) { st = XQ_ERR_OOM; goto done; }

    const uint8_t *stored = payload + XQ_DICT_PAYLOAD_HEADER;
    if (info.stored_codec == XQ_CODEC_STORED) {
        if (info.stored_size != info.raw_size) { st = XQ_ERR_CORRUPT_RECORD; goto done_raw; }
        memcpy(raw, stored, (size_t)info.raw_size);
    } else {
        const xq_codec_vt *c = xq_codec_get(info.stored_codec);
        if (!c) { st = XQ_ERR_UNSUPPORTED_CODEC; goto done_raw; }
        size_t got = 0;
        st = c->decompress(d->dctx, raw, (size_t)info.raw_size, &got,
                           stored, (size_t)info.stored_size, (size_t)info.raw_size, NULL);
        if (st != XQ_OK) goto done_raw;
        if (got != info.raw_size) { st = XQ_ERR_CORRUPT_RECORD; goto done_raw; }
    }

    if (xq_xxh64(raw, (size_t)info.raw_size, 0) != info.xxh64) {
        st = XQ_ERR_CORRUPT_RECORD;
        goto done_raw;
    }

    if (!xq_codec_has_dict(d->codec)) { st = XQ_ERR_UNSUPPORTED_FEATURE; goto done_raw; }

    d->ddict = d->codec->ddict_new(raw, (size_t)info.raw_size);
    if (!d->ddict) { st = XQ_ERR_OOM; goto done_raw; }

    d->dict_bytes = raw;
    efree(&d->p, payload);
    return XQ_OK;

done_raw:
    efree(&d->p, raw);
done:
    efree(&d->p, payload);
    return st;
}

static xq_status handle_record(xq_decoder *d, const uint8_t *hdrbytes)
{
    xq_record_header r;
    xq_status st = xq_fmt_record_parse(hdrbytes, XQ_RECORD_HEADER_SIZE, &r);
    if (st != XQ_OK) return st;
    consume(d, XQ_RECORD_HEADER_SIZE);

    if (r.tag == XQ_REC_END_OF_BLOCKS) {
        if (r.size != 0) return XQ_ERR_CORRUPT_RECORD;
        st = xq_fmt_record_verify(hdrbytes, NULL, 0);
        if (st != XQ_OK) return st;
        d->saw_eob = 1;
        return XQ_OK;
    }

    if (r.tag == XQ_REC_DICT) return read_dict_record(d, hdrbytes, &r);

    if (r.tag != XQ_REC_USER_META && r.tag != XQ_REC_INDEX &&
        (r.rflags & XQ_RFLAG_CRITICAL))
        return XQ_ERR_UNSUPPORTED_FEATURE;

    uint64_t remain = r.size;
    while (remain) {
        if (d->rpos == d->rlen) {
            if (d->src_eof) return XQ_ERR_TRUNCATED;
            xq_status fs = rbuf_fill(d);
            if (fs != XQ_OK) return fs;
            if (d->rpos == d->rlen && d->src_eof) return XQ_ERR_TRUNCATED;
        }
        uint64_t avail = d->rlen - d->rpos;
        uint64_t take = remain < avail ? remain : avail;
        d->rpos += (size_t)take;
        remain -= take;
    }
    return XQ_OK;
}

static xq_status next_block(xq_decoder *d)
{
    for (;;) {
        if (d->saw_eob) return XQ_OK;

        const uint8_t *p;
        xq_status st = need(d, 2, &p);
        if (st == XQ_ERR_TRUNCATED) {

            return XQ_ERR_TRUNCATED;
        }
        if (st != XQ_OK) return st;

        if (xq_ld16le(p) != XQ_BLOCK_MAGIC) {
            const uint8_t *rh;
            if ((st = need(d, XQ_RECORD_HEADER_SIZE, &rh)) != XQ_OK) return st;
            if ((st = handle_record(d, rh)) != XQ_OK) return st;
            continue;
        }

        const uint8_t *bh;
        if ((st = need(d, XQ_BLOCK_HEADER_SIZE, &bh)) != XQ_OK) return st;

        xq_block_header b;
        if ((st = xq_fmt_block_parse(bh, XQ_BLOCK_HEADER_SIZE, &b)) != XQ_OK) return st;
        consume(d, XQ_BLOCK_HEADER_SIZE);

        if (b.raw_offset != d->raw_offset) return XQ_ERR_CORRUPT_BLOCK;
        if (b.raw_size > d->raw_cap) return XQ_ERR_MEMLIMIT;
        if (b.stored_size > d->cbuf_cap) return XQ_ERR_CORRUPT_BLOCK;

        if ((st = read_exact(d, d->cbuf, b.stored_size)) != XQ_OK) return st;

        if (d->checksum_len) {
            uint8_t ck[8];
            if ((st = read_exact(d, ck, d->checksum_len)) != XQ_OK) return st;
            if (d->hdr.checksum_id == XQ_CHECKSUM_CRC32C) {
                if (xq_ld32le(ck) != xq_crc32c(d->cbuf, b.stored_size))
                    return XQ_ERR_CORRUPT_BLOCK;
            } else {
                if (xq_ld64le(ck) != xq_xxh64(d->cbuf, b.stored_size, 0))
                    return XQ_ERR_CORRUPT_BLOCK;
            }
        }

        size_t out = 0;
        if (b.bflags & XQ_BFLAG_STORED) {
            if (b.stored_size != b.raw_size) return XQ_ERR_CORRUPT_BLOCK;
            memcpy(d->raw, d->cbuf, b.stored_size);
            out = b.stored_size;
        } else {
            const xq_codec_vt *c = xq_codec_get(b.codec_id);
            if (!c) return XQ_ERR_UNSUPPORTED_CODEC;

            if ((b.bflags & XQ_BFLAG_USES_DICT) && !d->ddict)
                return XQ_ERR_CORRUPT_BLOCK;

            st = c->decompress(d->dctx, d->raw, d->raw_cap, &out,
                               d->cbuf, b.stored_size, b.raw_size,
                               (b.bflags & XQ_BFLAG_USES_DICT) ? d->ddict : NULL);
            if (st != XQ_OK) return st;
        }

        if (out != b.raw_size) return XQ_ERR_CORRUPT_BLOCK;

        if (d->hdr.checksum_id != XQ_CHECKSUM_NONE)
            xq_xxh64_update(&d->stream_ck, d->raw, out);

        d->raw_len = out;
        d->raw_pos = 0;
        d->raw_offset += out;
        return XQ_OK;
    }
}

xq_decoder *xq_decoder_create(xq_src_fn src, void *src_ctx,
                              const xq_params *params, xq_status *out_st)
{
    xq_status st = XQ_OK;
    xq_params p = params ? *params : xq_params_default();

    if (!src) { st = XQ_ERR_PARAM; goto fail_early; }

    xq_decoder *d = emalloc(&p, sizeof *d);
    if (!d) { st = XQ_ERR_OOM; goto fail_early; }
    memset(d, 0, sizeof *d);

    d->src = src;
    d->src_ctx = src_ctx;
    d->p = p;
    d->max_block = XQ_BLOCK_SIZE_MAX;
    xq_xxh64_init(&d->stream_ck, 0);

    d->rbuf = emalloc(&p, XQ_RBUF_SIZE);
    if (!d->rbuf) { st = XQ_ERR_OOM; goto fail; }

    if ((st = read_file_header(d)) != XQ_OK) goto fail;

    if (out_st) *out_st = XQ_OK;
    return d;

fail:
    xq_decoder_free(d);
fail_early:
    if (out_st) *out_st = st;
    return NULL;
}

int64_t xq_decoder_read(xq_decoder *d, void *buf, size_t len)
{
    if (!d || (!buf && len)) return -(int64_t)XQ_ERR_PARAM;
    if (d->sticky != XQ_OK) return -(int64_t)d->sticky;

    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        if (d->raw_pos == d->raw_len) {
            if (d->saw_eob) break;
            xq_status st = next_block(d);
            if (st != XQ_OK) { d->sticky = st; return -(int64_t)st; }
            if (d->raw_pos == d->raw_len) break;
        }
        size_t avail = d->raw_len - d->raw_pos;
        size_t take = (len - done) < avail ? (len - done) : avail;
        memcpy(out + done, d->raw + d->raw_pos, take);
        d->raw_pos += take;
        done += take;
    }

    return (int64_t)done;
}

void xq_decoder_free(xq_decoder *d)
{
    if (!d) return;
    xq_params p = d->p;
    if (d->ddict && d->codec && d->codec->ddict_free) d->codec->ddict_free(d->ddict);
    if (d->dctx && d->codec && d->codec->dctx_free) d->codec->dctx_free(d->dctx);
    efree(&p, d->dict_bytes);
    efree(&p, d->rbuf);
    efree(&p, d->raw);
    efree(&p, d->cbuf);
    efree(&p, d);
}

uint64_t xq_decoder_raw_size(const xq_decoder *d)
{
    return d ? d->hdr.raw_size : 0;
}

uint64_t xq_decoder_stream_checksum(const xq_decoder *d)
{
    return d ? xq_xxh64_final(&d->stream_ck) : 0;
}

int xq_decoder_has_checksums(const xq_decoder *d)
{
    return d && d->hdr.checksum_id != XQ_CHECKSUM_NONE;
}

const xq_file_header *xq_decoder_header(const xq_decoder *d)
{
    return d ? &d->hdr : NULL;
}

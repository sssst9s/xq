/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "xq.h"
#include "xq_encoder.h"
#include "xq_decoder.h"
#include "xq_file.h"
#include "xq_checked.h"
#include "xq_dict.h"

typedef struct {
    uint8_t *dst;
    size_t   cap;
    size_t   len;
} mem_sink;

static xq_status mem_sink_write(void *ctx, const void *buf, size_t len)
{
    mem_sink *m = (mem_sink *)ctx;
    if (!xq_range_ok_sz(m->len, len, m->cap)) return XQ_ERR_DST_TOO_SMALL;
    memcpy(m->dst + m->len, buf, len);
    m->len += len;
    return XQ_OK;
}

typedef struct {
    const uint8_t *src;
    size_t         len;
    size_t         pos;
} mem_src;

static xq_status mem_src_read(void *ctx, void *buf, size_t len, size_t *got)
{
    mem_src *m = (mem_src *)ctx;
    size_t avail = m->len - m->pos;
    size_t take = len < avail ? len : avail;
    if (take) memcpy(buf, m->src + m->pos, take);
    m->pos += take;
    *got = take;
    return XQ_OK;
}

static xq_status file_sink_write(void *ctx, const void *buf, size_t len)
{
    return xq_file_write((xq_file *)ctx, buf, len);
}

static xq_status file_src_read(void *ctx, void *buf, size_t len, size_t *got)
{
    return xq_file_read((xq_file *)ctx, buf, len, got);
}

size_t xq_compress_bound(uint64_t src_size, const xq_params *params)
{
    xq_params p = params ? *params : xq_params_default();
    uint32_t bs = p.block_size ? p.block_size : XQ_BLOCK_SIZE_DEFAULT;

    uint64_t nblocks = (src_size + bs - 1) / bs;
    if (nblocks == 0) nblocks = 1;

    uint64_t per = (uint64_t)XQ_BLOCK_HEADER_SIZE + xq_fmt_checksum_size((uint8_t)p.checksum);

    uint64_t total = XQ_FILE_HEADER_SIZE;
    uint64_t tmp;
    if (!xq_mul_u64_ok(nblocks, per, &tmp)) return 0;
    if (!xq_add_u64_ok(total, tmp, &total)) return 0;
    if (!xq_add_u64_ok(total, src_size, &total)) return 0;
    if (!xq_add_u64_ok(total, XQ_RECORD_HEADER_SIZE, &total)) return 0;

    uint64_t idx = xq_fmt_index_payload_size(nblocks);
    if (idx == 0) return 0;
    if (!xq_add_u64_ok(total, idx + XQ_RECORD_HEADER_SIZE, &total)) return 0;
    if (!xq_add_u64_ok(total, XQ_FOOTER_SIZE, &total)) return 0;

    uint32_t dict_len = xq_dict_size_for(src_size, p.dict_size);
    if (dict_len && xq_codec_has_dict(xq_codec_get((uint8_t)p.codec))) {
        uint64_t drec = (uint64_t)XQ_RECORD_HEADER_SIZE + XQ_DICT_PAYLOAD_HEADER + dict_len;
        if (!xq_add_u64_ok(total, drec, &total)) return 0;
    }

    if (total > SIZE_MAX) return 0;
    return (size_t)total;
}

xq_status xq_compress(void *dst, size_t cap, size_t *out_len,
                      const void *src, size_t src_len, const xq_params *params)
{
    if (!dst || !out_len || (!src && src_len)) return XQ_ERR_PARAM;

    xq_params p = params ? *params : xq_params_default();

    xq_dict dict;
    memset(&dict, 0, sizeof dict);
    uint32_t want = xq_dict_size_for(src_len, p.dict_size);
    if (want && xq_codec_has_dict(xq_codec_get((uint8_t)p.codec))) {
        xq_status ds = xq_dict_build_prefix(src, src_len, want, p.alloc, &dict);
        if (ds != XQ_OK) return ds;
    }

    mem_sink m = { (uint8_t *)dst, cap, 0 };
    xq_status st;
    xq_encoder *e = xq_encoder_create(mem_sink_write, &m, &p, src_len,
                                      dict.len ? &dict : NULL, &st);
    if (!e) { xq_dict_free(&dict, p.alloc); return st; }

    st = xq_encoder_write(e, src, src_len);
    if (st == XQ_OK) st = xq_encoder_finish(e);
    xq_encoder_free(e);
    xq_dict_free(&dict, p.alloc);

    if (st != XQ_OK) return st;
    *out_len = m.len;
    return XQ_OK;
}

xq_status xq_decompress(void *dst, size_t cap, size_t *out_len,
                        const void *src, size_t src_len, uint64_t mem_limit)
{
    if (!dst || !out_len || (!src && src_len)) return XQ_ERR_PARAM;

    mem_src m = { (const uint8_t *)src, src_len, 0 };
    xq_params p = xq_params_default();
    p.mem_limit = mem_limit;

    xq_status st;
    xq_decoder *d = xq_decoder_create(mem_src_read, &m, &p, &st);
    if (!d) return st;

    size_t total = 0;
    for (;;) {
        if (total == cap) {

            uint8_t probe;
            int64_t n = xq_decoder_read(d, &probe, 1);
            if (n < 0) { st = (xq_status)(-n); break; }
            if (n == 0) { st = XQ_OK; break; }
            st = XQ_ERR_DST_TOO_SMALL;
            break;
        }
        int64_t n = xq_decoder_read(d, (uint8_t *)dst + total, cap - total);
        if (n < 0) { st = (xq_status)(-n); break; }
        if (n == 0) { st = XQ_OK; break; }
        total += (size_t)n;
    }

    xq_decoder_free(d);
    if (st != XQ_OK) return st;
    *out_len = total;
    return XQ_OK;
}

xq_status xq_compress_file(const char *in_path, const char *out_path,
                           const xq_params *params, xq_file_stats *stats)
{
    if (!in_path || !out_path) return XQ_ERR_PARAM;

    xq_file in, out;
    xq_status st = xq_file_open_read(&in, in_path);
    if (st != XQ_OK) return st;

    uint64_t hint = XQ_SIZE_UNKNOWN;
    if (in.seekable) {
        uint64_t sz;
        if (xq_file_size(&in, &sz) == XQ_OK) hint = sz;
    }

    st = xq_file_open_write(&out, out_path);
    if (st != XQ_OK) { xq_file_close(&in); return st; }

    xq_params pp = params ? *params : xq_params_default();
    xq_dict dict;
    memset(&dict, 0, sizeof dict);

    if (in.seekable && hint != XQ_SIZE_UNKNOWN) {
        uint32_t want = xq_dict_size_for(hint, pp.dict_size);
        if (want && xq_codec_has_dict(xq_codec_get((uint8_t)pp.codec))) {
            st = xq_dict_build_sampled(&in, hint, want, pp.alloc, &dict);
            if (st != XQ_OK) { xq_file_close(&in); xq_file_close(&out); return st; }
        }
    }

    xq_encoder *e = xq_encoder_create(file_sink_write, &out, &pp, hint,
                                      dict.len ? &dict : NULL, &st);
    if (!e) { xq_dict_free(&dict, pp.alloc); xq_file_close(&in); xq_file_close(&out); return st; }

    enum { IOBUF = 1u << 20 };
    static _Thread_local uint8_t buf[IOBUF];

    for (;;) {
        size_t got = 0;
        st = xq_file_read(&in, buf, sizeof buf, &got);
        if (st != XQ_OK) break;
        if (got == 0) break;
        st = xq_encoder_write(e, buf, got);
        if (st != XQ_OK) break;
    }
    if (st == XQ_OK) st = xq_encoder_finish(e);

    if (stats && st == XQ_OK) {
        stats->raw_size    = xq_encoder_raw_written(e);
        stats->stored_size = xq_encoder_file_offset(e);
    }

    xq_encoder_free(e);
    xq_dict_free(&dict, pp.alloc);
    xq_file_close(&in);
    xq_file_close(&out);
    return st;
}

xq_status xq_decompress_file(const char *in_path, const char *out_path,
                             uint64_t mem_limit, xq_file_stats *stats)
{
    if (!in_path || !out_path) return XQ_ERR_PARAM;

    xq_file in, out;
    xq_status st = xq_file_open_read(&in, in_path);
    if (st != XQ_OK) return st;

    st = xq_file_open_write(&out, out_path);
    if (st != XQ_OK) { xq_file_close(&in); return st; }

    xq_params p = xq_params_default();
    p.mem_limit = mem_limit;

    xq_decoder *d = xq_decoder_create(file_src_read, &in, &p, &st);
    if (!d) { xq_file_close(&in); xq_file_close(&out); return st; }

    enum { IOBUF = 1u << 20 };
    static _Thread_local uint8_t buf[IOBUF];
    uint64_t total = 0;

    for (;;) {
        int64_t n = xq_decoder_read(d, buf, sizeof buf);
        if (n < 0) { st = (xq_status)(-n); break; }
        if (n == 0) break;
        st = xq_file_write(&out, buf, (size_t)n);
        if (st != XQ_OK) break;
        total += (uint64_t)n;
    }

    if (st == XQ_OK && in.seekable && xq_decoder_has_checksums(d)) {
        uint64_t fsz = 0;
        if (xq_file_size(&in, &fsz) == XQ_OK && fsz >= XQ_FOOTER_SIZE) {
            uint8_t fbuf[XQ_FOOTER_SIZE];
            size_t got = 0;
            xq_footer ft;
            if (xq_file_pread(&in, fbuf, sizeof fbuf, fsz - XQ_FOOTER_SIZE, &got) == XQ_OK
                && got == sizeof fbuf
                && xq_fmt_footer_parse(fbuf, sizeof fbuf, &ft) == XQ_OK
                && ft.stream_checksum != 0
                && ft.stream_checksum != xq_decoder_stream_checksum(d))
                st = XQ_ERR_CORRUPT_BLOCK;
        }
    }

    if (stats && st == XQ_OK) {
        stats->raw_size = total;
        stats->stored_size = 0;
    }

    xq_decoder_free(d);
    xq_file_close(&in);
    xq_file_close(&out);
    return st;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "xq_repair.h"
#include "xq_bits.h"
#include "xq_checked.h"
#include "xq_crc32c.h"
#include "xq_xxh64.h"
#include "xq_dict.h"

#define XQ_SCAN_WINDOW (1u << 20)

typedef struct {
    xq_file       *f;
    uint64_t       file_size;
    xq_file_header hdr;
    int            have_hdr;

    const xq_codec_vt *codec;
    size_t         checksum_len;
    uint32_t       max_raw;

    void          *dctx;
    void          *ddict;
    uint8_t       *dict_bytes;

    uint8_t       *staging;
    size_t         staging_cap;
    uint8_t       *raw;
} repair_ctx;

static void *m(size_t n) { return malloc(n); }
static void  f_(void *p) { free(p); }

static xq_status read_header(repair_ctx *c)
{
    uint8_t buf[XQ_FILE_HEADER_SIZE];
    size_t got = 0;
    xq_status st = xq_file_pread(c->f, buf, sizeof buf, 0, &got);
    if (st != XQ_OK) return st;
    if (got != sizeof buf) return XQ_ERR_TRUNCATED;

    st = xq_fmt_header_parse(buf, sizeof buf, &c->hdr);
    if (st != XQ_OK) return st;

    c->codec = xq_codec_get(c->hdr.codec_id);
    if (!c->codec) return XQ_ERR_UNSUPPORTED_CODEC;
    c->checksum_len = xq_fmt_checksum_size(c->hdr.checksum_id);
    c->max_raw = c->hdr.block_size_log ? (1u << c->hdr.block_size_log)
                                       : XQ_BLOCK_SIZE_MAX;
    c->have_hdr = 1;
    return XQ_OK;
}

static xq_status load_dict(repair_ctx *c, uint64_t scan_from, uint64_t scan_to)
{
    if (!(c->hdr.flags & XQ_FLAG_DICT_PRESENT)) return XQ_OK;

    uint64_t off = scan_from;
    while (off + XQ_RECORD_HEADER_SIZE <= scan_to) {
        uint8_t rh[XQ_RECORD_HEADER_SIZE];
        size_t got = 0;
        if (xq_file_pread(c->f, rh, sizeof rh, off, &got) != XQ_OK || got != sizeof rh)
            return XQ_ERR_TRUNCATED;

        xq_record_header r;
        if (xq_fmt_record_parse(rh, sizeof rh, &r) != XQ_OK) return XQ_ERR_CORRUPT_RECORD;
        if (r.size > (uint64_t)XQ_DICT_SIZE_MAX + XQ_DICT_PAYLOAD_HEADER)
            return XQ_ERR_CORRUPT_RECORD;

        if (r.tag == XQ_REC_DICT) {
            size_t plen = (size_t)r.size;
            uint8_t *payload = m(plen);
            if (!payload) return XQ_ERR_OOM;
            if (xq_file_pread(c->f, payload, plen, off + XQ_RECORD_HEADER_SIZE, &got) != XQ_OK
                || got != plen) { f_(payload); return XQ_ERR_TRUNCATED; }

            xq_status st = xq_fmt_record_verify(rh, payload, plen);
            xq_dict_info info;
            if (st == XQ_OK) st = xq_dict_payload_parse(payload, plen, &info);
            if (st != XQ_OK) { f_(payload); return st; }

            uint8_t *raw = m((size_t)info.raw_size);
            if (!raw) { f_(payload); return XQ_ERR_OOM; }

            const uint8_t *stored = payload + XQ_DICT_PAYLOAD_HEADER;
            if (info.stored_codec == XQ_CODEC_STORED) {
                if (info.stored_size != info.raw_size) st = XQ_ERR_CORRUPT_RECORD;
                else memcpy(raw, stored, (size_t)info.raw_size);
            } else {
                const xq_codec_vt *cc = xq_codec_get(info.stored_codec);
                size_t produced = 0;
                if (!cc) st = XQ_ERR_UNSUPPORTED_CODEC;
                else st = cc->decompress(c->dctx, raw, (size_t)info.raw_size, &produced,
                                         stored, (size_t)info.stored_size,
                                         (size_t)info.raw_size, NULL);
                if (st == XQ_OK && produced != info.raw_size) st = XQ_ERR_CORRUPT_RECORD;
            }
            if (st == XQ_OK && xq_xxh64(raw, (size_t)info.raw_size, 0) != info.xxh64)
                st = XQ_ERR_CORRUPT_RECORD;
            if (st != XQ_OK) { f_(raw); f_(payload); return st; }

            if (!xq_codec_has_dict(c->codec)) { f_(raw); f_(payload); return XQ_ERR_UNSUPPORTED_FEATURE; }
            c->ddict = c->codec->ddict_new(raw, (size_t)info.raw_size);
            if (!c->ddict) { f_(raw); f_(payload); return XQ_ERR_OOM; }
            c->dict_bytes = raw;
            f_(payload);
            return XQ_OK;
        }

        if (!xq_add_u64_ok(off, XQ_RECORD_HEADER_SIZE + r.size, &off))
            return XQ_ERR_CORRUPT_RECORD;
    }
    return XQ_ERR_CORRUPT_RECORD;
}

static xq_status decode_at(repair_ctx *c, uint64_t off, xq_block_header *b, uint32_t *out_len)
{
    uint64_t total = (uint64_t)XQ_BLOCK_HEADER_SIZE + b->stored_size + c->checksum_len;
    if (total > c->staging_cap) return XQ_ERR_CORRUPT_BLOCK;
    if (!xq_range_ok(off, total, c->file_size)) return XQ_ERR_TRUNCATED;

    size_t got = 0;
    xq_status st = xq_file_pread(c->f, c->staging, (size_t)total, off, &got);
    if (st != XQ_OK) return st;
    if (got != total) return XQ_ERR_TRUNCATED;

    const uint8_t *payload = c->staging + XQ_BLOCK_HEADER_SIZE;

    if (c->checksum_len) {
        const uint8_t *ck = payload + b->stored_size;
        if (c->hdr.checksum_id == XQ_CHECKSUM_CRC32C) {
            if (xq_ld32le(ck) != xq_crc32c(payload, b->stored_size)) return XQ_ERR_CORRUPT_BLOCK;
        } else {
            if (xq_ld64le(ck) != xq_xxh64(payload, b->stored_size, 0)) return XQ_ERR_CORRUPT_BLOCK;
        }
    }

    size_t produced = 0;
    if (b->bflags & XQ_BFLAG_STORED) {
        if (b->stored_size != b->raw_size) return XQ_ERR_CORRUPT_BLOCK;
        memcpy(c->raw, payload, b->stored_size);
        produced = b->stored_size;
    } else {
        const xq_codec_vt *cc = xq_codec_get(b->codec_id);
        if (!cc) return XQ_ERR_UNSUPPORTED_CODEC;
        if ((b->bflags & XQ_BFLAG_USES_DICT) && !c->ddict) return XQ_ERR_CORRUPT_BLOCK;
        st = cc->decompress(c->dctx, c->raw, c->max_raw, &produced,
                            payload, b->stored_size, b->raw_size,
                            (b->bflags & XQ_BFLAG_USES_DICT) ? c->ddict : NULL);
        if (st != XQ_OK) return st;
    }
    if (produced != b->raw_size) return XQ_ERR_CORRUPT_BLOCK;

    *out_len = (uint32_t)produced;
    return XQ_OK;
}

xq_status xq_repair_file(const char *in_path, const char *out_path,
                         const xq_repair_opts *opts, xq_repair_report *rep)
{
    if (!in_path || !rep) return XQ_ERR_PARAM;
    memset(rep, 0, sizeof *rep);

    xq_repair_opts o;
    if (opts) o = *opts;
    else { o.fill_gaps = 0; o.fill_byte = 0; }

    repair_ctx c;
    memset(&c, 0, sizeof c);

    xq_file in, out;
    int have_out = 0;
    xq_status st = xq_file_open_read(&in, in_path);
    if (st != XQ_OK) return st;
    c.f = &in;

    if (!in.seekable) { xq_file_close(&in); return XQ_ERR_IO; }
    if ((st = xq_file_size(&in, &c.file_size)) != XQ_OK) goto done;
    if ((st = read_header(&c)) != XQ_OK) goto done;

    if (c.codec->dctx_new) {
        c.dctx = c.codec->dctx_new();
        if (!c.dctx) { st = XQ_ERR_OOM; goto done; }
    }

    c.staging_cap = XQ_BLOCK_HEADER_SIZE + c.codec->bound(c.max_raw) + 8;
    c.staging = m(c.staging_cap);
    c.raw = m(c.max_raw);
    if (!c.staging || !c.raw) { st = XQ_ERR_OOM; goto done; }

    uint64_t first_block = c.hdr.header_size;
    if (c.hdr.flags & XQ_FLAG_DICT_PRESENT) {
        xq_status ds = load_dict(&c, c.hdr.header_size, c.file_size);
        if (ds != XQ_OK) {
            rep->dict_ok = 0;

        } else {
            rep->dict_ok = 1;
        }
    } else {
        rep->dict_ok = 1;
    }

    if (out_path) {
        if ((st = xq_file_open_write(&out, out_path)) != XQ_OK) goto done;
        have_out = 1;
    }

    uint8_t *win = m(XQ_SCAN_WINDOW);
    if (!win) { st = XQ_ERR_OOM; goto done; }

    uint64_t pos = first_block;
    uint64_t expect_raw = 0;

    while (pos + XQ_BLOCK_HEADER_SIZE <= c.file_size) {
        size_t want = XQ_SCAN_WINDOW;
        if (pos + want > c.file_size) want = (size_t)(c.file_size - pos);
        size_t got = 0;
        if (xq_file_pread(&in, win, want, pos, &got) != XQ_OK || got < XQ_BLOCK_HEADER_SIZE) break;

        size_t i = 0;
        while (i + XQ_BLOCK_HEADER_SIZE <= got) {
            if (xq_ld16le(win + i) != XQ_BLOCK_MAGIC) { i++; continue; }

            xq_block_header b;
            if (xq_fmt_block_parse(win + i, XQ_BLOCK_HEADER_SIZE, &b) != XQ_OK) { i++; continue; }

            rep->blocks_found++;
            uint64_t boff = pos + i;
            uint32_t n = 0;
            xq_status ds = decode_at(&c, boff, &b, &n);

            if (ds != XQ_OK) {
                rep->blocks_bad++;
                i += XQ_BLOCK_HEADER_SIZE;
                continue;
            }

            if (b.raw_offset > expect_raw) {
                rep->gap_bytes += b.raw_offset - expect_raw;
                rep->gaps++;
                if (have_out && o.fill_gaps) {
                    static uint8_t fillbuf[65536];
                    memset(fillbuf, o.fill_byte, sizeof fillbuf);
                    uint64_t remain = b.raw_offset - expect_raw;
                    while (remain) {
                        size_t chunk = remain < sizeof fillbuf ? (size_t)remain : sizeof fillbuf;
                        if (xq_file_write(&out, fillbuf, chunk) != XQ_OK) break;
                        remain -= chunk;
                    }
                }
            }

            if (b.raw_offset >= expect_raw) {
                if (have_out && xq_file_write(&out, c.raw, n) != XQ_OK) { st = XQ_ERR_IO; break; }
                rep->bytes_recovered += n;
                expect_raw = b.raw_offset + n;
            } else {
                rep->blocks_skipped++;
            }

            rep->blocks_ok++;
            uint64_t adv = (uint64_t)XQ_BLOCK_HEADER_SIZE + b.stored_size + c.checksum_len;
            i += (size_t)adv;
        }

        if (got < XQ_BLOCK_HEADER_SIZE) break;

        size_t keep = XQ_BLOCK_HEADER_SIZE - 1;
        size_t step = (i > got - keep) ? i : got - keep;
        if (step == 0) step = 1;
        pos += step;
        if (got < XQ_SCAN_WINDOW && pos + XQ_BLOCK_HEADER_SIZE > c.file_size) break;
    }

    f_(win);
    rep->raw_size_expected = (c.hdr.raw_size == XQ_SIZE_UNKNOWN) ? 0 : c.hdr.raw_size;

done:
    if (have_out) xq_file_close(&out);
    if (c.ddict && c.codec && c.codec->ddict_free) c.codec->ddict_free(c.ddict);
    if (c.dctx && c.codec && c.codec->dctx_free) c.codec->dctx_free(c.dctx);
    f_(c.dict_bytes);
    f_(c.staging);
    f_(c.raw);
    xq_file_close(&in);

    if (st == XQ_OK && rep->blocks_bad) return XQ_ERR_CORRUPT_BLOCK;
    return st;
}

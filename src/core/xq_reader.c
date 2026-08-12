/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "xq_reader.h"
#include "xq_bits.h"
#include "xq_checked.h"
#include "xq_crc32c.h"
#include "xq_xxh64.h"
#include "xq_dict.h"
#include "xq_thread.h"

#define XQ_CACHE_BLOCKS_DEFAULT 16

typedef struct {
    uint64_t block;
    uint64_t used;
    uint32_t len;
    uint8_t *data;
} cache_entry;

#define XQ_NO_BLOCK UINT64_MAX

struct xq_reader {
    xq_file        f;
    int            owns_fd;
    xq_allocator   alloc;
    int            has_alloc;

    xq_file_header hdr;
    xq_footer      footer;
    uint64_t       file_size;

    xq_map         index_map;
    const uint8_t *entries;
    uint64_t       nentries;
    uint64_t       block_count;
    uint64_t       raw_size;

    const xq_codec_vt *codec;
    xq_mutex       cache_mu;
    xq_mutex       slot_mu;
    xq_cond        slot_cv;
    struct xq_dec_slot *slots;
    unsigned       nslots;
    int            mt_ready;
    void          *dctx;
    void          *ddict;
    uint8_t       *dict_bytes;
    size_t         dict_len;
    size_t         checksum_len;

    uint32_t       max_raw;
    uint8_t       *cbuf;
    size_t         cbuf_cap;

    cache_entry   *cache;
    unsigned       cache_n;
    uint64_t       clock;

    uint64_t       stat_hits, stat_misses;
};

struct xq_dec_slot {
    void    *dctx;
    uint8_t *cbuf;
    uint8_t *raw;
    int      busy;
};

static void *ralloc(xq_reader *r, size_t n)
{
    if (r->has_alloc) return r->alloc.alloc(r->alloc.ctx, n);
    return malloc(n);
}
static void rfree(xq_reader *r, void *p)
{
    if (!p) return;
    if (r->has_alloc) r->alloc.free(r->alloc.ctx, p);
    else free(p);
}

static xq_status load_tail(xq_reader *r)
{
    xq_status st;

    if ((st = xq_file_size(&r->f, &r->file_size)) != XQ_OK) return st;
    if (r->file_size < XQ_FILE_HEADER_SIZE + XQ_FOOTER_SIZE) return XQ_ERR_TRUNCATED;

    uint8_t hbuf[XQ_FILE_HEADER_SIZE];
    size_t got = 0;
    if ((st = xq_file_pread(&r->f, hbuf, sizeof hbuf, 0, &got)) != XQ_OK) return st;
    if (got != sizeof hbuf) return XQ_ERR_TRUNCATED;
    if ((st = xq_fmt_header_parse(hbuf, sizeof hbuf, &r->hdr)) != XQ_OK) return st;

    uint8_t fbuf[XQ_FOOTER_SIZE];
    if ((st = xq_file_pread(&r->f, fbuf, sizeof fbuf, r->file_size - XQ_FOOTER_SIZE, &got)) != XQ_OK)
        return st;
    if (got != sizeof fbuf) return XQ_ERR_TRUNCATED;
    if ((st = xq_fmt_footer_parse(fbuf, sizeof fbuf, &r->footer)) != XQ_OK) return st;

    if (r->footer.index_offset == 0) return XQ_ERR_CORRUPT_INDEX;

    if (!xq_range_ok(r->footer.index_offset, r->footer.index_size,
                     r->file_size - XQ_FOOTER_SIZE))
        return XQ_ERR_CORRUPT_INDEX;

    return XQ_OK;
}

static xq_status load_index(xq_reader *r)
{
    xq_status st;
    size_t got = 0;

    uint8_t rec[XQ_RECORD_HEADER_SIZE];
    if ((st = xq_file_pread(&r->f, rec, sizeof rec, r->footer.index_offset, &got)) != XQ_OK)
        return st;
    if (got != sizeof rec) return XQ_ERR_TRUNCATED;

    xq_record_header rh;
    if ((st = xq_fmt_record_parse(rec, sizeof rec, &rh)) != XQ_OK) return st;
    if (rh.tag != XQ_REC_INDEX) return XQ_ERR_CORRUPT_INDEX;
    if (rh.size != r->footer.index_size - XQ_RECORD_HEADER_SIZE) return XQ_ERR_CORRUPT_INDEX;
    if (rh.size < XQ_INDEX_PREAMBLE) return XQ_ERR_CORRUPT_INDEX;

    if (rh.size > SIZE_MAX) return XQ_ERR_CORRUPT_INDEX;
    st = xq_file_map(&r->f, r->footer.index_offset + XQ_RECORD_HEADER_SIZE,
                     (size_t)rh.size, &r->index_map);
    if (st != XQ_OK) return st;

    xq_index_preamble ip;
    st = xq_fmt_index_preamble_parse(r->index_map.base, (size_t)rh.size, &ip);
    if (st != XQ_OK) return st;

    uint64_t want = xq_fmt_index_payload_size(ip.block_count);
    if (want == 0 || want != rh.size) return XQ_ERR_CORRUPT_INDEX;

    uint32_t crc = xq_crc32c(rec, 10);
    crc = xq_crc32c_update(crc, r->index_map.base, (size_t)rh.size);
    if (crc != rh.crc32c) return XQ_ERR_CORRUPT_INDEX;

    r->entries     = r->index_map.base + XQ_INDEX_PREAMBLE;
    r->block_count = ip.block_count;
    r->nentries    = ip.block_count + 1;
    r->raw_size    = ip.total_raw;

    if ((st = xq_fmt_index_validate(&ip, r->entries, r->file_size)) != XQ_OK) return st;

    if (r->hdr.raw_size != XQ_SIZE_UNKNOWN && r->hdr.raw_size != r->raw_size)
        return XQ_ERR_CORRUPT_INDEX;

    return XQ_OK;
}

static xq_status load_meta(xq_reader *r, uint64_t mem_limit)
{
    xq_index_entry first;
    xq_fmt_index_entry_load(r->entries, &first);

    uint64_t start = r->hdr.header_size;
    if (first.file_offset < start) return XQ_ERR_CORRUPT_INDEX;
    uint64_t region = first.file_offset - start;
    if (region == 0) return XQ_OK;
    if (region > (uint64_t)XQ_DICT_SIZE_MAX + (1u << 20)) return XQ_ERR_CORRUPT_RECORD;
    if (mem_limit && region > mem_limit) return XQ_ERR_MEMLIMIT;

    xq_map m;
    xq_status st = xq_file_map(&r->f, start, (size_t)region, &m);
    if (st != XQ_OK) return st;

    const uint8_t *p = m.base;
    size_t remain = (size_t)region;

    while (remain >= XQ_RECORD_HEADER_SIZE) {
        xq_record_header rh;
        if ((st = xq_fmt_record_parse(p, remain, &rh)) != XQ_OK) break;

        size_t avail = remain - XQ_RECORD_HEADER_SIZE;
        if (rh.size > avail) { st = XQ_ERR_CORRUPT_RECORD; break; }

        const uint8_t *payload = p + XQ_RECORD_HEADER_SIZE;
        if ((st = xq_fmt_record_verify(p, payload, (size_t)rh.size)) != XQ_OK) break;

        if (rh.tag == XQ_REC_DICT) {
            xq_dict_info info;
            if ((st = xq_dict_payload_parse(payload, (size_t)rh.size, &info)) != XQ_OK) break;
            if (mem_limit && info.raw_size > mem_limit) { st = XQ_ERR_MEMLIMIT; break; }

            uint8_t *raw = ralloc(r, (size_t)info.raw_size);
            if (!raw) { st = XQ_ERR_OOM; break; }

            const uint8_t *stored = payload + XQ_DICT_PAYLOAD_HEADER;
            if (info.stored_codec == XQ_CODEC_STORED) {
                if (info.stored_size != info.raw_size) { rfree(r, raw); st = XQ_ERR_CORRUPT_RECORD; break; }
                memcpy(raw, stored, (size_t)info.raw_size);
            } else {
                const xq_codec_vt *c = xq_codec_get(info.stored_codec);
                if (!c) { rfree(r, raw); st = XQ_ERR_UNSUPPORTED_CODEC; break; }
                size_t got = 0;
                st = c->decompress(r->dctx, raw, (size_t)info.raw_size, &got,
                                   stored, (size_t)info.stored_size,
                                   (size_t)info.raw_size, NULL);
                if (st != XQ_OK || got != info.raw_size) {
                    rfree(r, raw);
                    if (st == XQ_OK) st = XQ_ERR_CORRUPT_RECORD;
                    break;
                }
            }

            if (xq_xxh64(raw, (size_t)info.raw_size, 0) != info.xxh64) {
                rfree(r, raw); st = XQ_ERR_CORRUPT_RECORD; break;
            }
            if (!xq_codec_has_dict(r->codec)) {
                rfree(r, raw); st = XQ_ERR_UNSUPPORTED_FEATURE; break;
            }

            r->ddict = r->codec->ddict_new(raw, (size_t)info.raw_size);
            if (!r->ddict) { rfree(r, raw); st = XQ_ERR_OOM; break; }
            r->dict_bytes = raw;
            r->dict_len = (size_t)info.raw_size;

        } else if ((rh.rflags & XQ_RFLAG_CRITICAL) && rh.tag != XQ_REC_END_OF_BLOCKS) {
            st = XQ_ERR_UNSUPPORTED_FEATURE;
            break;
        }

        size_t used = XQ_RECORD_HEADER_SIZE + (size_t)rh.size;
        p += used;
        remain -= used;
    }

    xq_file_unmap(&m);

    if (st == XQ_OK && (r->hdr.flags & XQ_FLAG_DICT_PRESENT) && !r->ddict)
        st = XQ_ERR_CORRUPT_RECORD;
    return st;
}

static xq_status setup_buffers(xq_reader *r, const xq_reader_opts *o)
{
    uint32_t bs = r->hdr.block_size_log ? (1u << r->hdr.block_size_log)
                                        : XQ_BLOCK_SIZE_MAX;

    unsigned ncache = (o && o->cache_blocks) ? o->cache_blocks : XQ_CACHE_BLOCKS_DEFAULT;

    uint64_t limit = o ? o->mem_limit : 0;
    if (limit) {

        uint64_t per = (uint64_t)bs;
        uint64_t overhead = per + XQ_BLOCK_HEADER_SIZE + r->codec->bound(bs) + 8;
        if (overhead > limit) return XQ_ERR_MEMLIMIT;
        uint64_t room = (limit - overhead) / per;
        if (room < 1) room = 1;
        if (room < ncache) ncache = (unsigned)room;
    }

    r->max_raw  = bs;

    r->cbuf_cap = XQ_BLOCK_HEADER_SIZE + r->codec->bound(bs) + 8;
    r->cbuf = ralloc(r, r->cbuf_cap);
    if (!r->cbuf) return XQ_ERR_OOM;

    r->cache = ralloc(r, ncache * sizeof *r->cache);
    if (!r->cache) return XQ_ERR_OOM;
    memset(r->cache, 0, ncache * sizeof *r->cache);

    for (unsigned i = 0; i < ncache; i++) {
        r->cache[i].block = XQ_NO_BLOCK;
        r->cache[i].data = ralloc(r, bs);
        if (!r->cache[i].data) return XQ_ERR_OOM;
    }
    r->cache_n = ncache;

    unsigned nslots = (o && o->threads > 0) ? (unsigned)o->threads : 0;
    if (nslots == 0) {
        int c = xq_cpu_count();
        nslots = (unsigned)(c > 4 ? 4 : (c > 0 ? c : 1));
    }
    if (limit) {
        uint64_t per = (uint64_t)r->cbuf_cap + bs;
        uint64_t room = per ? (limit / per) : 1;
        if (room < 1) room = 1;
        if (room < nslots) nslots = (unsigned)room;
    }

    r->slots = ralloc(r, nslots * sizeof *r->slots);
    if (!r->slots) return XQ_ERR_OOM;
    memset(r->slots, 0, nslots * sizeof *r->slots);
    for (unsigned i = 0; i < nslots; i++) {
        r->slots[i].cbuf = ralloc(r, r->cbuf_cap);
        r->slots[i].raw  = ralloc(r, bs);
        if (!r->slots[i].cbuf || !r->slots[i].raw) return XQ_ERR_OOM;
        if (r->codec->dctx_new) {
            r->slots[i].dctx = r->codec->dctx_new();
            if (!r->slots[i].dctx) return XQ_ERR_OOM;
        }
    }
    r->nslots = nslots;
    r->mt_ready = 1;
    return XQ_OK;
}

xq_reader *xq_reader_open(const char *path, const xq_reader_opts *opts, xq_status *out_st)
{
    xq_status st = XQ_OK;
    xq_reader *r = calloc(1, sizeof *r);
    if (!r) { if (out_st) *out_st = XQ_ERR_OOM; return NULL; }

    if (opts && opts->alloc) { r->alloc = *opts->alloc; r->has_alloc = 1; }

    if (xq_mutex_init(&r->cache_mu) != XQ_OK) { st = XQ_ERR_INTERNAL; goto fail; }
    if (xq_mutex_init(&r->slot_mu) != XQ_OK)  { st = XQ_ERR_INTERNAL; goto fail; }
    if (xq_cond_init(&r->slot_cv) != XQ_OK)   { st = XQ_ERR_INTERNAL; goto fail; }

    if ((st = xq_file_open_read(&r->f, path)) != XQ_OK) goto fail;
    r->owns_fd = 1;

    if (!r->f.seekable) { st = XQ_ERR_IO; goto fail; }

    if ((st = load_tail(r)) != XQ_OK) goto fail;

    r->codec = xq_codec_get(r->hdr.codec_id);
    if (!r->codec) { st = XQ_ERR_UNSUPPORTED_CODEC; goto fail; }
    r->checksum_len = xq_fmt_checksum_size(r->hdr.checksum_id);

    if (r->codec->dctx_new) {
        r->dctx = r->codec->dctx_new();
        if (!r->dctx) { st = XQ_ERR_OOM; goto fail; }
    }

    if ((st = load_index(r)) != XQ_OK) goto fail;
    if ((st = load_meta(r, opts ? opts->mem_limit : 0)) != XQ_OK) goto fail;
    if ((st = setup_buffers(r, opts)) != XQ_OK) goto fail;

    if (out_st) *out_st = XQ_OK;
    return r;

fail:
    xq_reader_close(r);
    if (out_st) *out_st = st;
    return NULL;
}

void xq_reader_close(xq_reader *r)
{
    if (!r) return;
    if (r->cache) {
        for (unsigned i = 0; i < r->cache_n; i++) rfree(r, r->cache[i].data);
        rfree(r, r->cache);
    }
    if (r->slots) {
        for (unsigned i = 0; i < r->nslots; i++) {
            if (r->slots[i].dctx && r->codec && r->codec->dctx_free)
                r->codec->dctx_free(r->slots[i].dctx);
            rfree(r, r->slots[i].cbuf);
            rfree(r, r->slots[i].raw);
        }
        rfree(r, r->slots);
    }
    xq_cond_destroy(&r->slot_cv);
    xq_mutex_destroy(&r->slot_mu);
    xq_mutex_destroy(&r->cache_mu);
    rfree(r, r->cbuf);
    if (r->ddict && r->codec && r->codec->ddict_free) r->codec->ddict_free(r->ddict);
    if (r->dctx && r->codec && r->codec->dctx_free) r->codec->dctx_free(r->dctx);
    rfree(r, r->dict_bytes);
    xq_file_unmap(&r->index_map);
    if (r->owns_fd) xq_file_close(&r->f);
    free(r);
}

static void entry_at(const xq_reader *r, uint64_t i, xq_index_entry *e)
{
    xq_fmt_index_entry_load(r->entries + i * XQ_INDEX_ENTRY_SIZE, e);
}

static xq_status decode_block_slot(xq_reader *r, struct xq_dec_slot *sl,
                                   uint64_t bi, uint8_t *dst, uint32_t *out_len)
{
    xq_index_entry cur, next;
    entry_at(r, bi, &cur);
    entry_at(r, bi + 1, &next);

    uint64_t extent = next.file_offset - cur.file_offset;
    if (extent < XQ_BLOCK_HEADER_SIZE) return XQ_ERR_CORRUPT_INDEX;

    if (extent > r->cbuf_cap) return XQ_ERR_CORRUPT_INDEX;

    uint8_t *staging = sl ? sl->cbuf : r->cbuf;
    void    *dctx    = sl ? sl->dctx : r->dctx;

    size_t got = 0;
    xq_status st = xq_file_pread(&r->f, staging, (size_t)extent, cur.file_offset, &got);
    if (st != XQ_OK) return st;
    if (got != extent) return XQ_ERR_TRUNCATED;

    const uint8_t *hdr = staging;
    xq_block_header b;
    if ((st = xq_fmt_block_parse(hdr, XQ_BLOCK_HEADER_SIZE, &b)) != XQ_OK) return st;

    if (b.raw_offset != cur.raw_offset) return XQ_ERR_CORRUPT_BLOCK;
    if (b.raw_size > r->max_raw) return XQ_ERR_MEMLIMIT;

    uint64_t need = (uint64_t)XQ_BLOCK_HEADER_SIZE + b.stored_size + r->checksum_len;
    if (need != extent) return XQ_ERR_CORRUPT_BLOCK;
    if (b.stored_size > r->cbuf_cap) return XQ_ERR_CORRUPT_BLOCK;

    const uint8_t *tmp = staging + XQ_BLOCK_HEADER_SIZE;

    if (r->checksum_len) {
        const uint8_t *ck = tmp + b.stored_size;
        if (r->hdr.checksum_id == XQ_CHECKSUM_CRC32C) {
            if (xq_ld32le(ck) != xq_crc32c(tmp, b.stored_size)) return XQ_ERR_CORRUPT_BLOCK;
        } else {
            if (xq_ld64le(ck) != xq_xxh64(tmp, b.stored_size, 0)) return XQ_ERR_CORRUPT_BLOCK;
        }
    }

    size_t produced = 0;
    if (b.bflags & XQ_BFLAG_STORED) {
        if (b.stored_size != b.raw_size) return XQ_ERR_CORRUPT_BLOCK;
        memcpy(dst, tmp, b.stored_size);
        produced = b.stored_size;
    } else {
        const xq_codec_vt *c = xq_codec_get(b.codec_id);
        if (!c) return XQ_ERR_UNSUPPORTED_CODEC;
        if ((b.bflags & XQ_BFLAG_USES_DICT) && !r->ddict) return XQ_ERR_CORRUPT_BLOCK;
        st = c->decompress(dctx, dst, r->max_raw, &produced, tmp, b.stored_size,
                           b.raw_size,
                           (b.bflags & XQ_BFLAG_USES_DICT) ? r->ddict : NULL);
        if (st != XQ_OK) return st;
    }
    if (produced != b.raw_size) return XQ_ERR_CORRUPT_BLOCK;

    *out_len = (uint32_t)produced;
    return XQ_OK;
}

static xq_status decode_block(xq_reader *r, uint64_t bi, uint8_t *dst, uint32_t *out_len)
{
    return decode_block_slot(r, NULL, bi, dst, out_len);
}

static struct xq_dec_slot *slot_acquire(xq_reader *r)
{
    if (!r->mt_ready) return NULL;
    xq_mutex_lock(&r->slot_mu);
    for (;;) {
        for (unsigned i = 0; i < r->nslots; i++) {
            if (!r->slots[i].busy) {
                r->slots[i].busy = 1;
                xq_mutex_unlock(&r->slot_mu);
                return &r->slots[i];
            }
        }
        xq_cond_wait(&r->slot_cv, &r->slot_mu);
    }
}

static void slot_release(xq_reader *r, struct xq_dec_slot *sl)
{
    if (!sl) return;
    xq_mutex_lock(&r->slot_mu);
    sl->busy = 0;
    xq_cond_signal(&r->slot_cv);
    xq_mutex_unlock(&r->slot_mu);
}

static xq_status block_copy(xq_reader *r, uint64_t bi, uint64_t within,
                            size_t take, uint8_t *dst, uint32_t *blen_out)
{
    xq_mutex_lock(&r->cache_mu);
    for (unsigned i = 0; i < r->cache_n; i++) {
        if (r->cache[i].block == bi) {
            r->cache[i].used = ++r->clock;
            uint32_t blen = r->cache[i].len;
            if (within < blen) {
                size_t avail = blen - (size_t)within;
                if (take > avail) take = avail;
                memcpy(dst, r->cache[i].data + within, take);
            }
            r->stat_hits++;
            xq_mutex_unlock(&r->cache_mu);
            *blen_out = blen;
            return XQ_OK;
        }
    }
    xq_mutex_unlock(&r->cache_mu);

    struct xq_dec_slot *sl = slot_acquire(r);
    uint8_t *scratch = sl ? sl->raw : NULL;
    if (!scratch) {
        xq_mutex_lock(&r->cache_mu);
        scratch = r->cache[0].data;
        xq_mutex_unlock(&r->cache_mu);
    }

    uint32_t n = 0;
    xq_status st = decode_block_slot(r, sl, bi, sl ? sl->raw : scratch, &n);
    if (st != XQ_OK) { slot_release(r, sl); return st; }

    xq_mutex_lock(&r->cache_mu);
    unsigned victim = 0;
    for (unsigned i = 1; i < r->cache_n; i++)
        if (r->cache[i].used < r->cache[victim].used) victim = i;

    memcpy(r->cache[victim].data, sl ? sl->raw : scratch, n);
    r->cache[victim].block = bi;
    r->cache[victim].len = n;
    r->cache[victim].used = ++r->clock;
    r->stat_misses++;

    if (within < n) {
        size_t avail = n - (size_t)within;
        if (take > avail) take = avail;
        memcpy(dst, r->cache[victim].data + within, take);
    }
    xq_mutex_unlock(&r->cache_mu);

    slot_release(r, sl);
    *blen_out = n;
    return XQ_OK;
}

int64_t xq_reader_pread(xq_reader *r, void *buf, size_t n, uint64_t off)
{
    if (!r || (!buf && n)) return -(int64_t)XQ_ERR_PARAM;
    if (n == 0) return 0;

    if (off >= r->raw_size) return 0;
    uint64_t avail = r->raw_size - off;
    if (n > avail) n = (size_t)avail;

    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;

    while (done < n) {
        int64_t bi = xq_fmt_index_find(r->entries, r->nentries, off + done);
        if (bi < 0) return -(int64_t)XQ_ERR_CORRUPT_INDEX;

        xq_index_entry e;
        entry_at(r, (uint64_t)bi, &e);
        uint64_t within = (off + done) - e.raw_offset;

        uint32_t blen = 0;
        xq_status st = block_copy(r, (uint64_t)bi, within, n - done, out + done, &blen);
        if (st != XQ_OK) return -(int64_t)st;
        if (within >= blen) return -(int64_t)XQ_ERR_CORRUPT_BLOCK;

        size_t took = blen - (size_t)within;
        if (took > n - done) took = n - done;
        done += took;
    }

    return (int64_t)done;
}

uint64_t xq_reader_size(const xq_reader *r)        { return r ? r->raw_size : 0; }
uint64_t xq_reader_block_count(const xq_reader *r) { return r ? r->block_count : 0; }
uint64_t xq_reader_stored_size(const xq_reader *r) { return r ? r->file_size : 0; }

uint32_t xq_reader_block_size(const xq_reader *r)
{
    return r ? r->max_raw : 0;
}

uint64_t xq_reader_dict_size(const xq_reader *r)
{
    return r ? (uint64_t)r->dict_len : 0;
}

void xq_reader_cache_stats(const xq_reader *r, uint64_t *hits, uint64_t *misses)
{
    if (!r) return;
    if (hits) *hits = r->stat_hits;
    if (misses) *misses = r->stat_misses;
}

const xq_file_header *xq_reader_header(const xq_reader *r)
{
    return r ? &r->hdr : NULL;
}

xq_status xq_reader_verify(xq_reader *r, xq_verify_report *rep)
{
    if (!r || !rep) return XQ_ERR_PARAM;
    memset(rep, 0, sizeof *rep);
    rep->blocks_total = r->block_count;
    rep->raw_size = r->raw_size;
    rep->stored_size = r->file_size;
    rep->first_bad_raw_offset = UINT64_MAX;
    rep->stream_checksum = XQ_STREAM_CK_ABSENT;

    rep->index_ok = 1;
    rep->footer_ok = 1;

    xq_xxh64_state ck;
    xq_xxh64_init(&ck, 0);
    int checksummable = (r->hdr.checksum_id != XQ_CHECKSUM_NONE)
                     && (r->footer.stream_checksum != 0);

    uint8_t *scratch = ralloc(r, r->max_raw);
    if (!scratch) return XQ_ERR_OOM;

    for (uint64_t i = 0; i < r->block_count; i++) {
        uint32_t n = 0;
        xq_status st = decode_block(r, i, scratch, &n);
        if (st == XQ_OK) {
            rep->blocks_ok++;
            rep->bytes_verified += n;
            if (checksummable) xq_xxh64_update(&ck, scratch, n);
        } else {

            rep->blocks_bad++;
            if (rep->first_bad_raw_offset == UINT64_MAX) {
                xq_index_entry e;
                entry_at(r, i, &e);
                rep->first_bad_raw_offset = e.raw_offset;
                rep->first_bad_status = st;
            }
            checksummable = 0;
        }
    }

    if (checksummable) {
        rep->stream_checksum = (xq_xxh64_final(&ck) == r->footer.stream_checksum)
                             ? XQ_STREAM_CK_OK : XQ_STREAM_CK_BAD;
    } else if (r->hdr.checksum_id != XQ_CHECKSUM_NONE && r->footer.stream_checksum != 0) {

        rep->stream_checksum = XQ_STREAM_CK_SKIP;
    }

    rfree(r, scratch);
    return rep->blocks_bad ? XQ_ERR_CORRUPT_BLOCK : XQ_OK;
}

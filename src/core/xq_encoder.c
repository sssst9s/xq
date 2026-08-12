/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "xq_encoder.h"
#include "xq_bits.h"
#include "xq_checked.h"
#include "xq_crc32c.h"
#include "xq_xxh64.h"
#include "xq_dict.h"
#include "xq_thread.h"

struct xq_encoder {
    xq_sink_fn   sink;
    void        *sink_ctx;

    xq_params    p;
    const xq_codec_vt *codec;
    void        *cctx;
    void        *cdict;
    xq_dict      dict;
    int          have_dict;
    uint32_t     block_size;
    uint8_t      block_size_log;
    size_t       checksum_len;

    uint8_t     *raw;
    size_t       raw_len;
    uint8_t     *cbuf;
    size_t       cbuf_cap;

    uint64_t     raw_offset;
    uint64_t     file_offset;

    xq_index_entry *idx;
    size_t       idx_n, idx_cap;

    xq_xxh64_state stream_ck;

    int          header_written;
    int          finished;
    xq_status    sticky;

    struct xq_enc_mt *mt;
    int          mt_cur;
};

enum { XQ_SLOT_FREE = 0, XQ_SLOT_READY, XQ_SLOT_DONE };

typedef struct {
    uint8_t  *raw;
    size_t    raw_len;
    uint8_t  *cbuf;
    size_t    cbuf_len;
    int       stored;
    int       state;
    uint8_t   ck[8];
    xq_status st;
} xq_enc_job;

struct xq_enc_mt {
    int          nthreads;
    xq_thread   *threads;
    void       **cctx;
    struct xq_enc_worker *warg;

    xq_mutex     mu;
    xq_cond      cv_work;
    xq_cond      cv_done;

    xq_enc_job  *jobs;
    int          nslots;

    uint64_t     fill_seq;
    uint64_t     work_seq;
    uint64_t     emit_seq;
    int          quit;

    xq_encoder  *e;
};

struct xq_enc_worker { struct xq_enc_mt *mt; int id; };

static xq_status emit_job(xq_encoder *e, xq_enc_job *j);

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

static xq_status emit(xq_encoder *e, const void *buf, size_t len)
{
    if (e->sticky != XQ_OK) return e->sticky;
    xq_status st = e->sink(e->sink_ctx, buf, len);
    if (st != XQ_OK) { e->sticky = st; return st; }
    e->file_offset += len;
    return XQ_OK;
}

static xq_status write_file_header(xq_encoder *e, uint64_t raw_size_hint)
{
    xq_file_header h;
    memset(&h, 0, sizeof h);
    h.format_major   = XQ_FORMAT_MAJOR;
    h.format_minor   = XQ_FORMAT_MINOR;
    h.header_size    = XQ_FILE_HEADER_SIZE;
    h.codec_id       = (uint8_t)e->p.codec;
    h.level          = (uint8_t)e->p.level;
    h.checksum_id    = (uint8_t)e->p.checksum;
    h.block_size_log = e->block_size_log;
    h.raw_size       = raw_size_hint;

    h.flags = XQ_FLAG_INDEX_EXPECTED | XQ_FLAG_UNIFORM_BLOCKS;
    if (raw_size_hint == XQ_SIZE_UNKNOWN) h.flags |= XQ_FLAG_STREAM_WRITTEN;
    if (e->have_dict) h.flags |= XQ_FLAG_DICT_PRESENT;

    uint8_t buf[XQ_FILE_HEADER_SIZE];
    xq_fmt_header_store(buf, &h);
    e->header_written = 1;
    return emit(e, buf, sizeof buf);
}

static xq_status idx_push(xq_encoder *e, uint64_t file_off, uint64_t raw_off)
{
    if (e->idx_n == e->idx_cap) {
        size_t ncap = e->idx_cap ? e->idx_cap * 2 : 1024;

        if (ncap > (SIZE_MAX / sizeof *e->idx)) return XQ_ERR_OOM;
        xq_index_entry *n = emalloc(&e->p, ncap * sizeof *n);
        if (!n) return XQ_ERR_OOM;
        if (e->idx_n) memcpy(n, e->idx, e->idx_n * sizeof *n);
        efree(&e->p, e->idx);
        e->idx = n;
        e->idx_cap = ncap;
    }
    e->idx[e->idx_n].file_offset = file_off;
    e->idx[e->idx_n].raw_offset  = raw_off;
    e->idx_n++;
    return XQ_OK;
}

static void job_checksum(xq_encoder *e, xq_enc_job *j)
{
    if (!e->checksum_len) return;
    const uint8_t *payload = j->stored ? j->raw : j->cbuf;
    size_t len = j->stored ? j->raw_len : j->cbuf_len;
    if (e->p.checksum == XQ_CHECKSUM_CRC32C)
        xq_st32le(j->ck, xq_crc32c(payload, len));
    else
        xq_st64le(j->ck, xq_xxh64(payload, len, 0));
}

static void compress_job(xq_encoder *e, xq_enc_job *j, void *cctx)
{
    size_t out = 0;
    xq_status st = e->codec->compress(cctx, j->cbuf, e->cbuf_cap, &out,
                                      j->raw, j->raw_len, e->p.level, e->cdict);

    if (st == XQ_ERR_DST_TOO_SMALL || (st == XQ_OK && out >= j->raw_len)) {
        j->stored = 1;
        j->cbuf_len = 0;
        j->st = XQ_OK;
    } else if (st != XQ_OK) {
        j->st = st;
    } else {
        j->stored = 0;
        j->cbuf_len = out;
        j->st = XQ_OK;
    }
    if (j->st == XQ_OK) job_checksum(e, j);
}

static xq_status emit_job(xq_encoder *e, xq_enc_job *j)
{
    if (e->sticky != XQ_OK) return e->sticky;
    if (j->st != XQ_OK) return (e->sticky = j->st);

    xq_status st = idx_push(e, e->file_offset, e->raw_offset);
    if (st != XQ_OK) return (e->sticky = st);

    const uint8_t *payload = j->stored ? j->raw : j->cbuf;
    size_t payload_len     = j->stored ? j->raw_len : j->cbuf_len;
    uint8_t bflags         = j->stored ? XQ_BFLAG_STORED : 0;

    if (e->cdict && !j->stored) bflags |= XQ_BFLAG_USES_DICT;

    xq_block_header b;
    b.bflags      = bflags;
    b.codec_id    = j->stored ? XQ_CODEC_STORED : (uint8_t)e->p.codec;
    b.stored_size = (uint32_t)payload_len;
    b.raw_size    = (uint32_t)j->raw_len;
    b.raw_offset  = e->raw_offset;

    uint8_t hdr[XQ_BLOCK_HEADER_SIZE];
    xq_fmt_block_store(hdr, &b);
    if ((st = emit(e, hdr, sizeof hdr)) != XQ_OK) return st;
    if ((st = emit(e, payload, payload_len)) != XQ_OK) return st;

    if (e->checksum_len && (st = emit(e, j->ck, e->checksum_len)) != XQ_OK) return st;

    if (e->p.checksum != XQ_CHECKSUM_NONE)
        xq_xxh64_update(&e->stream_ck, j->raw, j->raw_len);

    e->raw_offset += j->raw_len;
    return XQ_OK;
}

static void *mt_worker(void *arg)
{
    struct xq_enc_worker *w = (struct xq_enc_worker *)arg;
    struct xq_enc_mt *mt = w->mt;
    void *cctx = mt->cctx[w->id];

    for (;;) {
        xq_mutex_lock(&mt->mu);
        while (!mt->quit && mt->work_seq >= mt->fill_seq)
            xq_cond_wait(&mt->cv_work, &mt->mu);
        if (mt->quit && mt->work_seq >= mt->fill_seq) {
            xq_mutex_unlock(&mt->mu);
            return NULL;
        }
        uint64_t seq = mt->work_seq++;
        xq_mutex_unlock(&mt->mu);

        xq_enc_job *j = &mt->jobs[seq % (uint64_t)mt->nslots];
        compress_job(mt->e, j, cctx);

        xq_mutex_lock(&mt->mu);
        j->state = XQ_SLOT_DONE;
        xq_cond_broadcast(&mt->cv_done);
        xq_mutex_unlock(&mt->mu);
    }
}

static xq_status mt_emit_one(xq_encoder *e)
{
    struct xq_enc_mt *mt = e->mt;

    xq_mutex_lock(&mt->mu);
    if (mt->emit_seq >= mt->fill_seq) { xq_mutex_unlock(&mt->mu); return XQ_OK; }
    xq_enc_job *j = &mt->jobs[mt->emit_seq % (uint64_t)mt->nslots];
    while (j->state != XQ_SLOT_DONE)
        xq_cond_wait(&mt->cv_done, &mt->mu);
    xq_mutex_unlock(&mt->mu);

    xq_status st = emit_job(e, j);

    xq_mutex_lock(&mt->mu);
    j->state = XQ_SLOT_FREE;
    mt->emit_seq++;
    xq_cond_broadcast(&mt->cv_work);
    xq_mutex_unlock(&mt->mu);
    return st;
}

static xq_status mt_drain_all(xq_encoder *e)
{
    struct xq_enc_mt *mt = e->mt;
    for (;;) {
        xq_mutex_lock(&mt->mu);
        int more = mt->emit_seq < mt->fill_seq;
        xq_mutex_unlock(&mt->mu);
        if (!more) return XQ_OK;
        xq_status st = mt_emit_one(e);
        if (st != XQ_OK) return st;
    }
}

static xq_status mt_submit(xq_encoder *e, size_t raw_len)
{
    struct xq_enc_mt *mt = e->mt;

    xq_mutex_lock(&mt->mu);
    xq_enc_job *j = &mt->jobs[mt->fill_seq % (uint64_t)mt->nslots];
    j->raw_len = raw_len;
    j->state = XQ_SLOT_READY;
    mt->fill_seq++;
    xq_cond_signal(&mt->cv_work);
    xq_mutex_unlock(&mt->mu);

    for (;;) {
        xq_mutex_lock(&mt->mu);
        int full = (mt->fill_seq - mt->emit_seq) >= (uint64_t)mt->nslots;
        xq_mutex_unlock(&mt->mu);
        if (!full) break;
        xq_status st = mt_emit_one(e);
        if (st != XQ_OK) return st;
    }
    return XQ_OK;
}

static void mt_destroy(xq_encoder *e)
{
    struct xq_enc_mt *mt = e->mt;
    if (!mt) return;

    xq_mutex_lock(&mt->mu);
    mt->quit = 1;
    xq_cond_broadcast(&mt->cv_work);
    xq_mutex_unlock(&mt->mu);

    for (int i = 0; i < mt->nthreads; i++) xq_thread_join(&mt->threads[i]);

    for (int i = 0; i < mt->nthreads; i++)
        if (mt->cctx && mt->cctx[i] && e->codec->cctx_free) e->codec->cctx_free(mt->cctx[i]);
    if (mt->jobs)
        for (int i = 0; i < mt->nslots; i++) {
            efree(&e->p, mt->jobs[i].raw);
            efree(&e->p, mt->jobs[i].cbuf);
        }

    xq_cond_destroy(&mt->cv_work);
    xq_cond_destroy(&mt->cv_done);
    xq_mutex_destroy(&mt->mu);

    efree(&e->p, mt->jobs);
    efree(&e->p, mt->cctx);
    efree(&e->p, mt->warg);
    efree(&e->p, mt->threads);
    efree(&e->p, mt);
    e->mt = NULL;
}

static xq_status mt_create(xq_encoder *e, int nthreads, uint32_t bs)
{
    struct xq_enc_mt *mt = emalloc(&e->p, sizeof *mt);
    if (!mt) return XQ_ERR_OOM;
    memset(mt, 0, sizeof *mt);
    e->mt = mt;

    mt->e = e;
    mt->nthreads = nthreads;
    mt->nslots = nthreads * 2;

    if (xq_mutex_init(&mt->mu) != XQ_OK) return XQ_ERR_INTERNAL;
    if (xq_cond_init(&mt->cv_work) != XQ_OK) return XQ_ERR_INTERNAL;
    if (xq_cond_init(&mt->cv_done) != XQ_OK) return XQ_ERR_INTERNAL;

    mt->jobs    = emalloc(&e->p, (size_t)mt->nslots * sizeof *mt->jobs);
    mt->cctx    = emalloc(&e->p, (size_t)nthreads * sizeof *mt->cctx);
    mt->threads = emalloc(&e->p, (size_t)nthreads * sizeof *mt->threads);
    mt->warg    = emalloc(&e->p, (size_t)nthreads * sizeof *mt->warg);
    if (!mt->jobs || !mt->cctx || !mt->threads || !mt->warg) return XQ_ERR_OOM;
    memset(mt->jobs, 0, (size_t)mt->nslots * sizeof *mt->jobs);
    memset(mt->cctx, 0, (size_t)nthreads * sizeof *mt->cctx);
    memset(mt->threads, 0, (size_t)nthreads * sizeof *mt->threads);

    for (int i = 0; i < mt->nslots; i++) {
        mt->jobs[i].raw  = emalloc(&e->p, bs);
        mt->jobs[i].cbuf = emalloc(&e->p, e->cbuf_cap);
        if (!mt->jobs[i].raw || !mt->jobs[i].cbuf) return XQ_ERR_OOM;
    }
    for (int i = 0; i < nthreads; i++) {
        if (e->codec->cctx_new) {
            mt->cctx[i] = e->codec->cctx_new();
            if (!mt->cctx[i]) return XQ_ERR_OOM;
        }
        mt->warg[i].mt = mt;
        mt->warg[i].id = i;
    }
    for (int i = 0; i < nthreads; i++) {
        if (xq_thread_start(&mt->threads[i], mt_worker, &mt->warg[i]) != XQ_OK)
            return XQ_ERR_INTERNAL;
    }
    return XQ_OK;
}

static xq_status flush_block(xq_encoder *e)
{
    if (e->raw_len == 0) return XQ_OK;
    if (e->sticky != XQ_OK) return e->sticky;

    if (e->mt) {
        size_t n = e->raw_len;
        e->raw_len = 0;
        xq_status st = mt_submit(e, n);
        if (st != XQ_OK) return st;
        e->raw = e->mt->jobs[e->mt->fill_seq % (uint64_t)e->mt->nslots].raw;
        return XQ_OK;
    }

    xq_enc_job j;
    j.raw = e->raw; j.raw_len = e->raw_len;
    j.cbuf = e->cbuf; j.cbuf_len = 0;
    j.stored = 0; j.st = XQ_OK;
    compress_job(e, &j, e->cctx);
    xq_status st = emit_job(e, &j);
    e->raw_len = 0;
    return st;
}

static xq_status write_dict_record(xq_encoder *e)
{
    const xq_dict *d = &e->dict;

    size_t cap = e->codec->bound(d->len);
    uint8_t *tmp = emalloc(&e->p, cap);
    if (!tmp) return XQ_ERR_OOM;

    uint8_t  stored_codec = XQ_CODEC_STORED;
    const uint8_t *stored_bytes = d->bytes;
    size_t   stored_len = d->len;

    size_t out = 0;
    xq_status st = e->codec->compress(e->cctx, tmp, cap, &out,
                                      d->bytes, d->len, e->p.level, NULL);
    if (st == XQ_OK && out < d->len) {
        stored_codec = (uint8_t)e->p.codec;
        stored_bytes = tmp;
        stored_len = out;
    } else if (st != XQ_OK && st != XQ_ERR_DST_TOO_SMALL) {
        efree(&e->p, tmp);
        return st;
    }

    size_t payload_len = xq_dict_payload_size(stored_len);
    uint8_t head[XQ_DICT_PAYLOAD_HEADER];
    xq_dict_payload_store(head, d, stored_codec, stored_len);

    uint8_t rh[XQ_RECORD_HEADER_SIZE];
    xq_st8   (rh + 0, XQ_REC_DICT);
    xq_st8   (rh + 1, XQ_RFLAG_CRITICAL);
    xq_st64le(rh + 2, (uint64_t)payload_len);

    uint32_t crc = xq_crc32c(rh, 10);
    crc = xq_crc32c_update(crc, head, sizeof head);
    crc = xq_crc32c_update(crc, stored_bytes, stored_len);
    xq_st32le(rh + 10, crc);

    if ((st = emit(e, rh, sizeof rh)) == XQ_OK &&
        (st = emit(e, head, sizeof head)) == XQ_OK)
        st = emit(e, stored_bytes, stored_len);

    efree(&e->p, tmp);
    return st;
}

xq_encoder *xq_encoder_create(xq_sink_fn sink, void *sink_ctx,
                              const xq_params *params,
                              uint64_t raw_size_hint,
                              const xq_dict *dict,
                              xq_status *out_st)
{
    xq_status st;
    xq_params p = params ? *params : xq_params_default();

    const char *why = NULL;
    if ((st = xq_params_check(&p, &why)) != XQ_OK) goto fail_early;

    if (!sink) { st = XQ_ERR_PARAM; goto fail_early; }

    uint32_t bs = p.block_size ? p.block_size : XQ_BLOCK_SIZE_DEFAULT;

    const xq_codec_vt *codec = xq_codec_get((uint8_t)p.codec);
    if (!codec) { st = XQ_ERR_UNSUPPORTED_CODEC; goto fail_early; }

    xq_encoder *e = emalloc(&p, sizeof *e);
    if (!e) { st = XQ_ERR_OOM; goto fail_early; }
    memset(e, 0, sizeof *e);

    e->sink = sink;
    e->sink_ctx = sink_ctx;
    e->p = p;
    e->codec = codec;
    e->block_size = bs;
    e->checksum_len = xq_fmt_checksum_size((uint8_t)p.checksum);
    xq_xxh64_init(&e->stream_ck, 0);

    e->block_size_log = 0;
    while ((1u << e->block_size_log) != bs) e->block_size_log++;

    e->cbuf_cap = codec->bound(bs);
    e->raw  = emalloc(&p, bs);
    e->cbuf = emalloc(&p, e->cbuf_cap);
    if (!e->raw || !e->cbuf) { st = XQ_ERR_OOM; goto fail; }

    if (codec->cctx_new) {
        e->cctx = codec->cctx_new();
        if (!e->cctx) { st = XQ_ERR_OOM; goto fail; }
    }

    if (dict && dict->len) {

        if (!xq_codec_has_dict(codec)) { st = XQ_ERR_UNSUPPORTED_FEATURE; goto fail; }

        e->dict = *dict;
        e->dict.borrowed = 1;
        e->have_dict = 1;

        e->cdict = codec->cdict_new(dict->bytes, dict->len, p.level, bs);
        if (!e->cdict) { st = XQ_ERR_OOM; goto fail; }
    }

    int nthreads = p.threads;
    if (nthreads == 0) {
        nthreads = xq_cpu_count();
        if (nthreads > 8) nthreads = 8;
    }
    if (nthreads > 1 && codec->cctx_new) {
        uint64_t need = (uint64_t)nthreads * 2 * ((uint64_t)bs + e->cbuf_cap);
        if (p.mem_limit && need > p.mem_limit) {
            uint64_t per = (uint64_t)bs + e->cbuf_cap;
            nthreads = per ? (int)(p.mem_limit / per) : 1;
            if (nthreads < 1) nthreads = 1;
        }
    }
    if (nthreads > 1 && codec->cctx_new) {
        if ((st = mt_create(e, nthreads, bs)) != XQ_OK) goto fail;
        efree(&p, e->raw);
        e->raw = e->mt->jobs[0].raw;
        e->mt_cur = 0;
    }

    if ((st = write_file_header(e, raw_size_hint)) != XQ_OK) goto fail;
    if (e->have_dict && (st = write_dict_record(e)) != XQ_OK) goto fail;

    if (out_st) *out_st = XQ_OK;
    return e;

fail:
    xq_encoder_free(e);
fail_early:
    if (out_st) *out_st = st;
    return NULL;
}

xq_status xq_encoder_write(xq_encoder *e, const void *buf, size_t len)
{
    if (!e) return XQ_ERR_PARAM;
    if (e->sticky != XQ_OK) return e->sticky;
    if (e->finished) return XQ_ERR_PARAM;
    if (!buf && len) return XQ_ERR_PARAM;

    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        size_t room = e->block_size - e->raw_len;
        size_t take = len < room ? len : room;
        memcpy(e->raw + e->raw_len, p, take);
        e->raw_len += take;
        p += take;
        len -= take;
        if (e->raw_len == e->block_size) {
            xq_status st = flush_block(e);
            if (st != XQ_OK) return st;
        }
    }
    return XQ_OK;
}

xq_status xq_encoder_finish(xq_encoder *e)
{
    if (!e) return XQ_ERR_PARAM;
    if (e->sticky != XQ_OK) return e->sticky;
    if (e->finished) return XQ_OK;

    xq_status st = flush_block(e);
    if (st != XQ_OK) return st;

    if (e->mt) {
        st = mt_drain_all(e);
        if (st != XQ_OK) return st;
    }

    uint8_t rec[XQ_RECORD_HEADER_SIZE];
    xq_record_header eob = { XQ_REC_END_OF_BLOCKS, XQ_RFLAG_CRITICAL, 0, 0 };
    xq_fmt_record_store(rec, &eob, NULL);

    uint64_t index_offset = e->file_offset;
    if ((st = emit(e, rec, sizeof rec)) != XQ_OK) return st;

    if ((st = idx_push(e, index_offset, e->raw_offset)) != XQ_OK)
        return (e->sticky = st);

    uint64_t nblocks = e->idx_n - 1;
    uint64_t index_size = 0;

    if (nblocks > 0) {
        index_offset = e->file_offset;

        uint64_t payload = xq_fmt_index_payload_size(nblocks);
        if (payload == 0) return (e->sticky = XQ_ERR_INTERNAL);

        xq_record_header ir = { XQ_REC_INDEX, 0, payload, 0 };

        uint8_t pre[XQ_INDEX_PREAMBLE];
        xq_index_preamble ip;
        ip.block_count  = nblocks;
        ip.total_raw    = e->raw_offset;
        ip.total_stored = index_offset - XQ_FILE_HEADER_SIZE;
        xq_fmt_index_preamble_store(pre, &ip);

        enum { CHUNK = 256 };
        uint8_t ent[CHUNK * XQ_INDEX_ENTRY_SIZE];

        uint8_t ihdr[XQ_RECORD_HEADER_SIZE];
        xq_st8   (ihdr + 0, ir.tag);
        xq_st8   (ihdr + 1, ir.rflags);
        xq_st64le(ihdr + 2, ir.size);

        uint32_t crc = xq_crc32c(ihdr, 10);
        crc = xq_crc32c_update(crc, pre, sizeof pre);
        for (size_t i = 0; i < e->idx_n; i += CHUNK) {
            size_t n = e->idx_n - i < CHUNK ? e->idx_n - i : CHUNK;
            for (size_t j = 0; j < n; j++)
                xq_fmt_index_entry_store(ent + j * XQ_INDEX_ENTRY_SIZE, &e->idx[i + j]);
            crc = xq_crc32c_update(crc, ent, n * XQ_INDEX_ENTRY_SIZE);
        }
        xq_st32le(ihdr + 10, crc);

        if ((st = emit(e, ihdr, sizeof ihdr)) != XQ_OK) return st;
        if ((st = emit(e, pre, sizeof pre)) != XQ_OK) return st;
        for (size_t i = 0; i < e->idx_n; i += CHUNK) {
            size_t n = e->idx_n - i < CHUNK ? e->idx_n - i : CHUNK;
            for (size_t j = 0; j < n; j++)
                xq_fmt_index_entry_store(ent + j * XQ_INDEX_ENTRY_SIZE, &e->idx[i + j]);
            if ((st = emit(e, ent, n * XQ_INDEX_ENTRY_SIZE)) != XQ_OK) return st;
        }

        index_size = XQ_RECORD_HEADER_SIZE + payload;
    } else {
        index_offset = 0;
    }

    xq_footer f;
    f.index_offset    = index_offset;
    f.index_size      = index_size;

    f.stream_checksum = (e->p.checksum != XQ_CHECKSUM_NONE)
                      ? xq_xxh64_final(&e->stream_ck) : 0;

    uint8_t fbuf[XQ_FOOTER_SIZE];
    xq_fmt_footer_store(fbuf, &f);
    if ((st = emit(e, fbuf, sizeof fbuf)) != XQ_OK) return st;

    e->finished = 1;
    return XQ_OK;
}

void xq_encoder_free(xq_encoder *e)
{
    if (!e) return;
    xq_params p = e->p;
    if (e->mt) { e->raw = NULL; mt_destroy(e); }
    if (e->cdict && e->codec && e->codec->cdict_free) e->codec->cdict_free(e->cdict);
    if (e->cctx && e->codec && e->codec->cctx_free) e->codec->cctx_free(e->cctx);
    efree(&p, e->raw);
    efree(&p, e->cbuf);
    efree(&p, e->idx);
    efree(&p, e);
}

uint64_t xq_encoder_raw_written(const xq_encoder *e)
{
    return e ? e->raw_offset + e->raw_len : 0;
}

uint64_t xq_encoder_file_offset(const xq_encoder *e)
{
    return e ? e->file_offset : 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "xq_format.h"
#include "xq_bits.h"
#include "xq_checked.h"
#include "xq_crc32c.h"

const uint8_t xq_magic[XQ_MAGIC_SIZE] = {
    0x89, XQ_NAME_B0, XQ_NAME_B1, XQ_NAME_B2, 0x0D, 0x0A, 0x1A, 0x0A
};

void xq_fmt_header_store(uint8_t *buf, const xq_file_header *h)
{
    memcpy(buf, xq_magic, XQ_MAGIC_SIZE);
    xq_st8  (buf +  8, h->format_major);
    xq_st8  (buf +  9, h->format_minor);
    xq_st16le(buf + 10, h->header_size);
    xq_st32le(buf + 12, h->flags);
    xq_st8  (buf + 16, h->codec_id);
    xq_st8  (buf + 17, h->level);
    xq_st8  (buf + 18, h->checksum_id);
    xq_st8  (buf + 19, h->block_size_log);
    xq_st64le(buf + 20, h->raw_size);
    xq_st32le(buf + 28, xq_crc32c(buf, 28));
}

xq_status xq_fmt_header_parse(const uint8_t *buf, size_t len, xq_file_header *out)
{
    if (!buf || !out) return XQ_ERR_PARAM;
    if (len < XQ_FILE_HEADER_SIZE) return XQ_ERR_TRUNCATED;

    if (memcmp(buf, xq_magic, XQ_MAGIC_SIZE) != 0) return XQ_ERR_BAD_MAGIC;

    if (xq_ld32le(buf + 28) != xq_crc32c(buf, 28)) return XQ_ERR_CORRUPT_HEADER;

    xq_file_header h;
    h.format_major   = xq_ld8(buf + 8);
    h.format_minor   = xq_ld8(buf + 9);
    h.header_size    = xq_ld16le(buf + 10);
    h.flags          = xq_ld32le(buf + 12);
    h.codec_id       = xq_ld8(buf + 16);
    h.level          = xq_ld8(buf + 17);
    h.checksum_id    = xq_ld8(buf + 18);
    h.block_size_log = xq_ld8(buf + 19);
    h.raw_size       = xq_ld64le(buf + 20);

    if (h.format_major != XQ_FORMAT_MAJOR) return XQ_ERR_UNSUPPORTED_VERSION;

    if (h.header_size < XQ_FILE_HEADER_SIZE) return XQ_ERR_CORRUPT_HEADER;
    if (h.header_size > 4096) return XQ_ERR_CORRUPT_HEADER;

    if (h.checksum_id > XQ_CHECKSUM_MAX) return XQ_ERR_UNSUPPORTED_FEATURE;

    if (h.block_size_log != 0) {
        if (h.block_size_log < 12 || h.block_size_log > 28) return XQ_ERR_CORRUPT_HEADER;
        uint32_t bs = 1u << h.block_size_log;
        if (bs < XQ_BLOCK_SIZE_MIN || bs > XQ_BLOCK_SIZE_MAX) return XQ_ERR_CORRUPT_HEADER;
    }

    if (h.flags & ~XQ_FLAG_KNOWN_MASK) return XQ_ERR_UNSUPPORTED_FEATURE;

    if ((h.flags & XQ_FLAG_UNIFORM_BLOCKS) && h.block_size_log == 0)
        return XQ_ERR_CORRUPT_HEADER;

    *out = h;
    return XQ_OK;
}

void xq_fmt_record_store(uint8_t *buf, const xq_record_header *r, const uint8_t *payload)
{
    xq_st8  (buf + 0, r->tag);
    xq_st8  (buf + 1, r->rflags);
    xq_st64le(buf + 2, r->size);

    uint32_t crc = xq_crc32c(buf, 10);
    if (r->size) crc = xq_crc32c_update(crc, payload, (size_t)r->size);
    xq_st32le(buf + 10, crc);
}

xq_status xq_fmt_record_parse(const uint8_t *buf, size_t len, xq_record_header *out)
{
    if (!buf || !out) return XQ_ERR_PARAM;
    if (len < XQ_RECORD_HEADER_SIZE) return XQ_ERR_TRUNCATED;

    xq_record_header r;
    r.tag    = xq_ld8(buf + 0);
    r.rflags = xq_ld8(buf + 1);
    r.size   = xq_ld64le(buf + 2);
    r.crc32c = xq_ld32le(buf + 10);

    if (r.size > (uint64_t)1 << 40) return XQ_ERR_CORRUPT_RECORD;

    *out = r;
    return XQ_OK;
}

xq_status xq_fmt_record_verify(const uint8_t *hdr, const uint8_t *payload, size_t payload_len)
{
    if (!hdr) return XQ_ERR_PARAM;
    if (payload_len && !payload) return XQ_ERR_PARAM;

    uint64_t declared = xq_ld64le(hdr + 2);
    if (declared != (uint64_t)payload_len) return XQ_ERR_CORRUPT_RECORD;

    uint32_t crc = xq_crc32c(hdr, 10);
    if (payload_len) crc = xq_crc32c_update(crc, payload, payload_len);

    return crc == xq_ld32le(hdr + 10) ? XQ_OK : XQ_ERR_CORRUPT_RECORD;
}

void xq_fmt_block_store(uint8_t *buf, const xq_block_header *b)
{
    xq_st16le(buf + 0, XQ_BLOCK_MAGIC);
    xq_st8   (buf + 2, b->bflags);
    xq_st8   (buf + 3, b->codec_id);
    xq_st32le(buf + 4, b->stored_size);
    xq_st32le(buf + 8, b->raw_size);
    xq_st64le(buf + 12, b->raw_offset);
    xq_st32le(buf + 20, xq_crc32c(buf, 20));
}

xq_status xq_fmt_block_parse(const uint8_t *buf, size_t len, xq_block_header *out)
{
    if (!buf || !out) return XQ_ERR_PARAM;
    if (len < XQ_BLOCK_HEADER_SIZE) return XQ_ERR_TRUNCATED;

    if (xq_ld16le(buf) != XQ_BLOCK_MAGIC) return XQ_ERR_CORRUPT_BLOCK;

    if (xq_ld32le(buf + 20) != xq_crc32c(buf, 20)) return XQ_ERR_CORRUPT_BLOCK;

    xq_block_header b;
    b.bflags      = xq_ld8(buf + 2);
    b.codec_id    = xq_ld8(buf + 3);
    b.stored_size = xq_ld32le(buf + 4);
    b.raw_size    = xq_ld32le(buf + 8);
    b.raw_offset  = xq_ld64le(buf + 12);

    if (b.bflags & XQ_BFLAG_STORED) {
        if (b.stored_size != b.raw_size) return XQ_ERR_CORRUPT_BLOCK;
    }

    if (b.raw_size == 0) return XQ_ERR_CORRUPT_BLOCK;
    if (b.raw_size > XQ_BLOCK_SIZE_MAX) return XQ_ERR_CORRUPT_BLOCK;

    if (b.stored_size > b.raw_size + (b.raw_size / 8) + 1024) return XQ_ERR_CORRUPT_BLOCK;

    uint64_t end;
    if (!xq_add_u64_ok(b.raw_offset, b.raw_size, &end)) return XQ_ERR_CORRUPT_BLOCK;

    *out = b;
    return XQ_OK;
}

size_t xq_fmt_checksum_size(uint8_t checksum_id)
{
    switch (checksum_id) {
    case XQ_CHECKSUM_NONE:   return 0;
    case XQ_CHECKSUM_CRC32C: return 4;
    case XQ_CHECKSUM_XXH64:  return 8;
    default:                 return 0;
    }
}

void xq_fmt_index_preamble_store(uint8_t *buf, const xq_index_preamble *p)
{
    xq_st64le(buf + 0,  p->block_count);
    xq_st64le(buf + 8,  p->total_raw);
    xq_st64le(buf + 16, p->total_stored);
}

xq_status xq_fmt_index_preamble_parse(const uint8_t *buf, size_t len, xq_index_preamble *out)
{
    if (!buf || !out) return XQ_ERR_PARAM;
    if (len < XQ_INDEX_PREAMBLE) return XQ_ERR_TRUNCATED;

    out->block_count  = xq_ld64le(buf + 0);
    out->total_raw    = xq_ld64le(buf + 8);
    out->total_stored = xq_ld64le(buf + 16);

    if (xq_fmt_index_payload_size(out->block_count) == 0) return XQ_ERR_CORRUPT_INDEX;

    return XQ_OK;
}

void xq_fmt_index_entry_store(uint8_t *buf, const xq_index_entry *e)
{
    xq_st64le(buf + 0, e->file_offset);
    xq_st64le(buf + 8, e->raw_offset);
}

void xq_fmt_index_entry_load(const uint8_t *buf, xq_index_entry *out)
{
    out->file_offset = xq_ld64le(buf + 0);
    out->raw_offset  = xq_ld64le(buf + 8);
}

uint64_t xq_fmt_index_payload_size(uint64_t block_count)
{
    uint64_t n, bytes, total;
    if (!xq_add_u64_ok(block_count, 1, &n)) return 0;
    if (!xq_mul_u64_ok(n, XQ_INDEX_ENTRY_SIZE, &bytes)) return 0;
    if (!xq_add_u64_ok(bytes, XQ_INDEX_PREAMBLE, &total)) return 0;
    return total;
}

int64_t xq_fmt_index_find(const uint8_t *entries, uint64_t n, uint64_t raw_off)
{
    if (!entries || n < 2) return -1;

    if (raw_off >= xq_ld64le(entries + (n - 1) * XQ_INDEX_ENTRY_SIZE + 8)) return -1;

    uint64_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (xq_ld64le(entries + mid * XQ_INDEX_ENTRY_SIZE + 8) <= raw_off) lo = mid;
        else hi = mid;
    }
    return (int64_t)lo;
}

xq_status xq_fmt_index_validate(const xq_index_preamble *p, const uint8_t *entries,
                                uint64_t file_size)
{
    if (!p || !entries) return XQ_ERR_PARAM;
    if (p->block_count == 0) return XQ_ERR_CORRUPT_INDEX;

    uint64_t n = p->block_count + 1;

    xq_index_entry prev;
    xq_fmt_index_entry_load(entries, &prev);

    if (prev.file_offset < XQ_FILE_HEADER_SIZE) return XQ_ERR_CORRUPT_INDEX;
    if (prev.raw_offset != 0) return XQ_ERR_CORRUPT_INDEX;

    for (uint64_t i = 1; i < n; i++) {
        xq_index_entry cur;
        xq_fmt_index_entry_load(entries + i * XQ_INDEX_ENTRY_SIZE, &cur);

        if (cur.file_offset <= prev.file_offset) return XQ_ERR_CORRUPT_INDEX;
        if (cur.raw_offset  <= prev.raw_offset)  return XQ_ERR_CORRUPT_INDEX;

        if (cur.file_offset - prev.file_offset < XQ_BLOCK_HEADER_SIZE)
            return XQ_ERR_CORRUPT_INDEX;

        if (cur.raw_offset - prev.raw_offset > XQ_BLOCK_SIZE_MAX)
            return XQ_ERR_CORRUPT_INDEX;

        if (file_size && cur.file_offset > file_size) return XQ_ERR_CORRUPT_INDEX;

        prev = cur;
    }

    if (prev.raw_offset != p->total_raw) return XQ_ERR_CORRUPT_INDEX;

    return XQ_OK;
}

void xq_fmt_footer_store(uint8_t *buf, const xq_footer *f)
{
    xq_st64le(buf + 4,  f->index_offset);
    xq_st64le(buf + 12, f->index_size);
    xq_st64le(buf + 20, f->stream_checksum);
    xq_st32le(buf + 28, XQ_FOOTER_MAGIC);
    xq_st32le(buf + 0,  xq_crc32c(buf + 4, XQ_FOOTER_SIZE - 4));
}

xq_status xq_fmt_footer_parse(const uint8_t *buf, size_t len, xq_footer *out)
{
    if (!buf || !out) return XQ_ERR_PARAM;
    if (len < XQ_FOOTER_SIZE) return XQ_ERR_TRUNCATED;

    if (xq_ld32le(buf + 28) != XQ_FOOTER_MAGIC) return XQ_ERR_CORRUPT_FOOTER;
    if (xq_ld32le(buf + 0) != xq_crc32c(buf + 4, XQ_FOOTER_SIZE - 4))
        return XQ_ERR_CORRUPT_FOOTER;

    out->index_offset    = xq_ld64le(buf + 4);
    out->index_size      = xq_ld64le(buf + 12);
    out->stream_checksum = xq_ld64le(buf + 20);

    if (out->index_offset == 0 && out->index_size != 0) return XQ_ERR_CORRUPT_FOOTER;
    if (out->index_offset != 0 && out->index_size < XQ_RECORD_HEADER_SIZE + XQ_INDEX_PREAMBLE)
        return XQ_ERR_CORRUPT_FOOTER;

    if (out->index_offset != 0 && out->index_offset < XQ_FILE_HEADER_SIZE)
        return XQ_ERR_CORRUPT_FOOTER;

    return XQ_OK;
}

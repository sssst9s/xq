/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_bits.h"
#include "xq_crc32c.h"
#include "xq_checked.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do {                                        \
    checks++;                                                        \
    if (!(cond)) {                                                   \
        failures++;                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);         \
        fprintf(stderr, __VA_ARGS__);                                \
        fputc('\n', stderr);                                         \
    }                                                                \
} while (0)

#define CHECK_ST(expr, want) do {                                    \
    xq_status got_ = (expr);                                         \
    CHECK(got_ == (want), "%s: expected %s, got %s",                 \
          #expr, xq_strerror(want), xq_strerror(got_));              \
} while (0)

static void test_crc32c(void)
{
    CHECK(xq_crc32c("", 0) == 0x00000000U, "crc of empty");
    CHECK(xq_crc32c("a", 1) == 0xC1D04330U, "crc of \"a\" = %08X", xq_crc32c("a", 1));
    CHECK(xq_crc32c("123456789", 9) == 0xE3069283U,
          "crc check value = %08X", xq_crc32c("123456789", 9));

    uint8_t z[32]; memset(z, 0x00, sizeof z);
    uint8_t f[32]; memset(f, 0xFF, sizeof f);
    CHECK(xq_crc32c(z, 32) == 0x8A9136AAU, "crc of 32 zeros = %08X", xq_crc32c(z, 32));
    CHECK(xq_crc32c(f, 32) == 0x62A8AB43U, "crc of 32 ones = %08X", xq_crc32c(f, 32));

    const char *msg = "the quick brown fox jumps over the lazy dog";
    size_t n = strlen(msg);
    uint32_t once = xq_crc32c(msg, n);
    for (size_t split = 0; split <= n; split++) {
        uint32_t inc = xq_crc32c_update(0, msg, split);
        inc = xq_crc32c_update(inc, msg + split, n - split);
        CHECK(inc == once, "incremental crc differs at split %zu", split);
    }

    static uint8_t blob[600];
    for (size_t i = 0; i < sizeof blob; i++)
        blob[i] = (uint8_t)(i * 131u + (i >> 3));
    for (size_t off = 0; off < 16; off++) {
        for (size_t len = 0; len + off < sizeof blob; len++) {
            uint32_t hw = xq_crc32c_update(0, blob + off, len);
            uint32_t sw = xq_crc32c_portable(0, blob + off, len);
            CHECK(hw == sw, "crc hw/sw differ at off %zu len %zu: %08X vs %08X",
                  off, len, hw, sw);
            if (hw != sw) return;
        }
    }
}

static void test_checked(void)
{
    uint64_t out;
    CHECK(xq_add_u64_ok(1, 2, &out) && out == 3, "add ok");
    CHECK(!xq_add_u64_ok(UINT64_MAX, 1, &out), "add overflow detected");
    CHECK(xq_mul_u64_ok(1ull << 32, 2, &out) && out == (1ull << 33), "mul ok");
    CHECK(!xq_mul_u64_ok(UINT64_MAX, 2, &out), "mul overflow detected");

    CHECK(xq_range_ok(0, 10, 10), "range exact fit");
    CHECK(!xq_range_ok(1, 10, 10), "range past end");

    CHECK(!xq_range_ok(UINT64_MAX, 2, 100), "wrapping range rejected");
    CHECK(!xq_range_ok_sz(SIZE_MAX, 2, 100), "wrapping size_t range rejected");
}

static xq_file_header sample_header(void)
{
    xq_file_header h;
    h.format_major   = XQ_FORMAT_MAJOR;
    h.format_minor   = XQ_FORMAT_MINOR;
    h.header_size    = XQ_FILE_HEADER_SIZE;
    h.flags          = XQ_FLAG_INDEX_EXPECTED | XQ_FLAG_UNIFORM_BLOCKS;
    h.codec_id       = XQ_CODEC_STORED;
    h.level          = 6;
    h.checksum_id    = XQ_CHECKSUM_CRC32C;
    h.block_size_log = 16;
    h.raw_size       = 1234567890ull;
    return h;
}

static void test_header_roundtrip(void)
{
    uint8_t buf[XQ_FILE_HEADER_SIZE];
    xq_file_header in = sample_header(), out;

    xq_fmt_header_store(buf, &in);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_OK);

    CHECK(out.format_major == in.format_major, "major");
    CHECK(out.format_minor == in.format_minor, "minor");
    CHECK(out.header_size == in.header_size, "header_size");
    CHECK(out.flags == in.flags, "flags");
    CHECK(out.codec_id == in.codec_id, "codec");
    CHECK(out.level == in.level, "level");
    CHECK(out.checksum_id == in.checksum_id, "checksum");
    CHECK(out.block_size_log == in.block_size_log, "block_size_log");
    CHECK(out.raw_size == in.raw_size, "raw_size");

    static const uint8_t want[8] = {0x89,'X','Q','1',0x0D,0x0A,0x1A,0x0A};
    CHECK(memcmp(buf, want, 8) == 0, "magic bytes");

    in.raw_size = XQ_SIZE_UNKNOWN;
    in.flags |= XQ_FLAG_STREAM_WRITTEN;
    xq_fmt_header_store(buf, &in);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_OK);
    CHECK(out.raw_size == XQ_SIZE_UNKNOWN, "unknown size survives round trip");
}

static void test_header_malformed(void)
{
    uint8_t buf[XQ_FILE_HEADER_SIZE];
    xq_file_header h = sample_header(), out;
    xq_fmt_header_store(buf, &h);

    for (size_t n = 0; n < XQ_FILE_HEADER_SIZE; n++)
        CHECK_ST(xq_fmt_header_parse(buf, n, &out), XQ_ERR_TRUNCATED);

    for (int i = 0; i < XQ_MAGIC_SIZE; i++) {
        uint8_t bad[XQ_FILE_HEADER_SIZE];
        memcpy(bad, buf, sizeof bad);
        bad[i] ^= 0xFF;
        CHECK_ST(xq_fmt_header_parse(bad, sizeof bad, &out), XQ_ERR_BAD_MAGIC);
    }

    for (size_t byte = XQ_MAGIC_SIZE; byte < XQ_FILE_HEADER_SIZE; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[XQ_FILE_HEADER_SIZE];
            memcpy(bad, buf, sizeof bad);
            bad[byte] ^= (uint8_t)(1u << bit);
            xq_status st = xq_fmt_header_parse(bad, sizeof bad, &out);
            CHECK(st != XQ_OK, "bit flip at byte %zu bit %d accepted", byte, bit);
        }
    }

    h = sample_header(); h.format_major = XQ_FORMAT_MAJOR + 1;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_UNSUPPORTED_VERSION);

    h = sample_header(); h.format_minor = XQ_FORMAT_MINOR + 7;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_OK);

    h = sample_header(); h.block_size_log = 63;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_HEADER);

    h = sample_header(); h.block_size_log = 1;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_HEADER);

    h = sample_header(); h.block_size_log = 12;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_OK);
    h = sample_header(); h.block_size_log = 28;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_OK);
    h = sample_header(); h.block_size_log = 29;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_HEADER);

    h = sample_header(); h.flags |= 0x80000000u;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_UNSUPPORTED_FEATURE);

    h = sample_header(); h.block_size_log = 0; h.flags |= XQ_FLAG_UNIFORM_BLOCKS;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_HEADER);

    h = sample_header(); h.block_size_log = 0; h.flags &= ~(uint32_t)XQ_FLAG_UNIFORM_BLOCKS;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_OK);

    h = sample_header(); h.checksum_id = 99;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_UNSUPPORTED_FEATURE);

    h = sample_header(); h.header_size = 8;
    xq_fmt_header_store(buf, &h);
    CHECK_ST(xq_fmt_header_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_HEADER);
}

static void test_record(void)
{
    uint8_t hdr[XQ_RECORD_HEADER_SIZE];
    const char payload[] = "dictionary bytes would live here";
    size_t plen = sizeof payload - 1;

    xq_record_header r = { XQ_REC_DICT, XQ_RFLAG_CRITICAL, plen, 0 }, out;
    xq_fmt_record_store(hdr, &r, (const uint8_t *)payload);

    CHECK_ST(xq_fmt_record_parse(hdr, sizeof hdr, &out), XQ_OK);
    CHECK(out.tag == XQ_REC_DICT, "tag");
    CHECK(out.rflags == XQ_RFLAG_CRITICAL, "rflags");
    CHECK(out.size == plen, "size");
    CHECK_ST(xq_fmt_record_verify(hdr, (const uint8_t *)payload, plen), XQ_OK);

    for (size_t i = 0; i < plen; i++) {
        char bad[64];
        memcpy(bad, payload, plen);
        bad[i] ^= 0x01;
        CHECK_ST(xq_fmt_record_verify(hdr, (const uint8_t *)bad, plen),
                 XQ_ERR_CORRUPT_RECORD);
    }

    CHECK_ST(xq_fmt_record_verify(hdr, (const uint8_t *)payload, plen - 1),
             XQ_ERR_CORRUPT_RECORD);

    for (size_t byte = 0; byte < XQ_RECORD_HEADER_SIZE; byte++) {
        uint8_t bad[XQ_RECORD_HEADER_SIZE];
        memcpy(bad, hdr, sizeof bad);
        bad[byte] ^= 0x40;
        CHECK_ST(xq_fmt_record_verify(bad, (const uint8_t *)payload, plen),
                 XQ_ERR_CORRUPT_RECORD);
    }

    xq_record_header eob = { XQ_REC_END_OF_BLOCKS, 0, 0, 0 };
    xq_fmt_record_store(hdr, &eob, NULL);
    CHECK_ST(xq_fmt_record_parse(hdr, sizeof hdr, &out), XQ_OK);
    CHECK(out.size == 0, "eob size 0");
    CHECK_ST(xq_fmt_record_verify(hdr, NULL, 0), XQ_OK);

    for (size_t n = 0; n < XQ_RECORD_HEADER_SIZE; n++)
        CHECK_ST(xq_fmt_record_parse(hdr, n, &out), XQ_ERR_TRUNCATED);

    uint8_t huge[XQ_RECORD_HEADER_SIZE];
    memset(huge, 0, sizeof huge);
    xq_st8(huge + 0, XQ_REC_USER_META);
    xq_st8(huge + 1, 0);
    xq_st64le(huge + 2, UINT64_MAX);
    CHECK_ST(xq_fmt_record_parse(huge, sizeof huge, &out), XQ_ERR_CORRUPT_RECORD);

    xq_st64le(huge + 2, ((uint64_t)1 << 40) + 1);
    CHECK_ST(xq_fmt_record_parse(huge, sizeof huge, &out), XQ_ERR_CORRUPT_RECORD);
    xq_st64le(huge + 2, (uint64_t)1 << 40);
    CHECK_ST(xq_fmt_record_parse(huge, sizeof huge, &out), XQ_OK);
}

static void test_block(void)
{
    uint8_t buf[XQ_BLOCK_HEADER_SIZE];
    xq_block_header b = { 0, XQ_CODEC_STORED, 40000, 65536, 65536ull * 17 }, out;

    xq_fmt_block_store(buf, &b);
    CHECK_ST(xq_fmt_block_parse(buf, sizeof buf, &out), XQ_OK);
    CHECK(out.stored_size == b.stored_size, "stored_size");
    CHECK(out.raw_size == b.raw_size, "raw_size");
    CHECK(out.raw_offset == b.raw_offset, "raw_offset");
    CHECK(out.codec_id == b.codec_id, "codec_id");

    CHECK(xq_ld16le(buf) == XQ_BLOCK_MAGIC, "block magic present for resync");

    for (size_t byte = 0; byte < XQ_BLOCK_HEADER_SIZE; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[XQ_BLOCK_HEADER_SIZE];
            memcpy(bad, buf, sizeof bad);
            bad[byte] ^= (uint8_t)(1u << bit);
            xq_status st = xq_fmt_block_parse(bad, sizeof bad, &out);
            CHECK(st != XQ_OK, "block bit flip at byte %zu bit %d accepted", byte, bit);
        }
    }

    for (size_t n = 0; n < XQ_BLOCK_HEADER_SIZE; n++)
        CHECK_ST(xq_fmt_block_parse(buf, n, &out), XQ_ERR_TRUNCATED);

    b.bflags = XQ_BFLAG_STORED; b.stored_size = 65536; b.raw_size = 65536;
    xq_fmt_block_store(buf, &b);
    CHECK_ST(xq_fmt_block_parse(buf, sizeof buf, &out), XQ_OK);

    b.stored_size = 65535;
    xq_fmt_block_store(buf, &b);
    CHECK_ST(xq_fmt_block_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_BLOCK);

    b.bflags = 0; b.raw_size = 0; b.stored_size = 0;
    xq_fmt_block_store(buf, &b);
    CHECK_ST(xq_fmt_block_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_BLOCK);

    b.raw_size = XQ_BLOCK_SIZE_MAX + 1; b.stored_size = 16;
    xq_fmt_block_store(buf, &b);
    CHECK_ST(xq_fmt_block_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_BLOCK);

    b.raw_size = 1024; b.stored_size = 1000000;
    xq_fmt_block_store(buf, &b);
    CHECK_ST(xq_fmt_block_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_BLOCK);

    b.raw_size = 65536; b.stored_size = 4096; b.raw_offset = UINT64_MAX - 16;
    xq_fmt_block_store(buf, &b);
    CHECK_ST(xq_fmt_block_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_BLOCK);

    CHECK(xq_fmt_checksum_size(XQ_CHECKSUM_NONE) == 0, "checksum size none");
    CHECK(xq_fmt_checksum_size(XQ_CHECKSUM_CRC32C) == 4, "checksum size crc32c");
    CHECK(xq_fmt_checksum_size(XQ_CHECKSUM_XXH64) == 8, "checksum size xxh64");
}

#define NBLK 1000

static void build_index(uint8_t *entries, uint32_t blocksz, uint32_t storedsz)
{
    uint64_t fo = XQ_FILE_HEADER_SIZE, ro = 0;
    for (uint64_t i = 0; i <= NBLK; i++) {
        xq_index_entry e = { fo, ro };
        xq_fmt_index_entry_store(entries + i * XQ_INDEX_ENTRY_SIZE, &e);
        fo += XQ_BLOCK_HEADER_SIZE + storedsz + 4;
        ro += blocksz;
    }
}

static void test_index(void)
{
    static uint8_t entries[(NBLK + 1) * XQ_INDEX_ENTRY_SIZE];
    const uint32_t bs = 65536, ss = 40000;
    build_index(entries, bs, ss);

    for (uint64_t i = 0; i < NBLK; i++) {
        uint64_t base = i * (uint64_t)bs;
        CHECK(xq_fmt_index_find(entries, NBLK + 1, base) == (int64_t)i,
              "find at block %llu start", (unsigned long long)i);
        CHECK(xq_fmt_index_find(entries, NBLK + 1, base + bs - 1) == (int64_t)i,
              "find at block %llu end", (unsigned long long)i);
        CHECK(xq_fmt_index_find(entries, NBLK + 1, base + bs / 2) == (int64_t)i,
              "find at block %llu middle", (unsigned long long)i);
    }

    CHECK(xq_fmt_index_find(entries, NBLK + 1, (uint64_t)NBLK * bs) == -1, "find at EOF");
    CHECK(xq_fmt_index_find(entries, NBLK + 1, UINT64_MAX) == -1, "find past EOF");
    CHECK(xq_fmt_index_find(entries, 1, 0) == -1, "find with sentinel only");
    CHECK(xq_fmt_index_find(NULL, NBLK + 1, 0) == -1, "find with NULL");

    xq_index_preamble p = { NBLK, (uint64_t)NBLK * bs,
                            (uint64_t)NBLK * (XQ_BLOCK_HEADER_SIZE + ss + 4) };
    CHECK_ST(xq_fmt_index_validate(&p, entries, 0), XQ_OK);

    uint8_t pbuf[XQ_INDEX_PREAMBLE];
    xq_index_preamble pout;
    xq_fmt_index_preamble_store(pbuf, &p);
    CHECK_ST(xq_fmt_index_preamble_parse(pbuf, sizeof pbuf, &pout), XQ_OK);
    CHECK(pout.block_count == p.block_count, "preamble block_count");
    CHECK(pout.total_raw == p.total_raw, "preamble total_raw");
    CHECK(pout.total_stored == p.total_stored, "preamble total_stored");

    for (size_t n = 0; n < XQ_INDEX_PREAMBLE; n++)
        CHECK_ST(xq_fmt_index_preamble_parse(pbuf, n, &pout), XQ_ERR_TRUNCATED);

    xq_index_preamble bad = { UINT64_MAX, 0, 0 };
    xq_fmt_index_preamble_store(pbuf, &bad);
    CHECK_ST(xq_fmt_index_preamble_parse(pbuf, sizeof pbuf, &pout), XQ_ERR_CORRUPT_INDEX);
    CHECK(xq_fmt_index_payload_size(UINT64_MAX) == 0, "payload size overflow");
    CHECK(xq_fmt_index_payload_size(0) == XQ_INDEX_PREAMBLE + XQ_INDEX_ENTRY_SIZE,
          "payload size of empty index");

    {
        static uint8_t bent[(NBLK + 1) * XQ_INDEX_ENTRY_SIZE];
        memcpy(bent, entries, sizeof bent);
        xq_index_entry e = { XQ_FILE_HEADER_SIZE + 10, 0 };
        xq_fmt_index_entry_store(bent + 5 * XQ_INDEX_ENTRY_SIZE, &e);
        CHECK_ST(xq_fmt_index_validate(&p, bent, 0), XQ_ERR_CORRUPT_INDEX);
    }

    CHECK_ST(xq_fmt_index_validate(&p, entries, 1024), XQ_ERR_CORRUPT_INDEX);

    {
        xq_index_preamble q = p;
        q.total_raw += 1;
        CHECK_ST(xq_fmt_index_validate(&q, entries, 0), XQ_ERR_CORRUPT_INDEX);
    }

    {
        static uint8_t bent[(NBLK + 1) * XQ_INDEX_ENTRY_SIZE];
        memcpy(bent, entries, sizeof bent);
        xq_index_entry e;
        xq_fmt_index_entry_load(bent + XQ_INDEX_ENTRY_SIZE, &e);
        e.raw_offset = (uint64_t)XQ_BLOCK_SIZE_MAX + 4096;
        xq_fmt_index_entry_store(bent + XQ_INDEX_ENTRY_SIZE, &e);
        CHECK_ST(xq_fmt_index_validate(&p, bent, 0), XQ_ERR_CORRUPT_INDEX);
    }
}

static void test_footer(void)
{
    uint8_t buf[XQ_FOOTER_SIZE];
    xq_footer f = { 100000, 16024, 0x0123456789ABCDEFull }, out;

    xq_fmt_footer_store(buf, &f);
    CHECK_ST(xq_fmt_footer_parse(buf, sizeof buf, &out), XQ_OK);
    CHECK(out.index_offset == f.index_offset, "index_offset");
    CHECK(out.index_size == f.index_size, "index_size");
    CHECK(out.stream_checksum == f.stream_checksum, "stream_checksum");

    for (size_t byte = 0; byte < XQ_FOOTER_SIZE; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[XQ_FOOTER_SIZE];
            memcpy(bad, buf, sizeof bad);
            bad[byte] ^= (uint8_t)(1u << bit);
            xq_status st = xq_fmt_footer_parse(bad, sizeof bad, &out);
            CHECK(st != XQ_OK, "footer bit flip at byte %zu bit %d accepted", byte, bit);
        }
    }

    for (size_t n = 0; n < XQ_FOOTER_SIZE; n++)
        CHECK_ST(xq_fmt_footer_parse(buf, n, &out), XQ_ERR_TRUNCATED);

    f.index_offset = 0; f.index_size = 0;
    xq_fmt_footer_store(buf, &f);
    CHECK_ST(xq_fmt_footer_parse(buf, sizeof buf, &out), XQ_OK);

    f.index_offset = 0; f.index_size = 99;
    xq_fmt_footer_store(buf, &f);
    CHECK_ST(xq_fmt_footer_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_FOOTER);

    f.index_offset = 4; f.index_size = 1024;
    xq_fmt_footer_store(buf, &f);
    CHECK_ST(xq_fmt_footer_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_FOOTER);

    f.index_offset = 100000; f.index_size = 4;
    xq_fmt_footer_store(buf, &f);
    CHECK_ST(xq_fmt_footer_parse(buf, sizeof buf, &out), XQ_ERR_CORRUPT_FOOTER);

    uint8_t hbuf[XQ_FILE_HEADER_SIZE];
    xq_file_header h = sample_header();
    xq_fmt_header_store(hbuf, &h);
    CHECK_ST(xq_fmt_footer_parse(hbuf, sizeof hbuf, &out), XQ_ERR_CORRUPT_FOOTER);
}

static void test_params(void)
{
    const char *why = NULL;
    xq_params p = xq_params_default();
    CHECK_ST(xq_params_check(&p, &why), XQ_OK);
    CHECK(p.block_size == XQ_BLOCK_SIZE_DEFAULT, "default block size is 64 KiB");

    p = xq_params_default(); p.level = 0;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);
    p = xq_params_default(); p.level = 13;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);

    p = xq_params_default(); p.block_size = 1024;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);
    p = xq_params_default(); p.block_size = XQ_BLOCK_SIZE_MAX * 2;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);
    p = xq_params_default(); p.block_size = 100000;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);

    p = xq_params_default(); p.codec = (xq_codec)99;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);
    p = xq_params_default(); p.codec = XQ_CODEC_LZB;
    CHECK_ST(xq_params_check(&p, &why), XQ_OK);
    p = xq_params_default(); p.codec = XQ_CODEC_LZE;
    CHECK_ST(xq_params_check(&p, &why), XQ_OK);
    p = xq_params_default(); p.codec = (xq_codec)7;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);
    CHECK(xq_codec_available(XQ_CODEC_LZB), "lzb is built in");
    CHECK(xq_codec_available(XQ_CODEC_STORED), "stored is always available");

    p = xq_params_default(); p.mem_limit = 1024;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_MEMLIMIT);

    p = xq_params_default(); p.dict_size = 0;
    CHECK_ST(xq_params_check(&p, &why), XQ_OK);

    xq_allocator half = { NULL, NULL, NULL };
    p = xq_params_default(); p.alloc = &half;
    CHECK_ST(xq_params_check(&p, &why), XQ_ERR_PARAM);

    CHECK(xq_strerror(XQ_OK) != NULL, "strerror never NULL");
    CHECK(xq_strerror((xq_status)12345) != NULL, "strerror of unknown never NULL");
    CHECK(xq_status_is_corruption(XQ_ERR_CORRUPT_BLOCK), "corruption classified");
    CHECK(!xq_status_is_corruption(XQ_ERR_PARAM), "param error not corruption");
}

int main(void)
{
    test_crc32c();
    test_checked();
    test_header_roundtrip();
    test_header_malformed();
    test_record();
    test_block();
    test_index();
    test_footer();
    test_params();

    printf("%d checks, %d failures  (crc32c %s)\n", checks, failures,
           xq_crc32c_is_accelerated() ? "hardware" : "table");
    return failures ? 1 : 0;
}

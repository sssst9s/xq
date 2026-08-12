/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_bits.h"

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

static void fill_pattern(uint8_t *p, size_t n, uint64_t seed)
{
    uint64_t s = seed | 1;
    for (size_t i = 0; i < n; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        p[i] = (uint8_t)((i % 251) ^ (s >> 33));
    }
}

static void roundtrip_one(size_t n, uint32_t block_size, xq_checksum ck)
{
    uint8_t *src = n ? malloc(n) : malloc(1);
    if (!src) { fprintf(stderr, "oom\n"); exit(1); }
    fill_pattern(src, n, n * 2654435761u + block_size);

    xq_params p = xq_params_default();
    p.block_size = block_size;
    p.checksum = ck;

    size_t bound = xq_compress_bound(n, &p);
    CHECK(bound > 0, "bound is zero for n=%zu", n);

    uint8_t *packed = malloc(bound);
    if (!packed) { fprintf(stderr, "oom\n"); exit(1); }

    size_t packed_len = 0;
    xq_status st = xq_compress(packed, bound, &packed_len, src, n, &p);
    CHECK(st == XQ_OK, "compress n=%zu bs=%u: %s", n, block_size, xq_strerror(st));
    if (st != XQ_OK) { free(src); free(packed); return; }

    CHECK(packed_len <= bound, "packed %zu exceeds bound %zu", packed_len, bound);

    uint8_t *out = malloc(n ? n : 1);
    size_t out_len = 0;
    st = xq_decompress(out, n ? n : 1, &out_len, packed, packed_len, 0);
    CHECK(st == XQ_OK, "decompress n=%zu bs=%u: %s", n, block_size, xq_strerror(st));
    CHECK(out_len == n, "length n=%zu bs=%u: got %zu", n, block_size, out_len);
    if (st == XQ_OK && out_len == n)
        CHECK(memcmp(src, out, n) == 0, "content differs n=%zu bs=%u", n, block_size);

    free(src); free(packed); free(out);
}

static void test_roundtrip_sizes(void)
{
    const uint32_t bs = 4096;

    const size_t sizes[] = {
        0, 1, 2, 255, 256, 4095, 4096, 4097, 8191, 8192, 8193,
        4096 * 3, 4096 * 3 + 1, 100000
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        roundtrip_one(sizes[i], bs, XQ_CHECKSUM_CRC32C);
        roundtrip_one(sizes[i], bs, XQ_CHECKSUM_NONE);
    }

    for (uint32_t log = 12; log <= 20; log++)
        roundtrip_one(70000, 1u << log, XQ_CHECKSUM_CRC32C);
}

static void test_corruption(void)
{
    const size_t n = 40000;
    uint8_t *src = malloc(n);
    fill_pattern(src, n, 12345);

    xq_params p = xq_params_default();
    p.block_size = 4096;

    size_t bound = xq_compress_bound(n, &p);
    uint8_t *packed = malloc(bound);
    size_t packed_len = 0;
    xq_status st = xq_compress(packed, bound, &packed_len, src, n, &p);
    CHECK(st == XQ_OK, "setup compress: %s", xq_strerror(st));

    uint8_t *out = malloc(n);
    size_t out_len;

    st = xq_decompress(out, n, &out_len, packed, packed_len, 0);
    CHECK(st == XQ_OK && out_len == n, "baseline decompress");

    size_t data_end = packed_len;
    {
        xq_footer f;
        CHECK(xq_fmt_footer_parse(packed + packed_len - XQ_FOOTER_SIZE,
                                  XQ_FOOTER_SIZE, &f) == XQ_OK, "parse footer");
        data_end = (size_t)f.index_offset;
    }
    int undetected = 0;
    for (size_t i = 0; i < data_end; i++) {
        uint8_t save = packed[i];
        packed[i] ^= 0x01;

        size_t len2 = 0;
        xq_status s2 = xq_decompress(out, n, &len2, packed, packed_len, 0);
        int detected = (s2 != XQ_OK) || (len2 != n) || (memcmp(src, out, n) != 0);
        if (!detected) {
            undetected++;
            if (undetected < 4)
                fprintf(stderr, "  undetected corruption at byte %zu\n", i);
        }
        packed[i] = save;
    }
    CHECK(undetected == 0, "%d byte flips went undetected in the block region", undetected);

    int missed = 0, spurious = 0;
    for (size_t cut = 1; cut < packed_len; cut++) {
        size_t len2 = 0;
        xq_status s2 = xq_decompress(out, n, &len2, packed, cut, 0);
        int complete = (s2 == XQ_OK && len2 == n && memcmp(src, out, n) == 0);

        if (cut < data_end) {
            if (complete) missed++;
        } else {
            if (!complete) spurious++;
        }
    }
    CHECK(missed == 0, "%d truncations inside the block region decoded as complete", missed);
    CHECK(spurious == 0, "%d truncations after END_OF_BLOCKS failed to yield the data", spurious);

    free(src); free(packed); free(out);
}

static void test_api(void)
{
    uint8_t src[1000];
    fill_pattern(src, sizeof src, 7);

    xq_params p = xq_params_default();
    size_t bound = xq_compress_bound(sizeof src, &p);
    uint8_t *packed = malloc(bound);
    size_t packed_len = 0;

    CHECK(xq_compress(packed, bound, &packed_len, src, sizeof src, &p) == XQ_OK, "compress");

    uint8_t small[4];
    size_t got = 0;
    CHECK(xq_compress(small, sizeof small, &got, src, sizeof src, &p) == XQ_ERR_DST_TOO_SMALL,
          "compress into tiny buffer");

    uint8_t out[sizeof src];
    CHECK(xq_decompress(out, sizeof out, &got, packed, packed_len, 0) == XQ_OK, "decompress");
    CHECK(got == sizeof src, "decompressed length");

    CHECK(xq_decompress(out, sizeof src - 1, &got, packed, packed_len, 0) == XQ_ERR_DST_TOO_SMALL,
          "decompress into short buffer");

    uint8_t junk[64];
    memset(junk, 0xAB, sizeof junk);
    CHECK(xq_decompress(out, sizeof out, &got, junk, sizeof junk, 0) == XQ_ERR_BAD_MAGIC,
          "reject foreign data");

    size_t elen = 0;
    uint8_t ebuf[512];
    CHECK(xq_compress(ebuf, sizeof ebuf, &elen, "", 0, &p) == XQ_OK, "compress empty");
    CHECK(xq_decompress(out, sizeof out, &got, ebuf, elen, 0) == XQ_OK, "decompress empty");
    CHECK(got == 0, "empty decodes to zero bytes");

    CHECK(xq_decompress(out, sizeof out, &got, packed, packed_len, 1024) == XQ_ERR_MEMLIMIT,
          "memory limit enforced");

    CHECK(xq_compress(NULL, 10, &got, src, 10, &p) == XQ_ERR_PARAM, "NULL dst");
    CHECK(xq_decompress(out, sizeof out, NULL, packed, packed_len, 0) == XQ_ERR_PARAM, "NULL out_len");

    free(packed);
}

static void test_bomb(void)
{
    xq_file_header h;
    memset(&h, 0, sizeof h);
    h.format_major = XQ_FORMAT_MAJOR;
    h.format_minor = XQ_FORMAT_MINOR;
    h.header_size = XQ_FILE_HEADER_SIZE;
    h.flags = 0;
    h.codec_id = XQ_CODEC_STORED;
    h.checksum_id = XQ_CHECKSUM_CRC32C;
    h.block_size_log = 28;
    h.raw_size = XQ_SIZE_UNKNOWN;

    uint8_t buf[XQ_FILE_HEADER_SIZE];
    xq_fmt_header_store(buf, &h);

    uint8_t out[64];
    size_t got = 0;

    CHECK(xq_decompress(out, sizeof out, &got, buf, sizeof buf, 1u << 20) == XQ_ERR_MEMLIMIT,
          "256 MiB block refused under a 1 MiB limit");

    xq_status st = xq_decompress(out, sizeof out, &got, buf, sizeof buf, 0);
    CHECK(st == XQ_ERR_TRUNCATED, "header-only file is truncated, got %s", xq_strerror(st));
}

int main(void)
{
    test_roundtrip_sizes();
    test_corruption();
    test_api();
    test_bomb();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_reader.h"

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

static const char *TMP = "/tmp/.xq_test_reader.xq";
static const char *TMPRAW = "/tmp/.xq_test_reader.raw";

static void fill_pattern(uint8_t *p, size_t n, uint64_t seed)
{
    uint64_t s = seed | 1;
    for (size_t i = 0; i < n; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        p[i] = (uint8_t)((i % 251) ^ (s >> 33));
    }
}

static uint8_t *make_file(size_t n, uint32_t block_size, xq_checksum ck)
{
    uint8_t *src = malloc(n);
    if (!src) { fprintf(stderr, "oom\n"); exit(1); }
    fill_pattern(src, n, n + block_size);

    FILE *fp = fopen(TMPRAW, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", TMPRAW); exit(1); }
    fwrite(src, 1, n, fp);
    fclose(fp);

    xq_params p = xq_params_default();
    p.block_size = block_size;
    p.checksum = ck;

    xq_status st = xq_compress_file(TMPRAW, TMP, &p, NULL);
    if (st != XQ_OK) { fprintf(stderr, "compress_file: %s\n", xq_strerror(st)); exit(1); }
    return src;
}

static void test_pread_exhaustive(void)
{
    const uint32_t bs = 4096;
    const size_t n = bs * 10 + 777;
    uint8_t *src = make_file(n, bs, XQ_CHECKSUM_CRC32C);

    xq_status st;
    xq_reader *r = xq_reader_open(TMP, NULL, &st);
    CHECK(r != NULL, "open: %s", xq_strerror(st));
    if (!r) { free(src); return; }

    CHECK(xq_reader_size(r) == n, "size %llu vs %zu",
          (unsigned long long)xq_reader_size(r), n);
    CHECK(xq_reader_block_count(r) == 11, "block count %llu",
          (unsigned long long)xq_reader_block_count(r));

    uint8_t buf[9000];

    int mismatches = 0;
    for (size_t off = 0; off < n; off++) {
        int64_t got = xq_reader_pread(r, buf, 1, off);
        if (got != 1 || buf[0] != src[off]) {
            if (++mismatches < 4)
                fprintf(stderr, "  single-byte mismatch at %zu (got %lld)\n",
                        off, (long long)got);
        }
    }
    CHECK(mismatches == 0, "%d single-byte reads wrong", mismatches);

    mismatches = 0;
    for (uint64_t b = 0; b <= 11; b++) {
        for (int64_t d = -3; d <= 3; d++) {
            int64_t base = (int64_t)(b * bs) + d;
            if (base < 0 || (size_t)base >= n) continue;
            size_t off = (size_t)base;

            const size_t lens[] = { 1, 2, 100, bs - 1, bs, bs + 1, bs * 2 + 5, 8000 };
            for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                size_t want = lens[li];
                if (want > sizeof buf) continue;
                size_t expect = (off + want > n) ? n - off : want;

                int64_t got = xq_reader_pread(r, buf, want, off);
                if (got < 0 || (size_t)got != expect ||
                    memcmp(buf, src + off, expect) != 0) {
                    if (++mismatches < 4)
                        fprintf(stderr, "  range mismatch off=%zu len=%zu got=%lld\n",
                                off, want, (long long)got);
                }
            }
        }
    }
    CHECK(mismatches == 0, "%d ranged reads wrong", mismatches);

    CHECK(xq_reader_pread(r, buf, 100, n - 10) == 10, "short read at EOF");
    CHECK(xq_reader_pread(r, buf, 100, n) == 0, "read at EOF returns 0");
    CHECK(xq_reader_pread(r, buf, 100, n + 1000) == 0, "read past EOF returns 0");
    CHECK(xq_reader_pread(r, buf, 100, UINT64_MAX) == 0, "read at UINT64_MAX returns 0");
    CHECK(xq_reader_pread(r, buf, 0, 0) == 0, "zero-length read");

    xq_reader_close(r);
    free(src);
}

static void test_cache(void)
{
    const uint32_t bs = 4096;
    const size_t n = bs * 40;
    uint8_t *src = make_file(n, bs, XQ_CHECKSUM_XXH64);

    xq_reader_opts o;
    memset(&o, 0, sizeof o);
    o.cache_blocks = 2;

    xq_status st;
    xq_reader *r = xq_reader_open(TMP, &o, &st);
    CHECK(r != NULL, "open with tiny cache: %s", xq_strerror(st));
    if (!r) { free(src); return; }

    uint8_t buf[512];
    int mismatches = 0;
    uint64_t seed = 99;
    for (int i = 0; i < 4000; i++) {
        seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
        size_t off = (size_t)((seed >> 3) % (n - sizeof buf));
        int64_t got = xq_reader_pread(r, buf, sizeof buf, off);
        if (got != (int64_t)sizeof buf || memcmp(buf, src + off, sizeof buf) != 0)
            mismatches++;
    }
    CHECK(mismatches == 0, "%d random reads wrong under eviction", mismatches);

    uint64_t hits = 0, misses = 0;
    xq_reader_cache_stats(r, &hits, &misses);
    CHECK(hits + misses > 0, "cache stats recorded");

    xq_reader_close(r);

    r = xq_reader_open(TMP, NULL, &st);
    CHECK(r != NULL, "reopen");
    if (r) {
        for (int i = 0; i < 100; i++) xq_reader_pread(r, buf, 16, 100);
        xq_reader_cache_stats(r, &hits, &misses);
        CHECK(misses == 1, "expected 1 miss for 100 reads of one block, got %llu",
              (unsigned long long)misses);
        CHECK(hits == 99, "expected 99 hits, got %llu", (unsigned long long)hits);
        xq_reader_close(r);
    }

    free(src);
}

static void test_verify(void)
{
    const uint32_t bs = 4096;
    const size_t n = bs * 20;
    uint8_t *src = make_file(n, bs, XQ_CHECKSUM_CRC32C);
    free(src);

    xq_status st;
    xq_reader *r = xq_reader_open(TMP, NULL, &st);
    CHECK(r != NULL, "open for verify");
    if (!r) return;

    xq_verify_report rep;
    st = xq_reader_verify(r, &rep);
    CHECK(st == XQ_OK, "verify clean file: %s", xq_strerror(st));
    CHECK(rep.blocks_bad == 0, "no bad blocks");
    CHECK(rep.blocks_ok == 20, "20 blocks ok, got %llu", (unsigned long long)rep.blocks_ok);
    CHECK(rep.bytes_verified == n, "all bytes verified");
    CHECK(rep.stream_checksum == XQ_STREAM_CK_OK, "stream checksum ok");
    xq_reader_close(r);

    FILE *fp = fopen(TMP, "r+b");
    CHECK(fp != NULL, "reopen for damage");
    if (!fp) return;
    fseek(fp, XQ_FILE_HEADER_SIZE + XQ_BLOCK_HEADER_SIZE + 100, SEEK_SET);
    fputc(0xFF, fp);
    fputc(0x00, fp);
    fclose(fp);

    r = xq_reader_open(TMP, NULL, &st);
    CHECK(r != NULL, "open damaged file");
    if (!r) return;

    st = xq_reader_verify(r, &rep);
    CHECK(st == XQ_ERR_CORRUPT_BLOCK, "verify reports corruption");
    CHECK(rep.blocks_bad == 1, "exactly 1 bad block, got %llu",
          (unsigned long long)rep.blocks_bad);
    CHECK(rep.blocks_ok == 19, "19 blocks still ok, got %llu",
          (unsigned long long)rep.blocks_ok);
    CHECK(rep.first_bad_raw_offset == 0, "first bad block is block 0");
    CHECK(rep.stream_checksum == XQ_STREAM_CK_SKIP,
          "stream checksum skipped, not absent (got %d)", rep.stream_checksum);

    uint8_t buf[64];
    CHECK(xq_reader_pread(r, buf, sizeof buf, bs * 5) == (int64_t)sizeof buf,
          "read from an undamaged block still works");
    CHECK(xq_reader_pread(r, buf, sizeof buf, 0) < 0,
          "read from the damaged block fails");

    xq_reader_close(r);
}

static void test_open_failures(void)
{
    xq_status st;

    CHECK(xq_reader_open("/nonexistent/path/xq", NULL, &st) == NULL, "missing file");
    CHECK(st == XQ_ERR_IO, "missing file reports IO, got %s", xq_strerror(st));

    FILE *fp = fopen("/tmp/.xq_junk", "wb");
    for (int i = 0; i < 500; i++) fputc(i & 0xFF, fp);
    fclose(fp);
    CHECK(xq_reader_open("/tmp/.xq_junk", NULL, &st) == NULL, "junk refused");
    CHECK(st == XQ_ERR_BAD_MAGIC, "junk reports bad magic, got %s", xq_strerror(st));

    uint8_t *src = make_file(40000, 4096, XQ_CHECKSUM_CRC32C);
    free(src);
    FILE *in = fopen(TMP, "rb");
    FILE *out = fopen("/tmp/.xq_cut", "wb");
    for (int i = 0; i < 1000; i++) { int c = fgetc(in); if (c < 0) break; fputc(c, out); }
    fclose(in); fclose(out);
    CHECK(xq_reader_open("/tmp/.xq_cut", NULL, &st) == NULL, "truncated refused");
    CHECK(xq_status_is_corruption(st), "truncated reports corruption, got %s",
          xq_strerror(st));

    xq_reader_opts o;
    memset(&o, 0, sizeof o);
    o.mem_limit = 64;
    CHECK(xq_reader_open(TMP, &o, &st) == NULL, "tiny mem_limit refused");
    CHECK(st == XQ_ERR_MEMLIMIT, "reports memlimit, got %s", xq_strerror(st));

    remove("/tmp/.xq_junk");
    remove("/tmp/.xq_cut");
}

int main(void)
{
    test_pread_exhaustive();
    test_cache();
    test_verify();
    test_open_failures();

    remove(TMP);
    remove(TMPRAW);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_dict.h"

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

#ifdef XQ_WITH_ZSTD
static const char *TMP = "/tmp/.xq_test_dict.xq";
static const char *TMPRAW = "/tmp/.xq_test_dict.raw";

#define VOCAB_N   4000
#define VOCAB_LEN 160

static uint8_t *vocab;

static void build_vocab(void)
{
    if (vocab) return;
    vocab = malloc((size_t)VOCAB_N * VOCAB_LEN);
    uint64_t s = 0x1234567;
    for (size_t i = 0; i < (size_t)VOCAB_N * VOCAB_LEN; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;

        vocab[i] = (uint8_t)('a' + ((s >> 33) % 26));
    }
}

static void fill_redundant(uint8_t *p, size_t n, uint64_t seed)
{
    build_vocab();
    uint64_t s = seed | 1;
    size_t i = 0;
    while (i < n) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        const uint8_t *w = vocab + ((s >> 17) % VOCAB_N) * VOCAB_LEN;
        size_t take = (n - i < VOCAB_LEN) ? n - i : VOCAB_LEN;
        memcpy(p + i, w, take);
        i += take;
    }
}
#endif

static void test_size_policy(void)
{

    CHECK(xq_dict_size_for(1000, 8u << 20) == 0, "tiny input gets no dictionary");
    CHECK(xq_dict_size_for(100u << 20, 0) == 0, "explicit 0 disables");
    CHECK(xq_dict_size_for(100u << 20, 8u << 20) == 8u << 20, "normal case");
    CHECK(xq_dict_size_for(16u << 20, 8u << 20) == 4u << 20, "capped at raw/4");
    CHECK(xq_dict_size_for(1u << 20, 8u << 20) == 256u * 1024u,
          "1 MiB input: capped to raw/4 = 256 KiB, which clears the floor");
    CHECK(xq_dict_size_for(200u * 1024u, 8u << 20) == 0,
          "200 KiB input: raw/4 is below the 64 KiB floor, so no dictionary");
}

#ifdef XQ_WITH_ZSTD
static uint8_t *make_file(size_t n, uint32_t block_size, uint32_t dict_size,
                          xq_status *st_out)
{
    uint8_t *src = malloc(n);
    fill_redundant(src, n, n + dict_size);

    FILE *fp = fopen(TMPRAW, "wb");
    fwrite(src, 1, n, fp);
    fclose(fp);

    xq_params p = xq_params_default();
    p.block_size = block_size;
    p.dict_size = dict_size;
    p.codec = XQ_CODEC_ZSTD;

    *st_out = xq_compress_file(TMPRAW, TMP, &p, NULL);
    return src;
}

static void test_roundtrip_with_dict(void)
{
    const size_t n = 8u << 20;
    const uint32_t bs = 64u * 1024u;

    for (uint32_t ds = 0; ds <= (1u << 20); ds = ds ? ds * 4 : (256u * 1024u)) {
        xq_status st;
        uint8_t *src = make_file(n, bs, ds, &st);
        CHECK(st == XQ_OK, "compress with dict %u: %s", ds, xq_strerror(st));
        if (st != XQ_OK) { free(src); continue; }

        st = xq_decompress_file(TMP, "/tmp/.xq_test_dict.out", 0, NULL);
        CHECK(st == XQ_OK, "sequential decompress dict %u: %s", ds, xq_strerror(st));

        FILE *fp = fopen("/tmp/.xq_test_dict.out", "rb");
        uint8_t *back = malloc(n);
        size_t got = fread(back, 1, n, fp);
        fclose(fp);
        CHECK(got == n, "sequential length dict %u", ds);
        CHECK(memcmp(src, back, n) == 0, "sequential content dict %u", ds);

        xq_reader *r = xq_reader_open(TMP, NULL, &st);
        CHECK(r != NULL, "reader open dict %u: %s", ds, xq_strerror(st));
        if (r) {
            int bad = 0;
            uint8_t buf[4096];
            for (size_t off = 0; off + sizeof buf <= n; off += bs / 2 + 7) {
                int64_t rd = xq_reader_pread(r, buf, sizeof buf, off);
                if (rd != (int64_t)sizeof buf || memcmp(buf, src + off, sizeof buf) != 0)
                    bad++;
            }
            CHECK(bad == 0, "%d random reads wrong with dict %u", bad, ds);
            xq_reader_close(r);
        }

        free(src); free(back);
    }
}

static void test_dict_improves_ratio(void)
{
    const size_t n = 8u << 20;
    const uint32_t bs = 64u * 1024u;
    xq_status st;

    uint8_t *src = make_file(n, bs, 0, &st);
    free(src);
    FILE *fp = fopen(TMP, "rb"); fseek(fp, 0, SEEK_END);
    long without = ftell(fp); fclose(fp);

    src = make_file(n, bs, 1u << 20, &st);
    free(src);
    fp = fopen(TMP, "rb"); fseek(fp, 0, SEEK_END);
    long with = ftell(fp); fclose(fp);

    CHECK(with < without,
          "dictionary must reduce size on redundant data: %ld -> %ld", without, with);
    printf("  ratio effect: %ld -> %ld bytes (%.1f%% smaller)\n",
           without, with, 100.0 * (double)(without - with) / (double)without);
}

static void test_dict_corruption(void)
{
    const size_t n = 4u << 20;
    xq_status st;
    uint8_t *src = make_file(n, 64u * 1024u, 512u * 1024u, &st);
    free(src);
    CHECK(st == XQ_OK, "setup");

    FILE *fp = fopen(TMP, "r+b");
    CHECK(fp != NULL, "reopen");
    if (!fp) return;
    fseek(fp, XQ_FILE_HEADER_SIZE + XQ_RECORD_HEADER_SIZE + XQ_DICT_PAYLOAD_HEADER + 500,
          SEEK_SET);
    int c = fgetc(fp);
    fseek(fp, -1, SEEK_CUR);
    fputc(c ^ 0x40, fp);
    fclose(fp);

    st = xq_decompress_file(TMP, "/tmp/.xq_test_dict.out", 0, NULL);
    CHECK(st != XQ_OK, "damaged dictionary must not decode cleanly");
    CHECK(xq_status_is_corruption(st), "reported as corruption, got %s", xq_strerror(st));

    xq_reader *r = xq_reader_open(TMP, NULL, &st);
    CHECK(r == NULL, "reader must refuse a file with a damaged dictionary");
    CHECK(xq_status_is_corruption(st), "reader reports corruption, got %s",
          xq_strerror(st));
    if (r) xq_reader_close(r);
}

static void test_dict_unsupported_codec(void)
{
    uint8_t src[256 * 1024];
    fill_redundant(src, sizeof src, 5);

    xq_params p = xq_params_default();
    p.codec = XQ_CODEC_STORED;
    p.dict_size = 64u * 1024u;
    p.block_size = 4096;

    size_t cap = xq_compress_bound(sizeof src, &p);
    uint8_t *out = malloc(cap);
    size_t len = 0;
    xq_status st = xq_compress(out, cap, &len, src, sizeof src, &p);

    CHECK(st == XQ_OK || st == XQ_ERR_UNSUPPORTED_FEATURE,
          "stored + dict: %s", xq_strerror(st));
    free(out);
}
#endif

#ifdef XQ_WITH_ZSTD

static void test_bound_covers_dictionary(void)
{
    const size_t n = 4u << 20;
    uint8_t *src = malloc(n);

    uint64_t s = 0xABCDEF;
    for (size_t i = 0; i < n; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        src[i] = (uint8_t)(s >> 33);
    }

    xq_params p = xq_params_default();
    p.codec = XQ_CODEC_ZSTD;
    p.block_size = 64u * 1024u;
    p.dict_size = 1u << 20;

    size_t bound = xq_compress_bound(n, &p);
    CHECK(bound > 0, "bound computed");

    uint8_t *dst = malloc(bound);
    size_t out = 0;
    xq_status st = xq_compress(dst, bound, &out, src, n, &p);
    CHECK(st == XQ_OK, "incompressible input must fit its own bound: %s",
          xq_strerror(st));
    CHECK(out <= bound, "produced %zu exceeds bound %zu", out, bound);

    if (st == XQ_OK) {
        uint8_t *back = malloc(n);
        size_t got = 0;
        st = xq_decompress(back, n, &got, dst, out, 0);
        CHECK(st == XQ_OK && got == n && memcmp(src, back, n) == 0,
              "incompressible round trip with dictionary");
        free(back);
    }

    free(src); free(dst);
}
#endif

int main(void)
{
    test_size_policy();
#ifdef XQ_WITH_ZSTD
    test_roundtrip_with_dict();
    test_dict_improves_ratio();
    test_dict_corruption();
    test_dict_unsupported_codec();
    test_bound_covers_dictionary();
    remove(TMP); remove(TMPRAW); remove("/tmp/.xq_test_dict.out");
#else
    printf("  (dictionary tests need a codec that supports one: make WITH_ZSTD=1)\n");
#endif
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

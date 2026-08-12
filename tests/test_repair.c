/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_format.h"

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

static const char *RAW = "/tmp/.xq_repair.raw";
static const char *XZF = "/tmp/.xq_repair.xq";
static const char *OUT = "/tmp/.xq_repair.out";

static size_t   g_len;
static uint8_t *g_src;

static void build(size_t n, uint32_t bs, xq_codec codec)
{
    g_len = n;
    g_src = malloc(n);
    uint64_t s = 777;
    for (size_t i = 0; i < n; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        g_src[i] = (uint8_t)((i % 251) ^ (s >> 33));
    }
    FILE *fp = fopen(RAW, "wb");
    fwrite(g_src, 1, n, fp);
    fclose(fp);

    xq_params p = xq_params_default();
    p.block_size = bs;
    p.codec = codec;
    p.dict_size = 0;
    xq_status st = xq_compress_file(RAW, XZF, &p, NULL);
    CHECK(st == XQ_OK, "setup compress: %s", xq_strerror(st));
}

static long fsize(const char *p)
{
    FILE *fp = fopen(p, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fclose(fp);
    return n;
}

static uint8_t *slurp(const char *p, long *n)
{
    long sz = fsize(p);
    if (sz < 0) return NULL;
    uint8_t *b = malloc((size_t)sz ? (size_t)sz : 1);
    FILE *fp = fopen(p, "rb");
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); free(b); return NULL; }
    fclose(fp);
    *n = sz;
    return b;
}

static void test_repair_healthy(void)
{
    build(1u << 20, 8192, XQ_CODEC_STORED);

    xq_repair_report rep;
    xq_status st = xq_repair_file(XZF, OUT, NULL, &rep);
    CHECK(st == XQ_OK, "repair healthy: %s", xq_strerror(st));
    CHECK(rep.blocks_bad == 0, "no damaged blocks, got %llu",
          (unsigned long long)rep.blocks_bad);
    CHECK(rep.bytes_recovered == g_len, "recovered all bytes");
    CHECK(rep.gaps == 0, "no gaps");

    long n = 0;
    uint8_t *got = slurp(OUT, &n);
    CHECK(got && (size_t)n == g_len && memcmp(got, g_src, g_len) == 0,
          "repaired output identical to the original");
    free(got); free(g_src);
}

static void test_repair_no_footer(void)
{
    build(1u << 20, 8192, XQ_CODEC_STORED);

    long sz = fsize(XZF);
    uint8_t *whole = NULL;
    long wn = 0;
    whole = slurp(XZF, &wn);
    FILE *fp = fopen(XZF, "wb");
    fwrite(whole, 1, (size_t)(sz - 4096), fp);
    fclose(fp);
    free(whole);

    xq_status st;
    xq_reader *r = xq_reader_open(XZF, NULL, &st);
    CHECK(r == NULL, "reader cannot open a file with no footer");
    if (r) xq_reader_close(r);

    xq_repair_report rep;
    st = xq_repair_file(XZF, OUT, NULL, &rep);
    CHECK(st == XQ_OK || xq_status_is_corruption(st), "repair runs anyway: %s",
          xq_strerror(st));
    CHECK(rep.bytes_recovered > g_len / 2,
          "recovered most of the data: %llu of %zu",
          (unsigned long long)rep.bytes_recovered, g_len);

    long n = 0;
    uint8_t *got = slurp(OUT, &n);
    CHECK(got && (size_t)n == rep.bytes_recovered, "output length matches the report");
    CHECK(got && memcmp(got, g_src, (size_t)n) == 0,
          "every recovered byte is correct");
    free(got); free(g_src);
}

static void test_repair_hole(void)
{
    const uint32_t bs = 8192;
    build(1u << 20, bs, XQ_CODEC_STORED);

    long sz = fsize(XZF);
    long hole_at = sz * 40 / 100;
    long hole_len = 30000;

    FILE *fp = fopen(XZF, "r+b");
    fseek(fp, hole_at, SEEK_SET);
    for (long i = 0; i < hole_len; i++) fputc(0xA5, fp);
    fclose(fp);

    xq_repair_opts o;
    memset(&o, 0, sizeof o);
    o.fill_gaps = 1;
    o.fill_byte = 0;

    xq_repair_report rep;
    xq_status st = xq_repair_file(XZF, OUT, &o, &rep);
    CHECK(xq_status_is_corruption(st) || st == XQ_OK, "repair with a hole: %s",
          xq_strerror(st));
    CHECK(rep.blocks_ok > 0, "some blocks survived");
    CHECK(rep.bytes_recovered < g_len, "not everything survived");
    CHECK(rep.bytes_recovered > g_len / 2, "most of it did");

    long n = 0;
    uint8_t *got = slurp(OUT, &n);
    CHECK(got != NULL, "output written");
    if (got) {
        size_t wrong = 0;
        size_t limit = (size_t)n < g_len ? (size_t)n : g_len;
        for (size_t i = 0; i < limit; i++)
            if (got[i] != g_src[i] && got[i] != 0) wrong++;
        CHECK(wrong == 0, "%zu bytes wrong outside the filled gap", wrong);
        free(got);
    }
    free(g_src);
}

static void test_repair_junk(void)
{
    FILE *fp = fopen(XZF, "wb");
    for (int i = 0; i < 5000; i++) fputc(i & 0xFF, fp);
    fclose(fp);

    xq_repair_report rep;
    xq_status st = xq_repair_file(XZF, OUT, NULL, &rep);
    CHECK(st == XQ_ERR_BAD_MAGIC, "junk rejected by magic, got %s", xq_strerror(st));

    st = xq_repair_file("/nonexistent/xq/file", OUT, NULL, &rep);
    CHECK(st == XQ_ERR_IO, "missing file: %s", xq_strerror(st));
}

int main(void)
{
    test_repair_healthy();
    test_repair_no_footer();
    test_repair_hole();
    test_repair_junk();

    remove(RAW); remove(XZF); remove(OUT);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

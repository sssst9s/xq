/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_thread.h"

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

static const char *TMP    = "/tmp/.xq_test_threads.xq";
static const char *TMPRAW = "/tmp/.xq_test_threads.raw";

static size_t   g_len;
static uint8_t *g_src;

static void fill(uint8_t *p, size_t n, uint64_t seed)
{
    uint64_t s = seed | 1;
    for (size_t i = 0; i < n; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        p[i] = (uint8_t)((i % 251) ^ (s >> 33));
    }
}

static void write_input(size_t n)
{
    g_len = n;
    g_src = malloc(n);
    fill(g_src, n, 4242);
    FILE *fp = fopen(TMPRAW, "wb");
    fwrite(g_src, 1, n, fp);
    fclose(fp);
}

static void compress_with(int threads, uint32_t bs, xq_codec codec, uint32_t dict)
{
    xq_params p = xq_params_default();
    p.block_size = bs;
    p.threads = threads;
    p.codec = codec;
    p.dict_size = dict;
    xq_status st = xq_compress_file(TMPRAW, TMP, &p, NULL);
    CHECK(st == XQ_OK, "compress threads=%d: %s", threads, xq_strerror(st));
}

static long file_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fclose(fp);
    return n;
}

static uint8_t *slurp(const char *path, long *out)
{
    long n = file_size(path);
    if (n < 0) return NULL;
    uint8_t *b = malloc((size_t)n);
    FILE *fp = fopen(path, "rb");
    if (fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(b); return NULL; }
    fclose(fp);
    *out = n;
    return b;
}

static void test_encode_determinism(xq_codec codec, uint32_t dict, const char *label)
{
    write_input(3 * 1024 * 1024 + 1234);

    compress_with(1, 16384, codec, dict);
    long n1 = 0;
    uint8_t *ref = slurp(TMP, &n1);
    CHECK(ref != NULL, "%s: reference output", label);
    if (!ref) { free(g_src); return; }

    const int counts[] = { 2, 3, 4, 7, 8, 16 };
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        compress_with(counts[i], 16384, codec, dict);
        long n = 0;
        uint8_t *got = slurp(TMP, &n);
        CHECK(got && n == n1 && memcmp(ref, got, (size_t)n1) == 0,
              "%s: threads=%d output differs from single-threaded", label, counts[i]);
        free(got);
    }

    xq_status st = xq_decompress_file(TMP, "/tmp/.xq_test_threads.out", 0, NULL);
    CHECK(st == XQ_OK, "%s: decompress: %s", label, xq_strerror(st));
    long bn = 0;
    uint8_t *back = slurp("/tmp/.xq_test_threads.out", &bn);
    CHECK(back && (size_t)bn == g_len && memcmp(back, g_src, g_len) == 0,
          "%s: round trip", label);

    free(back); free(ref); free(g_src);
}

typedef struct {
    xq_reader *r;
    uint64_t   seed;
    int        iters;
    int        errors;
} hammer_arg;

static void *hammer(void *argp)
{
    hammer_arg *a = (hammer_arg *)argp;
    uint8_t buf[8192];
    uint64_t s = a->seed | 1;

    for (int i = 0; i < a->iters; i++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        size_t len = (size_t)(s % sizeof buf) + 1;
        if (len > g_len) len = g_len;
        uint64_t off = (g_len > len) ? (s >> 20) % (g_len - len) : 0;

        int64_t got = xq_reader_pread(a->r, buf, len, off);
        if (got < 0 || (size_t)got != len || memcmp(buf, g_src + off, len) != 0)
            a->errors++;
    }
    return NULL;
}

static void test_concurrent_reads(xq_codec codec, uint32_t dict, const char *label)
{
    write_input(4 * 1024 * 1024);
    compress_with(0, 16384, codec, dict);

    xq_reader_opts o;
    memset(&o, 0, sizeof o);
    o.cache_blocks = 8;
    o.threads = 8;

    xq_status st;
    xq_reader *r = xq_reader_open(TMP, &o, &st);
    CHECK(r != NULL, "%s: reader open: %s", label, xq_strerror(st));
    if (!r) { free(g_src); return; }

    enum { NT = 8 };
    xq_thread th[NT];
    hammer_arg args[NT];
    memset(th, 0, sizeof th);

    for (int i = 0; i < NT; i++) {
        args[i].r = r;
        args[i].seed = 0x9E3779B9u * (uint64_t)(i + 1);
        args[i].iters = 3000;
        args[i].errors = 0;
        CHECK(xq_thread_start(&th[i], hammer, &args[i]) == XQ_OK,
              "%s: start thread %d", label, i);
    }

    int total = 0;
    for (int i = 0; i < NT; i++) { xq_thread_join(&th[i]); total += args[i].errors; }

    CHECK(total == 0, "%s: %d incorrect reads across %d threads x 3000 reads",
          label, total, NT);

    uint64_t hits = 0, misses = 0;
    xq_reader_cache_stats(r, &hits, &misses);
    CHECK(hits + misses >= NT * 3000,
          "%s: cache accounting lost events (%llu + %llu)", label,
          (unsigned long long)hits, (unsigned long long)misses);

    xq_reader_close(r);
    free(g_src);
}

static void test_threads_param(void)
{
    xq_params p = xq_params_default();
    const char *why = NULL;
    p.threads = -1;
    CHECK(xq_params_check(&p, &why) == XQ_ERR_PARAM, "negative thread count rejected");
    p.threads = 1;
    CHECK(xq_params_check(&p, &why) == XQ_OK, "threads=1 accepted");
    p.threads = 0;
    CHECK(xq_params_check(&p, &why) == XQ_OK, "threads=0 (auto) accepted");
    CHECK(xq_cpu_count() >= 1, "cpu count is at least 1");
}

int main(void)
{
    test_threads_param();

    test_encode_determinism(XQ_CODEC_STORED, 0, "stored");
    test_concurrent_reads(XQ_CODEC_STORED, 0, "stored");
#ifdef XQ_WITH_ZSTD
    test_encode_determinism(XQ_CODEC_ZSTD, 0, "zstd");
    test_encode_determinism(XQ_CODEC_ZSTD, 256 * 1024, "zstd+dict");
    test_concurrent_reads(XQ_CODEC_ZSTD, 256 * 1024, "zstd+dict");
#endif

    remove(TMP); remove(TMPRAW); remove("/tmp/.xq_test_threads.out");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

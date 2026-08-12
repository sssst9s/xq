/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xq.h"

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void)
{
    uint64_t x = rs; x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rs = x; return x * 0x2545F4914F6CDD1DULL;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.xq> [reads] [read_size] [cache_blocks]\n", argv[0]);
        return 2;
    }
    size_t nreads    = argc > 2 ? (size_t)atoll(argv[2]) : 20000;
    size_t read_size = argc > 3 ? (size_t)atoll(argv[3]) : 4096;
    unsigned cache   = argc > 4 ? (unsigned)atoi(argv[4]) : 16;

    xq_reader_opts o;
    memset(&o, 0, sizeof o);
    o.cache_blocks = cache;

    xq_status st;
    xq_reader *r = xq_reader_open(argv[1], &o, &st);
    if (!r) { fprintf(stderr, "open: %s\n", xq_strerror(st)); return 1; }

    uint64_t size = xq_reader_size(r);
    if (size <= read_size) { fprintf(stderr, "file too small\n"); return 1; }

    uint8_t *buf = malloc(read_size);
    double *lat = malloc(nreads * sizeof *lat);
    if (!buf || !lat) { fprintf(stderr, "oom\n"); return 1; }

    for (int i = 0; i < 64; i++)
        xq_reader_pread(r, buf, read_size, rnd() % (size - read_size));

    for (size_t i = 0; i < nreads; i++) {
        uint64_t off = rnd() % (size - read_size);
        double t0 = now_us();
        int64_t got = xq_reader_pread(r, buf, read_size, off);
        lat[i] = now_us() - t0;
        if (got < 0) { fprintf(stderr, "read: %s\n", xq_strerror((xq_status)(-got))); return 1; }
    }

    qsort(lat, nreads, sizeof *lat, cmp_d);
    double sum = 0;
    for (size_t i = 0; i < nreads; i++) sum += lat[i];

    uint64_t hits = 0, misses = 0;
    xq_reader_cache_stats(r, &hits, &misses);

    printf("file            %s\n", argv[1]);
    printf("uncompressed    %.1f MiB in %llu blocks\n",
           (double)size / 1048576.0, (unsigned long long)xq_reader_block_count(r));
    printf("reads           %zu x %zu bytes, cache %u blocks\n", nreads, read_size, cache);
    printf("cache           %llu hits, %llu misses (%.1f%% hit)\n",
           (unsigned long long)hits, (unsigned long long)misses,
           100.0 * (double)hits / (double)(hits + misses));
    printf("latency  mean   %8.2f us\n", sum / (double)nreads);
    printf("         p50    %8.2f us\n", lat[nreads / 2]);
    printf("         p90    %8.2f us\n", lat[(size_t)((double)nreads * 0.90)]);
    printf("         p99    %8.2f us\n", lat[(size_t)((double)nreads * 0.99)]);
    printf("         p99.9  %8.2f us\n", lat[(size_t)((double)nreads * 0.999)]);
    printf("         max    %8.2f us\n", lat[nreads - 1]);
    printf("throughput      %.1f MiB/s effective\n",
           (double)(nreads * read_size) / (sum) * 1e6 / 1048576.0);

    free(buf); free(lat);
    xq_reader_close(r);
    return 0;
}

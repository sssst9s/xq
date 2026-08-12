/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq_xxh64.h"

#ifdef XQ_WITH_ZSTD

extern unsigned long long ZSTD_XXH64(const void *input, size_t len,
                                     unsigned long long seed);
#endif

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

static void test_vectors(void)
{
    CHECK(xq_xxh64("", 0, 0) == 0xEF46DB3751D8E999ULL,
          "empty/seed0 = %016llX", (unsigned long long)xq_xxh64("", 0, 0));
    CHECK(xq_xxh64("", 0, 1) == 0xD5AFBA1336A3BE4BULL,
          "empty/seed1 = %016llX", (unsigned long long)xq_xxh64("", 0, 1));
    CHECK(xq_xxh64("a", 1, 0) == 0xD24EC4F1A98C6E5BULL,
          "\"a\" = %016llX", (unsigned long long)xq_xxh64("a", 1, 0));
    CHECK(xq_xxh64("abc", 3, 0) == 0x44BC2CF5AD770999ULL,
          "\"abc\" = %016llX", (unsigned long long)xq_xxh64("abc", 3, 0));
    CHECK(xq_xxh64("123456789", 9, 0) == 0x8CB841DB40E6AE83ULL,
          "\"123456789\" = %016llX", (unsigned long long)xq_xxh64("123456789", 9, 0));
    CHECK(xq_xxh64("Hello, world!", 13, 0) == 0xF58336A78B6F9476ULL,
          "\"Hello, world!\" = %016llX", (unsigned long long)xq_xxh64("Hello, world!", 13, 0));
}

static void test_streaming(void)
{
    static uint8_t blob[1024];
    for (size_t i = 0; i < sizeof blob; i++)
        blob[i] = (uint8_t)(i * 167u + (i >> 5));

    for (size_t len = 0; len <= 300; len++) {
        uint64_t once = xq_xxh64(blob, len, 0);

        for (size_t split = 0; split <= len; split++) {
            xq_xxh64_state s;
            xq_xxh64_init(&s, 0);
            xq_xxh64_update(&s, blob, split);
            xq_xxh64_update(&s, blob + split, len - split);
            uint64_t inc = xq_xxh64_final(&s);
            if (inc != once) {
                CHECK(0, "streaming differs at len %zu split %zu: %016llX vs %016llX",
                      len, split, (unsigned long long)inc, (unsigned long long)once);
                return;
            }
        }
    }
    checks++;

    for (size_t chunk = 1; chunk <= 64; chunk++) {
        xq_xxh64_state s;
        xq_xxh64_init(&s, 0);
        for (size_t off = 0; off < sizeof blob; off += chunk) {
            size_t n = sizeof blob - off < chunk ? sizeof blob - off : chunk;
            xq_xxh64_update(&s, blob + off, n);
        }
        CHECK(xq_xxh64_final(&s) == xq_xxh64(blob, sizeof blob, 0),
              "chunked update size %zu", chunk);
    }
}

#ifdef XQ_WITH_ZSTD

static void test_against_reference(void)
{
    static uint8_t blob[700];
    for (size_t i = 0; i < sizeof blob; i++)
        blob[i] = (uint8_t)(i * 211u + (i >> 3));

    const uint64_t seeds[] = { 0, 1, 0xDEADBEEF, 0xFFFFFFFFFFFFFFFFULL };

    for (size_t si = 0; si < sizeof seeds / sizeof seeds[0]; si++) {
        for (size_t off = 0; off < 8; off++) {
            for (size_t len = 0; len + off < sizeof blob; len++) {
                uint64_t mine = xq_xxh64(blob + off, len, seeds[si]);
                uint64_t ref  = ZSTD_XXH64(blob + off, len, seeds[si]);
                if (mine != ref) {
                    CHECK(0, "mismatch seed %llu off %zu len %zu: %016llX vs %016llX",
                          (unsigned long long)seeds[si], off, len,
                          (unsigned long long)mine, (unsigned long long)ref);
                    return;
                }
            }
        }
    }
    checks++;
    printf("  cross-checked against libzstd reference over 4 seeds x 8 offsets x ~700 lengths\n");
}
#endif

int main(void)
{
    test_vectors();
    test_streaming();
#ifdef XQ_WITH_ZSTD
    test_against_reference();
#else
    printf("  (build with WITH_ZSTD=1 to cross-check against the reference)\n");
#endif

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

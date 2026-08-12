/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xq_huff.h"

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

static void roundtrip(const uint8_t *data, size_t n, const char *label)
{
    uint32_t freq[XQ_HUFF_SYMBOLS];
    memset(freq, 0, sizeof freq);
    for (size_t i = 0; i < n; i++) freq[data[i]]++;

    xq_huff_enc e;
    if (!xq_huff_build(&e, freq)) { CHECK(n == 0, "%s: build failed", label); return; }
    CHECK(e.max_len <= XQ_HUFF_MAX_BITS, "%s: code length %d exceeds the ceiling",
          label, e.max_len);

    uint8_t table[XQ_HUFF_TABLE_BYTES];
    xq_huff_store(&e, table);

    uint8_t *bits = malloc(n * 3 + 64);
    xq_bitw w;
    xq_bitw_init(&w, bits, n * 3 + 64);
    for (size_t i = 0; i < n; i++) xq_huff_emit(&w, &e, data[i]);
    size_t blen = 0;
    CHECK(xq_bitw_flush(&w, &blen), "%s: bit writer overflowed", label);

    xq_huff_dec d;
    CHECK(xq_huff_load(&d, table), "%s: table failed to load", label);

    xq_bitr r;
    xq_bitr_init(&r, bits, blen);
    size_t wrong = 0;
    for (size_t i = 0; i < n; i++) {
        int s = xq_huff_decode(&r, &d);
        if (s < 0 || (uint8_t)s != data[i]) { wrong++; break; }
    }
    CHECK(wrong == 0, "%s: decoded stream differs from the input", label);
    free(bits);
}

int main(void)
{
    static uint8_t buf[200000];

    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i & 0xFF);
    roundtrip(buf, sizeof buf, "uniform");

    memset(buf, 'Q', sizeof buf);
    roundtrip(buf, sizeof buf, "single symbol");

    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i & 1 ? 'a' : 'b');
    roundtrip(buf, sizeof buf, "two symbols");

    {
        const char *w = "the quick brown fox jumps over the lazy dog ";
        size_t wl = strlen(w);
        for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)w[i % wl];
        roundtrip(buf, sizeof buf, "text");
    }

    {
        uint64_t s = 1;
        for (size_t i = 0; i < sizeof buf; i++) {
            s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
            buf[i] = ((s >> 33) % 1000 == 0) ? (uint8_t)((s >> 40) & 0xFF) : 0;
        }
        roundtrip(buf, sizeof buf, "skewed");
    }

    for (size_t n = 1; n <= 64; n++) {
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 7);
        roundtrip(buf, n, "short");
    }

    {
        uint8_t table[XQ_HUFF_TABLE_BYTES];
        xq_huff_dec d;

        memset(table, 0, sizeof table);
        CHECK(!xq_huff_load(&d, table), "empty table rejected");

        memset(table, 0x11, sizeof table);
        CHECK(!xq_huff_load(&d, table), "over-subscribed table rejected");

        memset(table, 0, sizeof table);
        table[0] = 0x02;
        CHECK(!xq_huff_load(&d, table), "incomplete table rejected");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

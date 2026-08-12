/* SPDX-License-Identifier: Apache-2.0 */
#include "xq_xxh64.h"
#include "xq_bits.h"
#include "xq_checked.h"

static const uint64_t P1 = 11400714785074694791ULL;
static const uint64_t P2 = 14029467366897019727ULL;
static const uint64_t P3 =  1609587929392839161ULL;
static const uint64_t P4 =  9650029242287828579ULL;
static const uint64_t P5 =  2870177450012600261ULL;

static inline uint64_t rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

XQ_WRAPS
static inline uint64_t round64(uint64_t acc, uint64_t input)
{
    acc += input * P2;
    acc = rotl64(acc, 31);
    acc *= P1;
    return acc;
}

XQ_WRAPS
static inline uint64_t merge_round(uint64_t acc, uint64_t val)
{
    val = round64(0, val);
    acc ^= val;
    acc = acc * P1 + P4;
    return acc;
}

XQ_WRAPS
uint64_t xq_xxh64(const void *data, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *const end = p + len;
    uint64_t h;

    if (len >= 32) {
        const uint8_t *const limit = end - 32;
        uint64_t v1 = seed + P1 + P2;
        uint64_t v2 = seed + P2;
        uint64_t v3 = seed + 0;
        uint64_t v4 = seed - P1;

        do {
            v1 = round64(v1, xq_ld64le(p)); p += 8;
            v2 = round64(v2, xq_ld64le(p)); p += 8;
            v3 = round64(v3, xq_ld64le(p)); p += 8;
            v4 = round64(v4, xq_ld64le(p)); p += 8;
        } while (p <= limit);

        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge_round(h, v1);
        h = merge_round(h, v2);
        h = merge_round(h, v3);
        h = merge_round(h, v4);
    } else {
        h = seed + P5;
    }

    h += (uint64_t)len;

    while (end - p >= 8) {
        h ^= round64(0, xq_ld64le(p));
        h = rotl64(h, 27) * P1 + P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= (uint64_t)xq_ld32le(p) * P1;
        h = rotl64(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p) * P5;
        h = rotl64(h, 11) * P1;
        p++;
    }

    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;

    return h;
}

XQ_WRAPS
void xq_xxh64_init(xq_xxh64_state *s, uint64_t seed)
{
    s->v1 = seed + P1 + P2;
    s->v2 = seed + P2;
    s->v3 = seed + 0;
    s->v4 = seed - P1;
    s->total = 0;
    s->buffered = 0;
    s->seed = seed;
}

XQ_WRAPS
void xq_xxh64_update(xq_xxh64_state *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *const end = p + len;
    s->total += len;

    if (s->buffered) {
        size_t want = 32 - s->buffered;
        size_t take = len < want ? len : want;
        for (size_t i = 0; i < take; i++) s->buf[s->buffered + i] = p[i];
        s->buffered += take;
        p += take;
        if (s->buffered < 32) return;

        const uint8_t *b = s->buf;
        s->v1 = round64(s->v1, xq_ld64le(b)); b += 8;
        s->v2 = round64(s->v2, xq_ld64le(b)); b += 8;
        s->v3 = round64(s->v3, xq_ld64le(b)); b += 8;
        s->v4 = round64(s->v4, xq_ld64le(b));
        s->buffered = 0;
    }

    while (end - p >= 32) {
        s->v1 = round64(s->v1, xq_ld64le(p)); p += 8;
        s->v2 = round64(s->v2, xq_ld64le(p)); p += 8;
        s->v3 = round64(s->v3, xq_ld64le(p)); p += 8;
        s->v4 = round64(s->v4, xq_ld64le(p)); p += 8;
    }

    if (p < end) {
        size_t rem = (size_t)(end - p);
        for (size_t i = 0; i < rem; i++) s->buf[i] = p[i];
        s->buffered = rem;
    }
}

XQ_WRAPS
uint64_t xq_xxh64_final(const xq_xxh64_state *s)
{
    uint64_t h;

    if (s->total >= 32) {
        h = rotl64(s->v1, 1) + rotl64(s->v2, 7) + rotl64(s->v3, 12) + rotl64(s->v4, 18);
        h = merge_round(h, s->v1);
        h = merge_round(h, s->v2);
        h = merge_round(h, s->v3);
        h = merge_round(h, s->v4);
    } else {
        h = s->seed + P5;
    }

    h += s->total;

    const uint8_t *p = s->buf;
    const uint8_t *const end = s->buf + s->buffered;

    while (end - p >= 8) {
        h ^= round64(0, xq_ld64le(p));
        h = rotl64(h, 27) * P1 + P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= (uint64_t)xq_ld32le(p) * P1;
        h = rotl64(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p) * P5;
        h = rotl64(h, 11) * P1;
        p++;
    }

    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;

    return h;
}

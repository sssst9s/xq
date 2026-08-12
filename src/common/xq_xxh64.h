/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_XXH64_H
#define XQ_XXH64_H

#include <stddef.h>
#include <stdint.h>

uint64_t xq_xxh64(const void *data, size_t len, uint64_t seed);

typedef struct {
    uint64_t v1, v2, v3, v4;
    uint64_t total;
    uint64_t seed;
    uint8_t  buf[32];
    size_t   buffered;
} xq_xxh64_state;

void     xq_xxh64_init(xq_xxh64_state *s, uint64_t seed);
void     xq_xxh64_update(xq_xxh64_state *s, const void *data, size_t len);
uint64_t xq_xxh64_final(const xq_xxh64_state *s);

#endif

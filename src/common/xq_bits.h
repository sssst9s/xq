/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_BITS_H
#define XQ_BITS_H

#include <stdint.h>
#include <string.h>

static inline uint8_t xq_ld8(const uint8_t *p) { return p[0]; }

static inline uint16_t xq_ld16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t xq_ld32le(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t xq_ld64le(const uint8_t *p)
{
    return (uint64_t)xq_ld32le(p) | ((uint64_t)xq_ld32le(p + 4) << 32);
}

static inline void xq_st8(uint8_t *p, uint8_t v) { p[0] = v; }

static inline void xq_st16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline void xq_st32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void xq_st64le(uint8_t *p, uint64_t v)
{
    xq_st32le(p,     (uint32_t)(v));
    xq_st32le(p + 4, (uint32_t)(v >> 32));
}

#endif

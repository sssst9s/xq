/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_DICT_H
#define XQ_DICT_H

#include <stddef.h>
#include <stdint.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_file.h"

typedef struct {
    uint8_t  *bytes;
    size_t    len;
    uint32_t  id;
    uint8_t   kind;
    int       borrowed;
} xq_dict;

uint32_t xq_dict_size_for(uint64_t raw_size, uint32_t requested);

xq_status xq_dict_build_sampled(xq_file *f, uint64_t size, uint32_t want,
                                const xq_allocator *a, xq_dict *out);

xq_status xq_dict_build_prefix(const void *buf, size_t len, uint32_t want,
                               const xq_allocator *a, xq_dict *out);

void xq_dict_free(xq_dict *d, const xq_allocator *a);

#define XQ_DICT_PAYLOAD_HEADER 30

size_t xq_dict_payload_size(size_t stored_len);

void xq_dict_payload_store(uint8_t *buf, const xq_dict *d,
                           uint8_t stored_codec, size_t stored_len);

typedef struct {
    uint8_t  kind;
    uint8_t  stored_codec;
    uint32_t id;
    uint64_t raw_size;
    uint64_t stored_size;
    uint64_t xxh64;
} xq_dict_info;

xq_status xq_dict_payload_parse(const uint8_t *buf, size_t len, xq_dict_info *out);

#endif

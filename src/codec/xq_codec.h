/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_CODEC_H
#define XQ_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include "xq.h"

typedef struct xq_codec_vt xq_codec_vt;

struct xq_codec_vt {
    uint8_t     id;
    const char *name;

    size_t (*bound)(size_t src_size);

    void *(*cctx_new)(void);
    void  (*cctx_free)(void *ctx);
    void *(*dctx_new)(void);
    void  (*dctx_free)(void *ctx);

    void *(*cdict_new)(const void *bytes, size_t len, int level, uint32_t block_size);
    void  (*cdict_free)(void *d);
    void *(*ddict_new)(const void *bytes, size_t len);
    void  (*ddict_free)(void *d);

    xq_status (*compress)(void *cctx, void *dst, size_t dst_cap, size_t *out_len,
                          const void *src, size_t src_len, int level,
                          const void *cdict);

    xq_status (*decompress)(void *dctx, void *dst, size_t dst_cap, size_t *out_len,
                            const void *src, size_t src_len, size_t expected_len,
                            const void *ddict);
};

const xq_codec_vt *xq_codec_get(uint8_t id);

int xq_codec_has_dict(const xq_codec_vt *c);

#endif

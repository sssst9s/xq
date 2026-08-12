/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "xq_codec.h"

static size_t stored_bound(size_t src_size)
{
    return src_size;
}

static xq_status stored_compress(void *cctx, void *dst, size_t dst_cap, size_t *out_len,
                                 const void *src, size_t src_len, int level,
                                 const void *cdict)
{
    (void)cctx; (void)level; (void)cdict;
    if (src_len > dst_cap) return XQ_ERR_DST_TOO_SMALL;
    if (src_len) memcpy(dst, src, src_len);
    *out_len = src_len;
    return XQ_OK;
}

static xq_status stored_decompress(void *dctx, void *dst, size_t dst_cap, size_t *out_len,
                                   const void *src, size_t src_len,
                                   size_t expected_len, const void *ddict)
{
    (void)dctx; (void)ddict;

    if (expected_len != src_len) return XQ_ERR_CORRUPT_BLOCK;
    if (src_len > dst_cap) return XQ_ERR_DST_TOO_SMALL;
    if (src_len) memcpy(dst, src, src_len);
    *out_len = src_len;
    return XQ_OK;
}

const xq_codec_vt xq_codec_stored_vt = {
    XQ_CODEC_STORED, "stored",
    stored_bound,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    stored_compress, stored_decompress
};

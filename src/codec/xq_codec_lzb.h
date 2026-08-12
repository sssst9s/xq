/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_CODEC_LZB_H
#define XQ_CODEC_LZB_H

#include <stddef.h>
#include <stdint.h>
#include "xq.h"

#define LZB_MIN_MATCH   4
#define LZB_HLOG_MIN    12
#define LZB_HLOG_MAX    22

size_t    xq_lzb_bound(size_t src_size);

xq_status xq_lzb_compress(void *cctx, void *dst, size_t dst_cap, size_t *out_len,
                          const void *src, size_t src_len, int level,
                          const void *cdict);

xq_status xq_lzb_decompress(void *dctx, void *dst, size_t dst_cap, size_t *out_len,
                            const void *src, size_t src_len, size_t expected_len,
                            const void *ddict);

void *xq_lzb_cctx_new(void);
void  xq_lzb_cctx_free(void *c);
void *xq_lzb_cdict_new(const void *bytes, size_t len, int level, uint32_t block_size);
void  xq_lzb_cdict_free(void *d);
void *xq_lzb_ddict_new(const void *bytes, size_t len);
void  xq_lzb_ddict_free(void *d);

#endif

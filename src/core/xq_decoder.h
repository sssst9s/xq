/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_DECODER_H
#define XQ_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_codec.h"

typedef xq_status (*xq_src_fn)(void *ctx, void *buf, size_t len, size_t *got);

typedef struct xq_decoder xq_decoder;

xq_decoder *xq_decoder_create(xq_src_fn src, void *src_ctx,
                              const xq_params *params, xq_status *out_st);

int64_t xq_decoder_read(xq_decoder *d, void *buf, size_t len);

void xq_decoder_free(xq_decoder *d);

uint64_t xq_decoder_raw_size(const xq_decoder *d);

const xq_file_header *xq_decoder_header(const xq_decoder *d);
uint64_t xq_decoder_stream_checksum(const xq_decoder *d);
int      xq_decoder_has_checksums(const xq_decoder *d);

#endif

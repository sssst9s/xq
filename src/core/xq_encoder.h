/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_ENCODER_H
#define XQ_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_codec.h"
#include "xq_dict.h"

typedef xq_status (*xq_sink_fn)(void *ctx, const void *buf, size_t len);

typedef struct xq_encoder xq_encoder;

xq_encoder *xq_encoder_create(xq_sink_fn sink, void *sink_ctx,
                              const xq_params *params,
                              uint64_t raw_size_hint,
                              const xq_dict *dict,
                              xq_status *out_st);

xq_status xq_encoder_write(xq_encoder *e, const void *buf, size_t len);

xq_status xq_encoder_finish(xq_encoder *e);

void xq_encoder_free(xq_encoder *e);

uint64_t xq_encoder_raw_written(const xq_encoder *e);
uint64_t xq_encoder_file_offset(const xq_encoder *e);

#endif

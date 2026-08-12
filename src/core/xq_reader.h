/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_READER_H
#define XQ_READER_H

#include "xq.h"
#include "xq_format.h"
#include "xq_codec.h"
#include "xq_file.h"

uint32_t xq_reader_block_size(const xq_reader *r);
uint64_t xq_reader_stored_size(const xq_reader *r);

uint64_t xq_reader_dict_size(const xq_reader *r);
const xq_file_header *xq_reader_header(const xq_reader *r);

#endif

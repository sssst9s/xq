/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_FORMAT_H
#define XQ_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include "xq.h"

#define XQ_NAME_B0 'X'
#define XQ_NAME_B1 'Q'
#define XQ_NAME_B2 '1'

#define XQ_MAGIC_SIZE 8
extern const uint8_t xq_magic[XQ_MAGIC_SIZE];

#define XQ_FOOTER_MAGIC 0x54465158U

#define XQ_BLOCK_MAGIC 0xB10CU

#define XQ_FILE_HEADER_SIZE   32
#define XQ_RECORD_HEADER_SIZE 14
#define XQ_BLOCK_HEADER_SIZE  24
#define XQ_FOOTER_SIZE        32
#define XQ_INDEX_ENTRY_SIZE   16
#define XQ_INDEX_PREAMBLE     24

#define XQ_FORMAT_MAJOR 1
#define XQ_FORMAT_MINOR 0

#define XQ_FLAG_INDEX_EXPECTED  (1u << 0)
#define XQ_FLAG_DICT_PRESENT    (1u << 1)
#define XQ_FLAG_UNIFORM_BLOCKS  (1u << 2)
#define XQ_FLAG_STREAM_WRITTEN  (1u << 3)
#define XQ_FLAG_KNOWN_MASK      0x0000000Fu

#define XQ_REC_DICT           0x01
#define XQ_REC_USER_META      0x02
#define XQ_REC_END_OF_BLOCKS  0x10
#define XQ_REC_INDEX          0x11

#define XQ_REC_RESERVED_LO    0x0C

#define XQ_STATIC_ASSERT(cond, name) typedef char xq_static_assert_##name[(cond) ? 1 : -1]
XQ_STATIC_ASSERT(XQ_REC_DICT          != XQ_REC_RESERVED_LO, dict_tag_not_reserved);
XQ_STATIC_ASSERT(XQ_REC_USER_META     != XQ_REC_RESERVED_LO, meta_tag_not_reserved);
XQ_STATIC_ASSERT(XQ_REC_END_OF_BLOCKS != XQ_REC_RESERVED_LO, eob_tag_not_reserved);
XQ_STATIC_ASSERT(XQ_REC_INDEX         != XQ_REC_RESERVED_LO, index_tag_not_reserved);
XQ_STATIC_ASSERT((XQ_BLOCK_MAGIC & 0xFFu) == XQ_REC_RESERVED_LO, reserved_matches_magic);

#define XQ_RFLAG_CRITICAL     (1u << 0)

#define XQ_BFLAG_STORED       (1u << 0)
#define XQ_BFLAG_USES_DICT    (1u << 1)

#define XQ_SIZE_UNKNOWN UINT64_MAX

typedef enum {
    XQ_DICT_KIND_NONE    = 0,
    XQ_DICT_KIND_PREFIX  = 1,
    XQ_DICT_KIND_SAMPLED = 2,
    XQ_DICT_KIND_TRAINED = 3,
} xq_dict_kind;

typedef struct {
    uint8_t  format_major;
    uint8_t  format_minor;
    uint16_t header_size;
    uint32_t flags;
    uint8_t  codec_id;
    uint8_t  level;
    uint8_t  checksum_id;
    uint8_t  block_size_log;
    uint64_t raw_size;
} xq_file_header;

typedef struct {
    uint8_t  tag;
    uint8_t  rflags;
    uint64_t size;
    uint32_t crc32c;
} xq_record_header;

typedef struct {
    uint8_t  bflags;
    uint8_t  codec_id;
    uint32_t stored_size;
    uint32_t raw_size;
    uint64_t raw_offset;
} xq_block_header;

typedef struct {
    uint64_t index_offset;
    uint64_t index_size;
    uint64_t stream_checksum;
} xq_footer;

typedef struct {
    uint64_t block_count;
    uint64_t total_raw;
    uint64_t total_stored;
} xq_index_preamble;

typedef struct {
    uint64_t file_offset;
    uint64_t raw_offset;
} xq_index_entry;

void xq_fmt_header_store(uint8_t *buf, const xq_file_header *h);

xq_status xq_fmt_header_parse(const uint8_t *buf, size_t len, xq_file_header *out);

void xq_fmt_record_store(uint8_t *buf, const xq_record_header *r, const uint8_t *payload);

xq_status xq_fmt_record_parse(const uint8_t *buf, size_t len, xq_record_header *out);

xq_status xq_fmt_record_verify(const uint8_t *hdr, const uint8_t *payload, size_t payload_len);

void xq_fmt_block_store(uint8_t *buf, const xq_block_header *b);

xq_status xq_fmt_block_parse(const uint8_t *buf, size_t len, xq_block_header *out);

size_t xq_fmt_checksum_size(uint8_t checksum_id);

void xq_fmt_index_preamble_store(uint8_t *buf, const xq_index_preamble *p);
xq_status xq_fmt_index_preamble_parse(const uint8_t *buf, size_t len, xq_index_preamble *out);

void xq_fmt_index_entry_store(uint8_t *buf, const xq_index_entry *e);
void xq_fmt_index_entry_load(const uint8_t *buf, xq_index_entry *out);

uint64_t xq_fmt_index_payload_size(uint64_t block_count);

int64_t xq_fmt_index_find(const uint8_t *entries, uint64_t n, uint64_t raw_off);

xq_status xq_fmt_index_validate(const xq_index_preamble *p, const uint8_t *entries,
                                uint64_t file_size);

void xq_fmt_footer_store(uint8_t *buf, const xq_footer *f);

xq_status xq_fmt_footer_parse(const uint8_t *buf, size_t len, xq_footer *out);

#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_H
#define XQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XQ_VERSION_MAJOR 0
#define XQ_VERSION_MINOR 1
#define XQ_VERSION_PATCH 0

typedef enum {
    XQ_OK = 0,

    XQ_ERR_PARAM = 1,
    XQ_ERR_DST_TOO_SMALL,
    XQ_ERR_OOM,
    XQ_ERR_MEMLIMIT,

    XQ_ERR_BAD_MAGIC = 16,
    XQ_ERR_UNSUPPORTED_VERSION,
    XQ_ERR_UNSUPPORTED_CODEC,
    XQ_ERR_UNSUPPORTED_FEATURE,

    XQ_ERR_CORRUPT_HEADER = 32,
    XQ_ERR_CORRUPT_RECORD,
    XQ_ERR_CORRUPT_BLOCK,
    XQ_ERR_CORRUPT_INDEX,
    XQ_ERR_CORRUPT_FOOTER,
    XQ_ERR_TRUNCATED,

    XQ_ERR_IO = 48,
    XQ_ERR_INTERNAL,
} xq_status;

const char *xq_strerror(xq_status st);

int xq_status_is_corruption(xq_status st);

typedef enum {
    XQ_CODEC_STORED = 0,
    XQ_CODEC_LZB    = 1,
    XQ_CODEC_LZE    = 2,
    XQ_CODEC_ZSTD   = 3,
    XQ_CODEC_MAX    = 3,
} xq_codec;

typedef enum {
    XQ_CHECKSUM_NONE   = 0,
    XQ_CHECKSUM_CRC32C = 1,
    XQ_CHECKSUM_XXH64  = 2,
    XQ_CHECKSUM_MAX    = 2,
} xq_checksum;

int xq_codec_available(xq_codec c);

#define XQ_BLOCK_SIZE_MIN       (4u * 1024u)
#define XQ_BLOCK_SIZE_MAX       (256u * 1024u * 1024u)
#define XQ_BLOCK_SIZE_DEFAULT   (64u * 1024u)
#define XQ_DICT_SIZE_MAX        (256u * 1024u * 1024u)
#define XQ_DICT_SIZE_DEFAULT    (8u * 1024u * 1024u)

#define XQ_BLOCK_OVERHEAD_MAX   (24u + 8u)

typedef struct {
    void *(*alloc)(void *ctx, size_t size);
    void  (*free)(void *ctx, void *ptr);
    void *ctx;
} xq_allocator;

typedef struct {
    int         level;
    uint32_t    block_size;
    uint32_t    dict_size;
    xq_codec    codec;
    xq_checksum checksum;
    int         threads;
    uint64_t    mem_limit;
    const xq_allocator *alloc;
} xq_params;

xq_params xq_params_default(void);

xq_status xq_params_check(const xq_params *p, const char **why);

size_t xq_compress_bound(uint64_t src_size, const xq_params *p);

xq_status xq_compress(void *dst, size_t cap, size_t *out_len,
                      const void *src, size_t src_len, const xq_params *p);

xq_status xq_decompress(void *dst, size_t cap, size_t *out_len,
                        const void *src, size_t src_len, uint64_t mem_limit);

typedef struct {
    uint64_t raw_size;
    uint64_t stored_size;
} xq_file_stats;

xq_status xq_compress_file(const char *in_path, const char *out_path,
                           const xq_params *p, xq_file_stats *stats);

xq_status xq_decompress_file(const char *in_path, const char *out_path,
                             uint64_t mem_limit, xq_file_stats *stats);

typedef struct xq_reader xq_reader;

typedef struct {
    uint64_t mem_limit;
    unsigned cache_blocks;
    int      threads;
    const xq_allocator *alloc;
} xq_reader_opts;

xq_reader *xq_reader_open(const char *path, const xq_reader_opts *opts, xq_status *st);
void       xq_reader_close(xq_reader *r);

uint64_t xq_reader_size(const xq_reader *r);
uint64_t xq_reader_block_count(const xq_reader *r);

int64_t xq_reader_pread(xq_reader *r, void *buf, size_t n, uint64_t off);

void xq_reader_cache_stats(const xq_reader *r, uint64_t *hits, uint64_t *misses);

enum {
    XQ_STREAM_CK_ABSENT = 0,
    XQ_STREAM_CK_OK     = 1,
    XQ_STREAM_CK_BAD    = 2,
    XQ_STREAM_CK_SKIP   = 3,
};

typedef struct {
    uint64_t  blocks_total;
    uint64_t  blocks_ok;
    uint64_t  blocks_bad;
    uint64_t  bytes_verified;
    uint64_t  raw_size;
    uint64_t  stored_size;
    uint64_t  first_bad_raw_offset;
    xq_status first_bad_status;
    int       index_ok;
    int       footer_ok;
    int       stream_checksum;
} xq_verify_report;

xq_status xq_reader_verify(xq_reader *r, xq_verify_report *rep);

typedef struct {
    int     fill_gaps;
    uint8_t fill_byte;
} xq_repair_opts;

typedef struct {
    uint64_t blocks_found;
    uint64_t blocks_ok;
    uint64_t blocks_bad;
    uint64_t blocks_skipped;
    uint64_t bytes_recovered;
    uint64_t raw_size_expected;
    uint64_t gaps;
    uint64_t gap_bytes;
    int      dict_ok;
} xq_repair_report;

xq_status xq_repair_file(const char *in_path, const char *out_path,
                         const xq_repair_opts *opts, xq_repair_report *rep);

#ifdef __cplusplus
}
#endif

#endif

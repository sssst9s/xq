/* SPDX-License-Identifier: Apache-2.0 */
#include "xq.h"
#include "xq_format.h"

const char *xq_strerror(xq_status st)
{
    switch (st) {
    case XQ_OK:                        return "ok";
    case XQ_ERR_PARAM:                 return "invalid parameter";
    case XQ_ERR_DST_TOO_SMALL:         return "destination buffer too small";
    case XQ_ERR_OOM:                   return "out of memory";
    case XQ_ERR_MEMLIMIT:              return "would exceed memory limit";
    case XQ_ERR_BAD_MAGIC:             return "not a " "xq" " file";
    case XQ_ERR_UNSUPPORTED_VERSION:   return "unsupported format version";
    case XQ_ERR_UNSUPPORTED_CODEC:     return "unsupported codec";
    case XQ_ERR_UNSUPPORTED_FEATURE:   return "file uses an unsupported feature";
    case XQ_ERR_CORRUPT_HEADER:        return "corrupt file header";
    case XQ_ERR_CORRUPT_RECORD:        return "corrupt metadata record";
    case XQ_ERR_CORRUPT_BLOCK:         return "corrupt block";
    case XQ_ERR_CORRUPT_INDEX:         return "corrupt index";
    case XQ_ERR_CORRUPT_FOOTER:        return "corrupt footer";
    case XQ_ERR_TRUNCATED:             return "file is truncated";
    case XQ_ERR_IO:                    return "I/O error";
    case XQ_ERR_INTERNAL:              return "internal error";
    }
    return "unknown error";
}

int xq_status_is_corruption(xq_status st)
{
    switch (st) {
    case XQ_ERR_CORRUPT_HEADER:
    case XQ_ERR_CORRUPT_RECORD:
    case XQ_ERR_CORRUPT_BLOCK:
    case XQ_ERR_CORRUPT_INDEX:
    case XQ_ERR_CORRUPT_FOOTER:
    case XQ_ERR_TRUNCATED:
        return 1;
    default:
        return 0;
    }
}

int xq_codec_available(xq_codec c)
{
    switch (c) {
    case XQ_CODEC_STORED: return 1;
    case XQ_CODEC_LZB:    return 1;
#ifdef XQ_WITH_ZSTD
    case XQ_CODEC_ZSTD:   return 1;
#endif
    default:              return 0;
    }
}

xq_params xq_params_default(void)
{
    xq_params p;
    p.level      = 6;

    p.block_size = XQ_BLOCK_SIZE_DEFAULT;
    p.dict_size  = XQ_DICT_SIZE_DEFAULT;
    p.codec      = XQ_CODEC_LZB;
    p.checksum   = XQ_CHECKSUM_CRC32C;
    p.threads    = 0;
    p.mem_limit  = 0;
    p.alloc      = NULL;
    return p;
}

static int is_pow2(uint32_t v) { return (v & (v - 1)) == 0; }

xq_status xq_params_check(const xq_params *p, const char **why)
{
    const char *msg = NULL;
    xq_status st = XQ_OK;

    if (!p) { msg = "params is NULL"; st = XQ_ERR_PARAM; goto done; }

    if (p->level < 1 || p->level > 12) {
        msg = "level must be in 1..12"; st = XQ_ERR_PARAM; goto done;
    }

    if (p->block_size != 0) {
        if (p->block_size < XQ_BLOCK_SIZE_MIN) {
            msg = "block_size below 4 KiB"; st = XQ_ERR_PARAM; goto done;
        }
        if (p->block_size > XQ_BLOCK_SIZE_MAX) {
            msg = "block_size above 256 MiB"; st = XQ_ERR_PARAM; goto done;
        }

        if (!is_pow2(p->block_size)) {
            msg = "block_size must be a power of two"; st = XQ_ERR_PARAM; goto done;
        }
    }

    if (p->dict_size > XQ_DICT_SIZE_MAX) {
        msg = "dict_size above 256 MiB"; st = XQ_ERR_PARAM; goto done;
    }

    if ((int)p->codec < 0 || (int)p->codec > XQ_CODEC_MAX) {
        msg = "unknown codec"; st = XQ_ERR_PARAM; goto done;
    }
    if (!xq_codec_available(p->codec)) {
        msg = "codec not available in this build"; st = XQ_ERR_UNSUPPORTED_CODEC; goto done;
    }

    if ((int)p->checksum < 0 || (int)p->checksum > XQ_CHECKSUM_MAX) {
        msg = "unknown checksum"; st = XQ_ERR_PARAM; goto done;
    }

    if (p->threads < 0) { msg = "threads must be >= 0"; st = XQ_ERR_PARAM; goto done; }

    if (p->alloc && (!p->alloc->alloc || !p->alloc->free)) {
        msg = "allocator must supply both alloc and free"; st = XQ_ERR_PARAM; goto done;
    }

    if (p->mem_limit) {
        uint64_t bs = p->block_size ? p->block_size : XQ_BLOCK_SIZE_DEFAULT;
        uint64_t need = (uint64_t)p->dict_size + bs * 2;
        if (p->mem_limit < need) {
            msg = "mem_limit too small for block_size and dict_size";
            st = XQ_ERR_MEMLIMIT; goto done;
        }
    }

done:
    if (why) *why = msg;
    return st;
}

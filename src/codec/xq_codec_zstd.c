/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "xq_codec.h"

static size_t zstd_bound(size_t src_size)
{
    return ZSTD_compressBound(src_size);
}

static void *zstd_cctx_new(void)  { return ZSTD_createCCtx(); }
static void  zstd_cctx_free(void *c) { if (c) ZSTD_freeCCtx((ZSTD_CCtx *)c); }
static void *zstd_dctx_new(void)  { return ZSTD_createDCtx(); }
static void  zstd_dctx_free(void *c) { if (c) ZSTD_freeDCtx((ZSTD_DCtx *)c); }

static int map_level(int level)
{
    if (level < 1) level = 1;
    if (level > 12) level = 12;
    static const int tbl[13] = { 0, 2, 3, 4, 5, 6, 7, 9, 11, 13, 15, 17, 19 };
    return tbl[level];
}

static void *zstd_cdict_new(const void *bytes, size_t len, int level, uint32_t block_size)
{
    int zl = map_level(level);

    ZSTD_compressionParameters cp = ZSTD_getCParams(zl, block_size, len);
    unsigned wlog = 10;
    while (((size_t)1 << wlog) < len + block_size && wlog < ZSTD_WINDOWLOG_MAX) wlog++;
    if (cp.windowLog < wlog) cp.windowLog = wlog;

    return ZSTD_createCDict_advanced(bytes, len, ZSTD_dlm_byRef,
                                     ZSTD_dct_rawContent, cp, ZSTD_defaultCMem);
}

static void zstd_cdict_free(void *d) { if (d) ZSTD_freeCDict((ZSTD_CDict *)d); }

static void *zstd_ddict_new(const void *bytes, size_t len)
{
    return ZSTD_createDDict_advanced(bytes, len, ZSTD_dlm_byRef,
                                     ZSTD_dct_rawContent, ZSTD_defaultCMem);
}

static void zstd_ddict_free(void *d) { if (d) ZSTD_freeDDict((ZSTD_DDict *)d); }

static xq_status zstd_compress(void *cctx, void *dst, size_t dst_cap, size_t *out_len,
                               const void *src, size_t src_len, int level,
                               const void *cdict)
{
    ZSTD_CCtx *c = (ZSTD_CCtx *)cctx;
    if (!c) return XQ_ERR_INTERNAL;

    size_t r;
    if (cdict) {

        ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
        r = ZSTD_CCtx_setParameter(c, ZSTD_c_forceAttachDict, ZSTD_dictForceAttach);
        if (ZSTD_isError(r)) return XQ_ERR_INTERNAL;
        r = ZSTD_CCtx_refCDict(c, (const ZSTD_CDict *)cdict);
        if (ZSTD_isError(r)) return XQ_ERR_INTERNAL;
        r = ZSTD_compress2(c, dst, dst_cap, src, src_len);
    } else {
        ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
        r = ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, map_level(level));
        if (ZSTD_isError(r)) return XQ_ERR_INTERNAL;
        r = ZSTD_compress2(c, dst, dst_cap, src, src_len);
    }

    if (ZSTD_isError(r)) {

        if (ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall)
            return XQ_ERR_DST_TOO_SMALL;
        return XQ_ERR_INTERNAL;
    }

    *out_len = r;
    return XQ_OK;
}

static xq_status zstd_decompress(void *dctx, void *dst, size_t dst_cap, size_t *out_len,
                                 const void *src, size_t src_len, size_t expected_len,
                                 const void *ddict)
{
    ZSTD_DCtx *d = (ZSTD_DCtx *)dctx;
    if (!d) return XQ_ERR_INTERNAL;
    (void)expected_len;

    ZSTD_DCtx_reset(d, ZSTD_reset_session_and_parameters);

    size_t r = ZSTD_DCtx_setParameter(d, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX);
    if (ZSTD_isError(r)) return XQ_ERR_INTERNAL;

    if (ddict) {
        r = ZSTD_DCtx_refDDict(d, (const ZSTD_DDict *)ddict);
        if (ZSTD_isError(r)) return XQ_ERR_INTERNAL;
    }

    r = ZSTD_decompressDCtx(d, dst, dst_cap, src, src_len);
    if (ZSTD_isError(r)) {
        if (ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall)
            return XQ_ERR_DST_TOO_SMALL;

        return XQ_ERR_CORRUPT_BLOCK;
    }

    *out_len = r;
    return XQ_OK;
}

const xq_codec_vt xq_codec_zstd_vt = {
    XQ_CODEC_ZSTD, "zstd",
    zstd_bound,
    zstd_cctx_new, zstd_cctx_free,
    zstd_dctx_new, zstd_dctx_free,
    zstd_cdict_new, zstd_cdict_free,
    zstd_ddict_new, zstd_ddict_free,
    zstd_compress, zstd_decompress
};

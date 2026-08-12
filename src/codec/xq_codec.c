/* SPDX-License-Identifier: Apache-2.0 */
#include "xq_codec.h"

extern const xq_codec_vt xq_codec_stored_vt;
extern const xq_codec_vt xq_codec_lzb_vt;
#ifdef XQ_WITH_ZSTD
extern const xq_codec_vt xq_codec_zstd_vt;
#endif

static const xq_codec_vt *const codecs[] = {
    &xq_codec_stored_vt,
    &xq_codec_lzb_vt,
#ifdef XQ_WITH_ZSTD
    &xq_codec_zstd_vt,
#endif
};

int xq_codec_has_dict(const xq_codec_vt *c)
{
    return c && c->cdict_new && c->ddict_new;
}

const xq_codec_vt *xq_codec_get(uint8_t id)
{
    for (size_t i = 0; i < sizeof codecs / sizeof codecs[0]; i++)
        if (codecs[i]->id == id) return codecs[i];
    return NULL;
}

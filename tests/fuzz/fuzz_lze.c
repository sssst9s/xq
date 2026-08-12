/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_codec.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    enum { OUT = 1 << 16, DICT = 4096 };
    static uint8_t out[OUT];
    static uint8_t dictbuf[DICT];
    static int init;

    if (!init) {
        for (size_t i = 0; i < DICT; i++) dictbuf[i] = (uint8_t)(i * 31u + (i >> 4));
        init = 1;
    }

    const xq_codec_vt *c = xq_codec_get(XQ_CODEC_LZE);
    if (!c) return 0;

    void *dctx = c->dctx_new ? c->dctx_new() : NULL;

    for (int with_dict = 0; with_dict < 2; with_dict++) {
        void *dd = with_dict && c->ddict_new ? c->ddict_new(dictbuf, DICT) : NULL;
        size_t produced = 0;
        xq_status st = c->decompress(dctx, out, sizeof out, &produced,
                                     data, size, sizeof out, dd);
        if (st == XQ_OK) assert(produced <= sizeof out);
        if (dd && c->ddict_free) c->ddict_free(dd);
    }

    if (dctx && c->dctx_free) c->dctx_free(dctx);
    return 0;
}

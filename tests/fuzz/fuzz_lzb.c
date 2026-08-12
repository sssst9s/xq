/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "xq.h"
#include "xq_codec_lzb.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    enum { OUT = 1 << 16, DICT = 4096 };
    static uint8_t out[OUT];
    static uint8_t dictbuf[DICT];
    static int dict_init;

    if (!dict_init) {
        for (size_t i = 0; i < DICT; i++) dictbuf[i] = (uint8_t)(i * 31u + (i >> 4));
        dict_init = 1;
    }

    for (int with_dict = 0; with_dict < 2; with_dict++) {
        void *dd = with_dict ? xq_lzb_ddict_new(dictbuf, DICT) : NULL;
        size_t produced = 0;
        xq_status st = xq_lzb_decompress(NULL, out, sizeof out, &produced,
                                         data, size, sizeof out, dd);
        if (st == XQ_OK) assert(produced <= sizeof out);
        if (dd) xq_lzb_ddict_free(dd);
    }
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "xq.h"
#include "xq_format.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{

    {
        xq_file_header h;
        if (xq_fmt_header_parse(data, size, &h) == XQ_OK) {
            assert(h.format_major == XQ_FORMAT_MAJOR);
            assert(h.header_size >= XQ_FILE_HEADER_SIZE);
            assert(h.checksum_id <= XQ_CHECKSUM_MAX);
            assert((h.flags & ~XQ_FLAG_KNOWN_MASK) == 0);
            if (h.block_size_log) {

                assert(h.block_size_log >= 12 && h.block_size_log <= 28);
                uint32_t bs = 1u << h.block_size_log;
                assert(bs >= XQ_BLOCK_SIZE_MIN && bs <= XQ_BLOCK_SIZE_MAX);
            } else {
                assert((h.flags & XQ_FLAG_UNIFORM_BLOCKS) == 0);
            }
        }
    }

    {
        xq_block_header b;
        if (xq_fmt_block_parse(data, size, &b) == XQ_OK) {
            assert(b.raw_size > 0);
            assert(b.raw_size <= XQ_BLOCK_SIZE_MAX);

            assert(b.stored_size <= b.raw_size + (b.raw_size / 8) + 1024);
            if (b.bflags & XQ_BFLAG_STORED) assert(b.stored_size == b.raw_size);
            assert(b.raw_offset <= UINT64_MAX - b.raw_size);
        }
    }

    {
        xq_footer f;
        if (xq_fmt_footer_parse(data, size, &f) == XQ_OK) {
            if (f.index_offset == 0) assert(f.index_size == 0);
            else {
                assert(f.index_offset >= XQ_FILE_HEADER_SIZE);
                assert(f.index_size >= XQ_RECORD_HEADER_SIZE + XQ_INDEX_PREAMBLE);
            }
        }
    }

    {
        xq_record_header r;
        if (xq_fmt_record_parse(data, size, &r) == XQ_OK) {
            assert(r.size <= (uint64_t)1 << 40);
            size_t avail = size - XQ_RECORD_HEADER_SIZE;
            if (r.size <= avail)
                (void)xq_fmt_record_verify(data, data + XQ_RECORD_HEADER_SIZE,
                                           (size_t)r.size);
        }
    }

    {
        xq_index_preamble p;
        if (xq_fmt_index_preamble_parse(data, size, &p) == XQ_OK) {
            uint64_t need = xq_fmt_index_payload_size(p.block_count);
            if (need && need <= size) {
                const uint8_t *ent = data + XQ_INDEX_PREAMBLE;
                if (xq_fmt_index_validate(&p, ent, 0) == XQ_OK) {
                    assert(p.block_count > 0);

                    int64_t i = xq_fmt_index_find(ent, p.block_count + 1, 0);
                    assert(i == 0);
                    if (p.total_raw > 0) {
                        i = xq_fmt_index_find(ent, p.block_count + 1, p.total_raw - 1);
                        assert(i >= 0 && (uint64_t)i < p.block_count);
                    }
                    i = xq_fmt_index_find(ent, p.block_count + 1, p.total_raw);
                    assert(i == -1);
                }
            }
        }
    }

    return 0;
}

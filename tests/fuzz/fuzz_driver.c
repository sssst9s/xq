/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_bits.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#if defined(__clang__) || defined(__GNUC__)
#  define XQ_NO_UINT_OVERFLOW_CHECK __attribute__((no_sanitize("unsigned-integer-overflow")))
#else
#  define XQ_NO_UINT_OVERFLOW_CHECK
#endif

static uint64_t rs;
XQ_NO_UINT_OVERFLOW_CHECK
static uint64_t rnd(void)
{
    uint64_t x = rs; x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rs = x; return x * 0x2545F4914F6CDD1DULL;
}
static uint32_t rnd_below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

static size_t make_input(uint8_t *buf, size_t cap)
{
    memset(buf, 0, cap);
    size_t len = 0;

    switch (rnd_below(5)) {
    case 0: {
        xq_file_header h;
        h.format_major   = (uint8_t)(rnd_below(4) ? XQ_FORMAT_MAJOR : (uint8_t)rnd_below(256));
        h.format_minor   = (uint8_t)rnd_below(4);
        h.header_size    = (uint16_t)(rnd_below(4) ? XQ_FILE_HEADER_SIZE : (uint16_t)rnd_below(65536));
        h.flags          = rnd_below(4) ? (rnd() & XQ_FLAG_KNOWN_MASK) : (uint32_t)rnd();
        h.codec_id       = (uint8_t)rnd_below(6);
        h.level          = (uint8_t)rnd_below(16);
        h.checksum_id    = (uint8_t)(rnd_below(4) ? rnd_below(3) : (uint8_t)rnd_below(256));
        h.block_size_log = (uint8_t)(rnd_below(4) ? (uint8_t)(12 + rnd_below(17)) : (uint8_t)rnd_below(64));
        h.raw_size       = rnd_below(2) ? rnd() : XQ_SIZE_UNKNOWN;
        xq_fmt_header_store(buf, &h);
        len = XQ_FILE_HEADER_SIZE;
        break;
    }
    case 1: {
        xq_block_header b;
        b.bflags      = (uint8_t)rnd_below(4);
        b.codec_id    = (uint8_t)rnd_below(6);
        b.raw_size    = rnd_below(4) ? (1u + rnd_below(XQ_BLOCK_SIZE_MAX)) : (uint32_t)rnd();
        b.stored_size = rnd_below(4) ? rnd_below(b.raw_size ? b.raw_size : 1u) : (uint32_t)rnd();
        b.raw_offset  = rnd_below(2) ? rnd_below(1u << 30) : rnd();
        if (b.bflags & XQ_BFLAG_STORED) b.stored_size = b.raw_size;
        xq_fmt_block_store(buf, &b);
        len = XQ_BLOCK_HEADER_SIZE;
        break;
    }
    case 2: {
        xq_footer f;
        f.index_offset    = rnd_below(2) ? rnd_below(1u << 30) : rnd();
        f.index_size      = rnd_below(2) ? rnd_below(1u << 20) : rnd();
        f.stream_checksum = rnd();
        xq_fmt_footer_store(buf, &f);
        len = XQ_FOOTER_SIZE;
        break;
    }
    case 3: {
        size_t plen = rnd_below(64);
        for (size_t i = 0; i < plen; i++) buf[XQ_RECORD_HEADER_SIZE + i] = (uint8_t)rnd();
        xq_record_header r;
        r.tag    = (uint8_t)rnd_below(0x20);
        r.rflags = (uint8_t)rnd_below(4);
        r.size   = rnd_below(4) ? plen : rnd();
        r.crc32c = 0;

        uint64_t save = r.size;
        r.size = plen;
        xq_fmt_record_store(buf, &r, buf + XQ_RECORD_HEADER_SIZE);
        if (save != plen) xq_st64le(buf + 2, save);
        len = XQ_RECORD_HEADER_SIZE + plen;
        break;
    }
    default: {

        uint64_t nblk = 1 + rnd_below(24);
        xq_index_preamble p;
        p.block_count  = rnd_below(8) ? nblk : rnd();
        p.total_raw    = 0;
        p.total_stored = 0;
        xq_fmt_index_preamble_store(buf, &p);
        len = XQ_INDEX_PREAMBLE;
        uint64_t fo = XQ_FILE_HEADER_SIZE, ro = 0;
        for (uint64_t i = 0; i <= nblk && len + XQ_INDEX_ENTRY_SIZE <= cap; i++) {
            xq_index_entry e = { fo, ro };
            xq_fmt_index_entry_store(buf + len, &e);
            len += XQ_INDEX_ENTRY_SIZE;
            fo += XQ_BLOCK_HEADER_SIZE + 1 + rnd_below(4096);
            ro += 1 + rnd_below(65536);
        }
        xq_st64le(buf + 8, ro);
        if (rnd_below(4) == 0) xq_st64le(buf + 8, rnd());
        break;
    }
    }

    uint32_t nmut = rnd_below(4);
    for (uint32_t i = 0; i < nmut && len; i++)
        buf[rnd_below((uint32_t)len)] ^= (uint8_t)(1u << rnd_below(8));

    if (rnd_below(8) == 0 && len) len = rnd_below((uint32_t)len);

    return len;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            FILE *fp = fopen(argv[i], "rb");
            if (!fp) { fprintf(stderr, "cannot open %s\n", argv[i]); return 1; }
            static uint8_t buf[1 << 20];
            size_t n = fread(buf, 1, sizeof buf, fp);
            fclose(fp);
            LLVMFuzzerTestOneInput(buf, n);
            printf("ok %s (%zu bytes)\n", argv[i], n);
        }
        return 0;
    }

    const char *env = getenv("XQ_FUZZ_ITERS");
    unsigned long iters = env ? strtoul(env, NULL, 10) : 200000;
    env = getenv("XQ_FUZZ_SEED");
    rs = env ? strtoull(env, NULL, 10) : 0x243F6A8885A308D3ULL;
    if (rs == 0) rs = 1;

    static uint8_t buf[4096];
    for (unsigned long i = 0; i < iters; i++) {
        size_t n = make_input(buf, sizeof buf);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("%lu iterations, no crashes (seed %llu)\n", iters, (unsigned long long)rs);
    return 0;
}

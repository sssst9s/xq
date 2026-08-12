/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "xq.h"
#include "xq_format.h"
#include "xq_file.h"
#include "xq_bits.h"
#include "xq_checked.h"
#include "xq_reader.h"

#define PROG "xq"

static int usage(FILE *fp, int code)
{
    fprintf(fp,
"usage: " PROG " <command> [options] [file]\n"
"\n"
"commands:\n"
"  compress    [-o out] [-b size] [-l level] [-c codec] [-D size] [-T n] [in]\n"
"  decompress  [-o out] [-M limit] [in]\n"
"  info        [in]\n"
"  verify      [-q] [in]\n"
"  extract     -a OFF [-n LEN] [-o out] [in]\n"
"  repair      [-o out] [-f[BYTE]] [in]\n"
"\n"
"options:\n"
"  -o PATH     output file ('-' for stdout, the default)\n"
"  -b SIZE     block size, power of two, 4K..256M (default 64K)\n"
"  -l N        compression level 1..12 (default 6)\n"
"  -c NAME     codec: stored, lzb, zstd (default lzb)\n"
"  -D SIZE     shared dictionary size, 0 disables (default 8M)\n"
"  -T N        worker threads, 1 disables, 0 auto (default 0)\n"
"  -C NAME     block checksum: none, crc32c, xxh64 (default crc32c)\n"
"  -M SIZE     memory limit for decompression\n"
"  -a OFF      byte offset into the *uncompressed* stream\n"
"  -n LEN      number of bytes to extract (default: to end)\n"
"  -q          quiet: report only via exit status\n"
"  -f[BYTE]    repair: fill unrecoverable gaps with BYTE (default 0)\n"
"  -h          this help\n"
"\n"
"sizes accept K/M/G suffixes. an input of '-' or none means stdin.\n"
"\n"
"note: only the 'stored' codec exists so far, so 'compress' does not yet\n"
"      reduce size. it exercises the container end to end.\n");
    return code;
}

static int parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s) return -1;

    uint64_t mult = 1;
    if (*end) {
        switch (*end) {
        case 'k': case 'K': mult = 1024ull; break;
        case 'm': case 'M': mult = 1024ull * 1024; break;
        case 'g': case 'G': mult = 1024ull * 1024 * 1024; break;
        default: return -1;
        }
        end++;
        if (*end == 'b' || *end == 'B') end++;
        if (*end) return -1;
    }
    if (v != 0 && mult > UINT64_MAX / v) return -1;
    *out = (uint64_t)v * mult;
    return 0;
}

static void human(uint64_t n, char *buf, size_t cap)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double d = (double)n;
    size_t i = 0;
    while (d >= 1024.0 && i + 1 < sizeof u / sizeof u[0]) { d /= 1024.0; i++; }
    if (i == 0) snprintf(buf, cap, "%" PRIu64 " B", n);
    else snprintf(buf, cap, "%.1f %s", d, u[i]);
}

static int cmd_compress(int argc, char **argv)
{
    const char *out = "-", *in = "-";
    xq_params p = xq_params_default();

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) { out = argv[++i]; }
        else if (!strcmp(a, "-b") && i + 1 < argc) {
            uint64_t v;
            if (parse_size(argv[++i], &v) || v > XQ_BLOCK_SIZE_MAX) {
                fprintf(stderr, PROG ": bad block size\n"); return 2;
            }
            p.block_size = (uint32_t)v;
        }
        else if (!strcmp(a, "-l") && i + 1 < argc) { p.level = atoi(argv[++i]); }
        else if (!strcmp(a, "-T") && i + 1 < argc) {
            p.threads = atoi(argv[++i]);
            if (p.threads < 0) { fprintf(stderr, PROG ": bad thread count\n"); return 2; }
        }
        else if (!strcmp(a, "-c") && i + 1 < argc) {
            const char *c = argv[++i];
            if (!strcmp(c, "stored")) p.codec = XQ_CODEC_STORED;
            else if (!strcmp(c, "lzb")) p.codec = XQ_CODEC_LZB;
            else if (!strcmp(c, "zstd")) p.codec = XQ_CODEC_ZSTD;
            else { fprintf(stderr, PROG ": unknown codec '%s'\n", c); return 2; }
            if (!xq_codec_available(p.codec)) {
                fprintf(stderr, PROG ": codec '%s' not in this build "
                                "(rebuild with WITH_ZSTD=1)\n", c);
                return 2;
            }
        }
        else if (!strcmp(a, "-D") && i + 1 < argc) {
            uint64_t v;
            if (parse_size(argv[++i], &v) || v > XQ_DICT_SIZE_MAX) {
                fprintf(stderr, PROG ": bad dictionary size\n"); return 2;
            }
            p.dict_size = (uint32_t)v;
        }
        else if (!strcmp(a, "-C") && i + 1 < argc) {
            const char *c = argv[++i];
            if (!strcmp(c, "none")) p.checksum = XQ_CHECKSUM_NONE;
            else if (!strcmp(c, "crc32c")) p.checksum = XQ_CHECKSUM_CRC32C;
            else if (!strcmp(c, "xxh64")) p.checksum = XQ_CHECKSUM_XXH64;
            else { fprintf(stderr, PROG ": unknown checksum '%s'\n", c); return 2; }
        }
        else if (!strcmp(a, "-h")) return usage(stdout, 0);
        else if (a[0] == '-' && a[1]) { fprintf(stderr, PROG ": unknown option %s\n", a); return 2; }
        else in = a;
    }

    const char *why = NULL;
    xq_status st = xq_params_check(&p, &why);
    if (st != XQ_OK) {
        fprintf(stderr, PROG ": %s%s%s\n", xq_strerror(st), why ? ": " : "", why ? why : "");
        return 2;
    }

    xq_file_stats stats;
    st = xq_compress_file(in, out, &p, &stats);
    if (st != XQ_OK) {
        fprintf(stderr, PROG ": compress: %s\n", xq_strerror(st));
        return 1;
    }

    if (strcmp(out, "-") != 0) {
        char a[32], b[32];
        human(stats.raw_size, a, sizeof a);
        human(stats.stored_size, b, sizeof b);
        fprintf(stderr, "%s -> %s (%.3fx)\n", a, b,
                stats.stored_size ? (double)stats.raw_size / (double)stats.stored_size : 0.0);
    }
    return 0;
}

static int cmd_decompress(int argc, char **argv)
{
    const char *out = "-", *in = "-";
    uint64_t mem_limit = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "-M") && i + 1 < argc) {
            if (parse_size(argv[++i], &mem_limit)) {
                fprintf(stderr, PROG ": bad memory limit\n"); return 2;
            }
        }
        else if (!strcmp(a, "-h")) return usage(stdout, 0);
        else if (a[0] == '-' && a[1]) { fprintf(stderr, PROG ": unknown option %s\n", a); return 2; }
        else in = a;
    }

    xq_file_stats stats;
    xq_status st = xq_decompress_file(in, out, mem_limit, &stats);
    if (st != XQ_OK) {
        fprintf(stderr, PROG ": decompress: %s\n", xq_strerror(st));
        return xq_status_is_corruption(st) ? 3 : 1;
    }
    return 0;
}

static int cmd_repair(int argc, char **argv)
{
    const char *path = "-", *out = NULL;
    xq_repair_opts o;
    memset(&o, 0, sizeof o);

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) out = argv[++i];
        else if (!strncmp(a, "-f", 2)) {
            o.fill_gaps = 1;
            if (a[2]) o.fill_byte = (uint8_t)strtoul(a + 2, NULL, 0);
        }
        else if (!strcmp(a, "-h")) return usage(stdout, 0);
        else if (a[0] == '-' && a[1]) { fprintf(stderr, PROG ": unknown option %s\n", a); return 2; }
        else path = a;
    }

    xq_repair_report rep;
    xq_status st = xq_repair_file(path, out, &o, &rep);

    if (st != XQ_OK && !xq_status_is_corruption(st)) {
        fprintf(stderr, PROG ": %s: %s\n", path, xq_strerror(st));
        return 1;
    }

    char a[32];
    human(rep.bytes_recovered, a, sizeof a);
    fprintf(stderr, "blocks found    %" PRIu64 "\n", rep.blocks_found);
    fprintf(stderr, "blocks ok       %" PRIu64 "\n", rep.blocks_ok);
    if (rep.blocks_bad)     fprintf(stderr, "blocks damaged  %" PRIu64 "\n", rep.blocks_bad);
    if (rep.blocks_skipped) fprintf(stderr, "blocks skipped  %" PRIu64 " (out of order)\n", rep.blocks_skipped);
    fprintf(stderr, "recovered       %" PRIu64 " bytes (%s)\n", rep.bytes_recovered, a);
    if (rep.raw_size_expected) {
        human(rep.raw_size_expected, a, sizeof a);
        fprintf(stderr, "original size   %s (%.2f%% recovered)\n", a,
                100.0 * (double)rep.bytes_recovered / (double)rep.raw_size_expected);
    }
    if (rep.gaps) fprintf(stderr, "gaps            %" PRIu64 " totalling %" PRIu64 " bytes%s\n",
                          rep.gaps, rep.gap_bytes, o.fill_gaps ? " (filled)" : " (omitted)");
    if (!rep.dict_ok) fprintf(stderr, "dictionary      DAMAGED: blocks needing it are unrecoverable\n");
    if (!out) fprintf(stderr, "\n(no -o given, so nothing was written)\n");

    return rep.blocks_bad ? 3 : 0;
}

static int cmd_verify(int argc, char **argv)
{
    const char *path = "-";
    int quiet = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-h")) return usage(stdout, 0);
        if (!strcmp(argv[i], "-q")) { quiet = 1; continue; }
        path = argv[i];
    }

    xq_status st;
    xq_reader *r = xq_reader_open(path, NULL, &st);
    if (!r) {
        if (!quiet) fprintf(stderr, PROG ": %s: %s\n", path, xq_strerror(st));
        return xq_status_is_corruption(st) ? 3 : 1;
    }

    xq_verify_report rep;
    st = xq_reader_verify(r, &rep);
    xq_reader_close(r);

    if (!quiet) {
        char a[32];
        human(rep.raw_size, a, sizeof a);
        printf("blocks          %" PRIu64 " ok", rep.blocks_ok);
        if (rep.blocks_bad) printf(", %" PRIu64 " BAD", rep.blocks_bad);
        printf(" (of %" PRIu64 ")\n", rep.blocks_total);
        printf("raw size        %s\n", a);
        printf("index           %s\n", rep.index_ok ? "ok" : "BAD");
        printf("footer          %s\n", rep.footer_ok ? "ok" : "BAD");
        switch (rep.stream_checksum) {
        case XQ_STREAM_CK_OK:   printf("stream checksum ok\n"); break;
        case XQ_STREAM_CK_BAD:  printf("stream checksum MISMATCH\n"); break;
        case XQ_STREAM_CK_SKIP: printf("stream checksum skipped (damaged blocks)\n"); break;
        default:                printf("stream checksum absent\n"); break;
        }
        if (rep.blocks_bad) {
            printf("first bad at    raw offset %" PRIu64 " (%s)\n",
                   rep.first_bad_raw_offset, xq_strerror(rep.first_bad_status));
            printf("recoverable     %" PRIu64 " of %" PRIu64 " bytes\n",
                   rep.bytes_verified, rep.raw_size);
        }
    }

    if (rep.blocks_bad || rep.stream_checksum == XQ_STREAM_CK_BAD) return 3;
    return st == XQ_OK ? 0 : 3;
}

static int cmd_extract(int argc, char **argv)
{
    const char *path = "-", *out = "-";
    uint64_t off = 0, len = 0;
    int have_len = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-a") && i + 1 < argc) {
            if (parse_size(argv[++i], &off)) { fprintf(stderr, PROG ": bad offset\n"); return 2; }
        } else if (!strcmp(a, "-n") && i + 1 < argc) {
            if (parse_size(argv[++i], &len)) { fprintf(stderr, PROG ": bad length\n"); return 2; }
            have_len = 1;
        } else if (!strcmp(a, "-o") && i + 1 < argc) { out = argv[++i]; }
        else if (!strcmp(a, "-h")) return usage(stdout, 0);
        else if (a[0] == '-' && a[1]) { fprintf(stderr, PROG ": unknown option %s\n", a); return 2; }
        else path = a;
    }

    xq_status st;
    xq_reader *r = xq_reader_open(path, NULL, &st);
    if (!r) {
        fprintf(stderr, PROG ": %s: %s\n", path, xq_strerror(st));
        return xq_status_is_corruption(st) ? 3 : 1;
    }

    uint64_t total = xq_reader_size(r);
    if (off > total) {
        fprintf(stderr, PROG ": offset %" PRIu64 " past end of stream (%" PRIu64 ")\n", off, total);
        xq_reader_close(r);
        return 2;
    }
    if (!have_len) len = total - off;

    xq_file of;
    if ((st = xq_file_open_write(&of, out)) != XQ_OK) {
        fprintf(stderr, PROG ": %s: %s\n", out, xq_strerror(st));
        xq_reader_close(r);
        return 1;
    }

    enum { CHUNK = 1u << 20 };
    static uint8_t buf[CHUNK];
    uint64_t done = 0;
    int rc = 0;

    while (done < len) {
        size_t want = (len - done) < CHUNK ? (size_t)(len - done) : CHUNK;
        int64_t got = xq_reader_pread(r, buf, want, off + done);
        if (got < 0) {
            st = (xq_status)(-got);
            fprintf(stderr, PROG ": read at %" PRIu64 ": %s\n", off + done, xq_strerror(st));
            rc = xq_status_is_corruption(st) ? 3 : 1;
            break;
        }
        if (got == 0) break;
        if ((st = xq_file_write(&of, buf, (size_t)got)) != XQ_OK) {
            fprintf(stderr, PROG ": write: %s\n", xq_strerror(st));
            rc = 1;
            break;
        }
        done += (uint64_t)got;
    }

    xq_file_close(&of);
    xq_reader_close(r);
    return rc;
}

static int cmd_info(int argc, char **argv)
{
    const char *path = "-";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-h")) return usage(stdout, 0);
        path = argv[i];
    }

    xq_file f;
    xq_status st = xq_file_open_read(&f, path);
    if (st != XQ_OK) { fprintf(stderr, PROG ": %s: %s\n", path, xq_strerror(st)); return 1; }

    if (!f.seekable) {
        fprintf(stderr, PROG ": info needs a seekable file\n");
        xq_file_close(&f);
        return 2;
    }

    uint64_t fsize = 0;
    if ((st = xq_file_size(&f, &fsize)) != XQ_OK) goto io_err;

    uint8_t hbuf[XQ_FILE_HEADER_SIZE];
    size_t got = 0;
    if ((st = xq_file_pread(&f, hbuf, sizeof hbuf, 0, &got)) != XQ_OK) goto io_err;
    if (got < sizeof hbuf) { st = XQ_ERR_TRUNCATED; goto err; }

    xq_file_header h;
    if ((st = xq_fmt_header_parse(hbuf, sizeof hbuf, &h)) != XQ_OK) goto err;

    static const char *ck[] = { "none", "crc32c", "xxh64" };
    char sz[32];

    printf("file            %s\n", strcmp(path, "-") ? path : "(stdin)");
    human(fsize, sz, sizeof sz);
    printf("stored size     %" PRIu64 " (%s)\n", fsize, sz);
    printf("format version  %u.%u\n", h.format_major, h.format_minor);
    static const char *codecs[] = { "stored", "lzb", "lze", "zstd" };
    printf("codec           %s%s\n",
           h.codec_id < 4 ? codecs[h.codec_id] : "?",
           xq_codec_available((xq_codec)h.codec_id) ? "" : " (NOT IN THIS BUILD)");
    printf("level           %u\n", h.level);
    printf("checksum        %s\n", h.checksum_id < 3 ? ck[h.checksum_id] : "?");
    if (h.block_size_log) {
        human(1u << h.block_size_log, sz, sizeof sz);
        printf("block size      %s\n", sz);
    } else {
        printf("block size      variable\n");
    }
    if (h.raw_size == XQ_SIZE_UNKNOWN) {
        printf("raw size        unknown (stream-written)\n");
    } else {
        human(h.raw_size, sz, sizeof sz);
        printf("raw size        %" PRIu64 " (%s)\n", h.raw_size, sz);
        if (fsize) printf("ratio           %.3fx\n", (double)h.raw_size / (double)fsize);
    }
    printf("flags          ");
    if (h.flags & XQ_FLAG_INDEX_EXPECTED) printf(" index");
    if (h.flags & XQ_FLAG_DICT_PRESENT)   printf(" dict");
    if (h.flags & XQ_FLAG_UNIFORM_BLOCKS) printf(" uniform-blocks");
    if (h.flags & XQ_FLAG_STREAM_WRITTEN) printf(" stream-written");
    if (!h.flags) printf(" none");
    printf("\n");

    if (fsize < XQ_FILE_HEADER_SIZE + XQ_FOOTER_SIZE) {
        printf("footer          missing (file too short)\n");
        goto done;
    }

    uint8_t fbuf[XQ_FOOTER_SIZE];
    if ((st = xq_file_pread(&f, fbuf, sizeof fbuf, fsize - XQ_FOOTER_SIZE, &got)) != XQ_OK)
        goto io_err;

    xq_footer ft;
    st = xq_fmt_footer_parse(fbuf, sizeof fbuf, &ft);
    if (st != XQ_OK) {
        printf("footer          %s -- file is truncated or damaged\n", xq_strerror(st));
        goto done;
    }

    if (ft.index_offset == 0) {
        printf("index           absent\n");
        goto done;
    }

    if (!xq_range_ok(ft.index_offset, ft.index_size, fsize - XQ_FOOTER_SIZE)) {
        printf("index           corrupt (offset/size outside file)\n");
        goto done;
    }

    uint8_t irec[XQ_RECORD_HEADER_SIZE + XQ_INDEX_PREAMBLE];
    if ((st = xq_file_pread(&f, irec, sizeof irec, ft.index_offset, &got)) != XQ_OK)
        goto io_err;
    if (got < sizeof irec) { printf("index           truncated\n"); goto done; }

    xq_index_preamble ip;
    st = xq_fmt_index_preamble_parse(irec + XQ_RECORD_HEADER_SIZE, XQ_INDEX_PREAMBLE, &ip);
    if (st != XQ_OK) { printf("index           %s\n", xq_strerror(st)); goto done; }

    printf("index           %" PRIu64 " blocks at offset %" PRIu64 "\n",
           ip.block_count, ft.index_offset);

    if (h.flags & XQ_FLAG_DICT_PRESENT) {
        xq_status rs;
        xq_reader *rr = xq_reader_open(path, NULL, &rs);
        if (rr) {
            human(xq_reader_dict_size(rr), sz, sizeof sz);
            printf("dictionary      %s\n", sz);
            xq_reader_close(rr);
        } else {
            printf("dictionary      present but unreadable (%s)\n", xq_strerror(rs));
        }
    } else {
        printf("dictionary      none\n");
    }
    if (ip.block_count) {
        human(ip.total_raw / ip.block_count, sz, sizeof sz);
        printf("avg block raw   %s\n", sz);
    }

done:
    xq_file_close(&f);
    return 0;

io_err:
err:
    fprintf(stderr, PROG ": %s: %s\n", path, xq_strerror(st));
    xq_file_close(&f);
    return xq_status_is_corruption(st) ? 3 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage(stderr, 2);

    const char *cmd = argv[1];
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help"))
        return usage(stdout, 0);

    if (!strcmp(cmd, "compress")   || !strcmp(cmd, "c")) return cmd_compress(argc - 2, argv + 2);
    if (!strcmp(cmd, "decompress") || !strcmp(cmd, "d")) return cmd_decompress(argc - 2, argv + 2);
    if (!strcmp(cmd, "info")       || !strcmp(cmd, "i")) return cmd_info(argc - 2, argv + 2);
    if (!strcmp(cmd, "verify")     || !strcmp(cmd, "v")) return cmd_verify(argc - 2, argv + 2);
    if (!strcmp(cmd, "extract")    || !strcmp(cmd, "x")) return cmd_extract(argc - 2, argv + 2);
    if (!strcmp(cmd, "repair")     || !strcmp(cmd, "r")) return cmd_repair(argc - 2, argv + 2);

    fprintf(stderr, PROG ": unknown command '%s'\n", cmd);
    return usage(stderr, 2);
}

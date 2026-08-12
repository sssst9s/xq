# Usage

Everything the command line tool and the C library can do, with the reasoning
where it affects what you should pick.

- [Install](#install)
- [Commands](#commands)
- [Options](#options)
- [Choosing a block size](#choosing-a-block-size)
- [The dictionary](#the-dictionary)
- [Threads](#threads)
- [Damaged files](#damaged-files)
- [Exit codes](#exit-codes)
- [C library](#c-library)
- [file(1) integration](#file1-integration)

## Install

```sh
git clone https://github.com/sssst9s/xq
cd xq
make                    # static library, CLI and tests
./build/xq --help
```

The default build compresses with `lzb` and needs nothing else. An optional
reference codec can be built in for comparison:

```sh
make WITH_ZSTD=1
./build/with-zstd/xq compress -c zstd -o out.xq file
```

Copy `build/xq` onto your PATH; there is no install target yet.

### Codecs

| codec | dependencies | ratio | notes |
|---|---|---|---|
| `lzb` | none | 6.7x | the default; LZ77 with lazy matching |
| `stored` | none | 1.0x | no compression; framing and checksums only |
| `zstd` | libzstd | 9.5x | optional, build-time, for comparison |

Ratios are for a source tree at 64 KiB blocks with an 8 MiB dictionary.
`stored` is useful when the data is already compressed, or to isolate a
container problem from a codec problem.

Verify a build with `make check`, which runs the tests, the sanitiser and the
fuzzer.

## Commands

### compress

```sh
xq compress [-o out] [-b size] [-l level] [-c codec] [-D size] [-T n] [-C name] [in]
```

```sh
xq compress -o data.xq data.bin
xq compress -D 8M -o data.xq data.bin               # with a dictionary
cat data.bin | xq compress > data.xq                # from a pipe
```

Input defaults to stdin and output to stdout, so `xq` works in a pipeline. A
file written to a pipe is byte-identical to one written to disk apart from the
header fields that must differ, and is fully seekable either way, because the
encoder never seeks backwards.

### decompress

```sh
xq decompress [-o out] [-M limit] [in]
```

```sh
xq decompress -o data.bin data.xq
xq decompress -M 64M -o data.bin untrusted.xq       # cap decoder memory
```

Verifies every block checksum, and the whole-stream checksum too when the
input is a real file rather than a pipe.

### extract

Reads a byte range out of the middle without decompressing what precedes it.
This is what the format exists for.

```sh
xq extract -a OFFSET [-n LENGTH] [-o out] [in]
```

```sh
xq extract -a 100000000 -n 4096 -o piece.bin data.xq
xq extract -a 5G -n 1M data.xq | head -c 200        # suffixes work
```

`-a` is an offset into the **uncompressed** stream. Omit `-n` to read to the
end. Cost is one index lookup plus the blocks the range actually touches,
independent of file size.

### verify

```sh
xq verify [-q] [in]
```

Decodes and checks every block. It does not stop at the first failure, because
with independent blocks how much survived is the useful answer:

```
blocks          4095 ok, 1 BAD (of 4096)
raw size        256.0 MiB
index           ok
footer          ok
stream checksum skipped (damaged blocks)
first bad at    raw offset 49938432 (corrupt block)
recoverable     268369920 of 268435456 bytes
```

### info

```sh
xq info [in]
```

Reads structure without decoding payloads, and reports what it can even when
the file is damaged, which is when it is most useful.

### repair

```sh
xq repair [-o out] [-f[BYTE]] [in]
```

```sh
xq repair -o recovered.bin damaged.xq       # skip unrecoverable ranges
xq repair -f0 -o recovered.bin damaged.xq   # zero-fill them instead
```

Scans for block headers and confirms each with its CRC, so it works even when
the index and footer are gone entirely, i.e. exactly when `decompress` and
`extract` cannot open the file at all. Every block header records its own
offset in the uncompressed stream, so recovered data lands in the right place.

Without `-o` it reports what could be recovered and writes nothing.

**`-f` matters if offsets matter.** Skipping a damaged range shifts everything
after it; filling keeps every surviving byte at its original offset, which is
what you want for a disk image or a database file.

## Options

| option | commands | meaning |
|---|---|---|
| `-o PATH` | most | output file; `-` means stdout (the default) |
| `-b SIZE` | compress | block size, power of two, 4K to 256M (default 64K) |
| `-l N` | compress | compression level 1 to 12 (default 6) |
| `-c NAME` | compress | codec: `lzb`, `stored`, `zstd` (default `lzb`) |
| `-D SIZE` | compress | shared dictionary size, `0` disables (default 8M) |
| `-T N` | compress | worker threads, `1` disables, `0` auto (default 0) |
| `-C NAME` | compress | block checksum: `none`, `crc32c`, `xxh64` (default `crc32c`) |
| `-M SIZE` | decompress | memory limit for decoding |
| `-a OFF` | extract | offset into the uncompressed stream |
| `-n LEN` | extract | bytes to read |
| `-f[BYTE]` | repair | fill unrecoverable gaps with BYTE (default 0) |
| `-q` | verify | report only through the exit status |

Sizes accept `K`, `M` and `G` suffixes. An input of `-`, or none, means stdin.

## Choosing a block size

Block size is the main tradeoff in the format:

| block size | seek latency | ratio |
|---|---|---|
| 64 KiB | lowest | lowest |
| 1 MiB | ~10x higher | better |
| 8 MiB | ~50x higher | best |

Pick by how you will read the file:

- **Random access, small reads** (databases, indexes, columnar data): 64 KiB,
  the default, ideally with a dictionary.
- **Mixed** (log archives you grep occasionally): 256 KiB to 1 MiB.
- **Whole-file reads only** (backups you restore end to end): 4 MiB or more.
  If you never seek, the format's advantage does not apply and a larger block
  is strictly better.

Measure rather than guess: `benchmarks/random_access.sh yourfile` sweeps block
and dictionary sizes on your own data and prints ratio against seek latency.

## The dictionary

A buffer sampled from across the whole input, stored once, and preloaded into
the codec's window before every block. It makes small blocks compress far
better than their size alone would allow, which is what makes low seek latency
affordable.

```sh
xq compress -D 8M -o out.xq file    # explicit
xq compress -D 0  -o out.xq file    # disabled
```

Measured on a source tree at 64 KiB blocks: **+28.2%** ratio with the default
`lzb` codec, enough that 64 KiB blocks beat blocks 128 times larger that have
no dictionary.

Three things to know:

1. **It is a single point of failure.** Every block references it, so damaging
   it makes the whole file unreadable. It carries its own checksum, so this is
   always detected and never silent, but if you value corruption independence
   above ratio then use `-D 0`.
2. **It needs a seekable input to be good.** Sampling across the file requires
   seeking. From a pipe only a prefix is available, which is measurably
   weaker.
3. **It is declined automatically for small inputs.** A dictionary larger than
   a quarter of the input would store the same bytes twice, so it is capped,
   and dropped entirely below 64 KiB.

It also requires a codec that supports one. Asking for a dictionary with
`stored` fails rather than silently producing a worse file.

## Threads

```sh
xq compress -T 0 ...   # auto, the default
xq compress -T 4 ...
xq compress -T 1 ...   # single-threaded
```

**Output is byte-identical at any thread count.** Compressing the same input
with the same options always produces the same bytes, so thread count is a
performance knob and never a correctness one.

Scaling on six performance cores, 256 MiB at level 9:

| threads | 1 | 2 | 4 | 6 |
|---|---|---|---|---|
| speedup | 1.00x | 1.97x | 3.86x | 4.60x |

Threading is POSIX-only for now.

## Damaged files

Which tool to reach for:

| symptom | use |
|---|---|
| want to know if a file is intact | `xq verify` |
| file opens but some blocks fail | `xq verify`, then `xq repair` |
| file will not open at all | `xq repair` |
| want structure without decoding | `xq info` |

A worked example. The index and footer are destroyed, so nothing can open it:

```
$ xq verify damaged.xq
xq: damaged.xq: corrupt footer

$ xq repair -o recovered.bin damaged.xq
blocks found    124
blocks ok       123
blocks damaged  1
recovered       8060928 bytes (7.7 MiB)
original size   8.0 MiB (96.09% recovered)
```

## Exit codes

| code | meaning |
|---|---|
| 0 | success |
| 1 | I/O error, or the file is not an `xq` file |
| 2 | bad usage or invalid options |
| 3 | the data is damaged |

Code 3 is distinct on purpose: scripts can tell "this file is corrupt" from
"something went wrong running the tool".

## C library

Link against `build/libxq.a` and include `include/xq.h`. The caller owns every
buffer; the library never retains, reallocates or frees caller memory.

### One-shot

```c
#include <xq.h>

xq_params p = xq_params_default();
p.codec = XQ_CODEC_ZSTD;

size_t cap = xq_compress_bound(src_len, &p);
void  *dst = malloc(cap);
size_t out = 0;

xq_status st = xq_compress(dst, cap, &out, src, src_len, &p);
if (st != XQ_OK) fprintf(stderr, "%s\n", xq_strerror(st));
```

`xq_compress_bound` is a hard bound, not an estimate: blocks that would expand
are stored raw.

### Random access

```c
xq_status st;
xq_reader *r = xq_reader_open("data.xq", NULL, &st);
if (!r) { /* st says why */ }

char buf[4096];
int64_t n = xq_reader_pread(r, buf, sizeof buf, 100000000);
/* n >= 0 is the byte count, short at end of stream, like pread(2).
   n < 0 is -(xq_status). */

xq_reader_close(r);
```

Safe to call `xq_reader_pread` concurrently on one handle from any number of
threads.

Memory is a stated function of configuration, never of file size:

```c
xq_reader_opts o = {0};
o.cache_blocks = 32;      /* decoded blocks kept resident */
o.threads      = 8;       /* concurrent decode slots      */
o.mem_limit    = 64 << 20;/* shrinks the above to fit     */
```

### Whole files

```c
xq_file_stats stats;
xq_compress_file("in.bin", "out.xq", &p, &stats);
xq_decompress_file("out.xq", "back.bin", 0, &stats);
```

### Verify and repair

```c
xq_verify_report rep;
xq_reader_verify(r, &rep);

xq_repair_opts o = { .fill_gaps = 1, .fill_byte = 0 };
xq_repair_report rr;
xq_repair_file("damaged.xq", "recovered.bin", &o, &rr);
```

### Errors

Every fallible call returns `xq_status`. There is no `errno`, no global and no
thread-local error state.

```c
if (xq_status_is_corruption(st)) {
    /* the data is damaged: worth trying xq_repair_file */
} else {
    /* a caller error, or this build cannot read that file */
}
```

## file(1) integration

```sh
cat contrib/xq.magic >> ~/.magic
file data.xq
# data.xq: xq compressed data, format 1.0, zstd, crc32c, 2^16 byte blocks, dictionary, 8388608 bytes uncompressed
```

## See also

- [README.md](README.md) what the format is and why
- [docs/format-spec.md](docs/format-spec.md) normative byte layout
- [docs/internals.md](docs/internals.md) why the code is shaped as it is
- [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md)

# xq

A seekable, block-parallel compression library and file format, written in C
with no dependencies.

**Status: early development.** Milestone 9 of 10. The container is complete and
works end to end, and the default build compresses with no dependencies.
Nothing here is stable, and no performance claim is made that is not backed by
a script in `benchmarks/`.

## What it is for

A compression format whose organizing principle is **random access on large
files**. Reading 4 KiB from the middle of a 100 GB archive should cost
microseconds, not a full decompression pass.

The design problem is a tradeoff:

```
seek latency  is proportional to block size    small blocks seek fast
ratio         is proportional to block size    large blocks compress well
```

Pick small blocks and every read is cheap but the file is large. Pick large
blocks and the file is small but every read decompresses far more than you
asked for.

`xq` breaks that tradeoff with a **shared file-level dictionary**: a buffer
sampled from across the whole input, stored once, and preloaded into the
codec's window before every block. Blocks stay independently decodable, so
parallelism and corruption isolation are preserved, while small blocks
compress far better than their size alone would allow.

### Measured

64 MiB source tree, 64 KiB blocks, own `lzb` codec, single threaded:

| config | ratio |
|---|---|
| 64 KiB blocks, no dictionary | 5.194x |
| **64 KiB blocks + 8 MiB dictionary** | **6.661x (+28.2%)** |
| 1 MiB blocks, no dictionary | 5.972x |
| 8 MiB blocks, no dictionary | 6.552x |

Small blocks with a dictionary beat blocks 128 times larger without one, which
is the entire thesis of the format.

The same measurement on a 256 MiB tree with the optional reference codec:

| config | ratio | seek p50 |
|---|---|---|
| 64 KiB blocks, no dictionary | 6.549x | 40 us |
| **64 KiB blocks + 8 MiB dictionary** | **7.678x** | **52 us** |
| 1 MiB blocks, no dictionary | 7.930x | 531 us |
| 8 MiB blocks, no dictionary | 8.275x | 2205 us |

The dictionary is worth **+17.2%** ratio at 64 KiB blocks. Read the first two
rows against the third: it matches a 1 MiB block's ratio to within 3.2% while
seeking **10.2x faster**. A [feasibility spike](docs/design/01-m0-spike-results.md)
predicted +17.4% and 9.2x before any of it was built.

Seek cost is independent of file size. Extracting 4 KiB from a 256 MiB file
takes the same wall time as from a 100 KB one; in-process a seek is tens of
microseconds, and the CLI number is dominated by process startup.

Container overhead is 0.067% at the default 64 KiB block size: a 24-byte block
header, a 4-byte checksum, and 16 bytes of index per block.

Compression is parallel, and the output is byte-identical whatever thread
count you use. On a six performance core machine, 256 MiB at level 9 with an
8 MiB dictionary:

| threads | 1 | 2 | 4 | 6 |
|---|---|---|---|---|
| speedup | 1.00x | 1.97x | 3.86x | 4.60x |

`xq_reader_pread` is safe to call concurrently on a single handle.

### Non-goals

- **Maximum compression ratio.** Optimising for ratio is explicitly not the
  point, and denser formats exist.
- **Maximum speed.** Faster codecs exist.
- **Archiving.** `xq` compresses a byte stream; it has no concept of filenames
  or permissions. Pair it with `tar`.
- **Encryption.** Wrong layer. The format reserves record types so it can be
  added compatibly later.

Full command reference: [USAGE.md](USAGE.md).

## Building

Requires a C11 compiler. No dependencies.

```sh
make            # static library, CLI and tests
make check      # tests, UBSan and fuzzing
```

## Using it

```sh
xq compress -o out.xq somefile          # lzb, 64 KiB blocks, CRC-32C
xq info out.xq                          # structure, without decoding
xq verify out.xq                        # check every block
xq repair -o recovered.bin damaged.xq   # salvage a damaged file
xq decompress -o back.out out.xq

# random access: read from the middle without decoding what precedes it
xq extract -a 100000000 -n 4096 -o piece.bin out.xq

# threads: 0 auto, 1 to disable
xq compress -T 4 -o out.xq somefile

# works on pipes; the result is still fully indexed and seekable
cat somefile | xq compress | xq decompress | cmp - somefile
```

An optional reference codec is available for comparison:

```sh
make WITH_ZSTD=1
build/with-zstd/xq compress -c zstd -D 8M -o out.xq somefile
```

### Library

```c
#include <xq.h>

xq_status st;
xq_reader *r = xq_reader_open("data.xq", NULL, &st);
char buf[4096];
int64_t n = xq_reader_pread(r, buf, sizeof buf, 100000000);
xq_reader_close(r);
```

Memory is a stated function of configuration, never of file size:
`block_size * (cache_blocks + 1)` plus staging. The index is memory-mapped and
searched in place.

## Current state

| # | milestone | state |
|---|---|---|
| 0 | Feasibility spike | done |
| 1 | Format layer, tests, fuzzing | done |
| 2 | Container end to end, CLI | done |
| 3 | Checksums, `verify` | done |
| 4 | Index, random access, block cache | done |
| 5 | Optional zstd codec | done |
| 6 | Shared dictionary | done |
| 7 | Threading, parallel encode and decode | done |
| 8 | `repair`, richer `info`, stream checksum verification | done |
| 9 | `LZB` codec with lazy matching | done |
| 10 | `LZE` entropy stage | next |

`lzb` is the default codec and needs no dependencies. It reaches 6.7x where
the optional reference codec reaches 9.5x on the same data, and decompresses
at a comparable speed. That ratio gap is what milestone 10 addresses: `lzb`
has no entropy coder, so every literal costs a full eight bits.

## Layout

```
include/xq.h        the entire public API
src/format/         pure parse/serialise: no I/O, no allocation, no globals
src/codec/          codec vtable and implementations
src/core/           encoder, decoder, dictionary, reader, public API
src/common/         checksums, byte access, overflow-checked arithmetic
src/platform/       file I/O and memory mapping
cli/                the command line tool
tests/              unit, malformed-input, round-trip and reader tests
benchmarks/         reproducible measurements
docs/               format specification and design records
```

`src/format/` being pure is a deliberate security property: the code that
parses untrusted bytes needs no filesystem and no setup, so it is exhaustively
fuzzable and small enough to read in one sitting.

The source files carry no comments. Rationale lives in
[docs/internals.md](docs/internals.md), which is worth reading before changing
anything in `src/`.

## Documentation

- [docs/format-spec.md](docs/format-spec.md) normative byte layout
- [docs/internals.md](docs/internals.md) why the code is shaped the way it is
- [docs/design/00-proposal.md](docs/design/00-proposal.md) goals and alternatives
- [docs/design/01-m0-spike-results.md](docs/design/01-m0-spike-results.md) the
  measurements that decided the architecture
- [USAGE.md](USAGE.md) every command and option, with the reasoning
- [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md)

## Licence

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

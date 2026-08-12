# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning will follow [Semantic Versioning](https://semver.org/) from 1.0.0;
until then the API and the on-disk format may change without notice.

## [Unreleased]

Pre-1.0 development. Milestones 0 through 9 of 10 are complete. The container
works end to end; the dependency-free codec does not exist yet, so real
compression currently requires `make WITH_ZSTD=1`.

### Added

- On-disk format v1.0 draft: 8-byte signature, TLV metadata records,
  independently decodable blocks, fixed-width index, 32-byte footer.
  Specified in `docs/format-spec.md`.
- Pure format layer (`src/format/`) with no I/O, allocation or global state,
  plus a structure-aware fuzz harness.
- CRC-32C with ARMv8 and SSE4.2 acceleration and a table fallback; the two are
  cross-checked against each other at every offset and length.
- XXH64, one-shot and streaming, for per-block and whole-stream checksums.
  Cross-checked exhaustively against the reference implementation.
- `STORED` codec, and an optional zstd codec behind `WITH_ZSTD=1`.
- Codec vtable with per-thread working contexts and digest-once shared
  dictionaries.
- Shared file-level dictionary: sampled construction on seekable input, prefix
  fallback on pipes, stored compressed with its own integrity check.
- Random-access reader: memory-mapped index binary-searched in place, LRU block
  cache, `pread`-shaped API, memory bounded by configuration rather than file
  size.
- CLI: `compress`, `decompress`, `info`, `verify`, `extract`.
- Benchmarks for random-access latency across block sizes and dictionary
  settings.
- Parallel compression with a pipelined slot ring, and a thread-safe reader.
  Compressed output is byte-identical at any thread count.
- `repair`: recovers data by scanning for block headers, so it works when the
  index and footer are gone entirely. Optional gap filling keeps surviving
  bytes at their original offsets.
- Whole-stream checksum verification on decompression, catching reassembly
  faults that per-block checksums cannot see.
- `file(1)` magic entry in `contrib/xq.magic`, and USAGE.md.
- `lzb`, a dependency-free LZ77 codec with lazy matching, varint offsets and a
  shared dictionary match index. Now the default codec, so the default build
  compresses without any third-party code.

### Measured

256 MiB source tree, zstd codec, versus the M0 feasibility spike's predictions:

| quantity | predicted | measured |
|---|---|---|
| dictionary ratio gain at 64 KiB blocks | +17.4% | +17.2% |
| seek advantage at matched ratio | 9.2x | 10.2x |

Container overhead is 0.067% at the default 64 KiB block size.

### Fixed

- `xq_compress_bound` did not account for the DICT record, so compression could
  overrun its own guaranteed bound on incompressible input with a dictionary
  enabled. Only reproducible at the worst case; now pinned by a regression
  test.
- Unsigned wraparound in the CRC-32C tail loops, found by UBSan.
- Objects built under different `WITH_ZSTD` settings could be linked together,
  because make tracks timestamps and not flags. Configurations now build into
  separate directories.

### Known limitations

- Threading is not yet supported on Windows: the positional-read fallback
  there is not thread-safe.
- The shared dictionary is a single point of failure for the file that uses it.
- No dependency-free compressing codec yet.

## Milestone history

| # | milestone | state |
|---|---|---|
| 0 | Feasibility spike | done |
| 1 | Format layer, tests, fuzzing | done |
| 2 | `STORED` codec end to end, CLI | done |
| 3 | Checksums, `verify` | done |
| 4 | Index, random access, block cache | done |
| 5 | Optional zstd codec | done |
| 6 | Shared dictionary | done |
| 7 | Threading, parallel encode and decode | done |
| 8 | `repair`, richer `info`, `file(1)` magic | done |
| 9 | `LZB` codec with lazy matching | done |
| 10 | `LZE` entropy stage; re-run the M0 spike against it | next |

# Design Proposal - a seekable, block-parallel compression format

**Status: accepted, with three sections superseded by measurement.**
Read [01-m0-spike-results.md](01-m0-spike-results.md) alongside this document.

| Section | What changed |
|---|---|
| §2.2 | The hypothesis as stated here - small blocks reaching *large-block* ratio - is **false**. The real, measured result is that at *equal* ratio the dictionary buys a 16× smaller block and ~9-12× lower seek latency. |
| §2.4 | Prefix dictionaries are much weaker than **sampled** ones (+11.3% vs +17.4%). Sampling is now the default whenever the input is seekable. |
| §6 | The planned fast-matcher-class v1 codec would show **near-zero** dictionary benefit. A lazy matcher and an entropy stage are on the critical path, not the roadmap; the milestone order changed accordingly. |

Decisions taken: shared-dictionary axis confirmed, own codec (option D),
Apache-2.0, spike-first, named `xq`. Default block size is **64 KiB**, not the
1 MiB assumed below.

**Name: `xq`** (chosen 2026-08-12), file extension `.xq`, symbol prefix `xq_`.

---

## 0. Executive summary

I propose a container format and C library whose organizing principle is
**random access on large files**, with a specific technical mechanism that
distinguishes it from prior art:

> **A file-level shared dictionary that decouples block size from compression
> ratio.**

Every other property you asked for (parallelism, streaming, corruption
isolation, predictable memory) falls out of a block-structured format almost
for free. The dictionary is the part that makes the format *earn its
existence*. §2 explains why.

---

## 1. The problem this format addresses

Block-structured compression with an index is well-trodden ground. Splitting a
stream into independently compressed blocks, recording their offsets, and
seeking to the right one is a known technique, and several formats already do
it. Taking that as a starting point rather than a contribution:

**Block structure alone is not enough.** It gives seekability, parallelism and
corruption isolation, all of which this format wants. What it does not solve is
the tradeoff those properties are bought with, and that is where the design
effort has to go.

The rest of this document takes block structure, an index, per-block
checksums and parallel-friendly layout as *requirements*, and puts the actual
design work into the ratio-versus-seek-latency tradeoff described next.

## 2. The core differentiator

### 2.1 The tension nobody has solved cleanly

There is a hard tradeoff at the center of every seekable format:

```
random-access latency   ∝  block size        (must decompress a whole block to read 1 byte)
compression ratio       ∝  block size        (bigger block = more back-references available)
```

Concretely, compressing a 4 GiB PostgreSQL dump with zstd -9:

- 1 MiB blocks → good seek latency, but each block starts with a cold window
  and cannot reference the other 4095 MiB of highly-repetitive schema/strings.
- 64 MiB blocks → good ratio, but reading one row costs a 64 MiB decompress.

Existing block formats hand you the dial and leave the choice to you: block
size is a single knob trading one property directly against the other.

### 2.2 The mechanism

Compress a prefix of the input - say the first 8 MiB - and store it **once**,
near the front of the file, as a *dictionary record*. Then compress every
block with that dictionary preloaded into the LZ window.

```
window during block compression/decompression:

   [ ---- shared dictionary (8 MiB, identical for every block) ---- ][ block (1 MiB) ]
   ^                                                                 ^
   matches may reach back here                                       block data starts here
```

Each block remains **fully independent**: to decode block 4711 you need the
dictionary and block 4711, and nothing else. So we keep:

- independent decode → random access
- independent decode → parallelism
- independent decode → corruption isolation

...while every block compresses roughly as well as if it were part of a much
larger window. Small blocks stop being expensive.

### 2.3 Why this is a real result, not a trick

This is the same mechanism zstd exposes as `ZSTD_c_enableLongDistanceMatching`
+ `ZSTD_CCtx_loadDictionary`, and it is well-proven - but **nobody has packaged
it as a file format with an index.** zstd dictionaries are aimed at *many tiny
files* (the classic "1000 × 1 KB JSON documents" case). Applying the same idea
*within one large file, to make small blocks cheap*, is the inversion, and it
is exactly the case seekable formats need.

Expected behaviour, to be confirmed by benchmark (§9), **not claimed yet**:

| | 1 MiB blocks, no dict | 1 MiB blocks, 8 MiB dict | 64 MiB blocks, no dict |
|---|---|---|---|
| ratio | baseline | close to the 64 MiB column | best |
| seek latency | good | good | bad |

If benchmarks show the middle column does *not* approach the right column on
realistic large files, **the premise of this project is wrong and we should
say so and stop.** That is the single most important experiment to run, and I
propose running a cheap version of it *before* writing the format (§11).

### 2.4 Honest costs of the dictionary

1. **Single point of failure.** Corrupt the dictionary and the whole file is
   unreadable - this directly undercuts the corruption-isolation goal.
   *Mitigations:* the dictionary carries its own checksum; it is small enough
   (≤ 64 MiB) that we can offer `--dict-copies=2` to store it twice; and
   `dict_size = 0` is always legal, giving a pure independent-block file for
   users who prioritize robustness over ratio.
2. **Decode-time memory floor.** Any read now costs `dict_size` of resident
   memory. This is *predictable and configurable*, which is what you asked
   for, but it is not zero.
3. **A prefix dictionary only helps homogeneous files.** For a tarball of
   mixed content, the first 8 MiB is a bad predictor of byte 3 GiB. This is
   real. Roadmap: content-sampled dictionaries (sample K windows spread across
   the input, requires a seekable input) and eventually a trained dictionary.
   v1 ships prefix-only and documents the limitation.

---

## 3. Goals and non-goals

### Goals
- **G1.** `pread`-shaped random access with latency bounded by one block, and
  an API that is as easy to use as `pread(2)`.
- **G2.** Decompression throughput near the best byte-oriented LZ codecs.
- **G3.** Linear multi-core scaling for both directions, bounded by I/O.
- **G4.** Memory usage that is a *stated function* of configuration, never of
  input size. `O(dict + threads × block)`, never `O(file)`.
- **G5.** Corruption in one block costs at most that block.
- **G6.** A file written to a pipe is byte-identical to one written to disk,
  and both are fully seekable. **No backpatching, ever.**
- **G7.** Trivial integration: one header, one static lib, zero dependencies,
  C11, builds with a plain Makefile.

### Non-goals
- **N1.** Maximum compression ratio. Denser formats exist and this will not be
  the densest; that is a deliberate trade, not an oversight.
- **N2.** Being an archiver. No filenames, no permissions, no multi-member
  archives. It compresses *a byte stream*. Pair it with tar.
- **N3.** Encryption. Wrong layer, and getting AEAD right is its own project.
  The format reserves record types so it can be added compatibly later.
- **N4.** Maximum compression speed. Faster codecs exist.
- **N5.** In-place / append-to-existing-file editing. v1 files are immutable.

---

## 4. File format

All integers **little-endian**, unaligned, loaded via explicit byte-wise
helpers (never a struct cast - that is both a UB and a portability bug).
Big-endian hosts byte-swap on load; the cost is irrelevant next to LZ decode.

### 4.1 Layout

```
+---------------------------+
| File Header      32 bytes |  fixed
+---------------------------+
| Meta records         0..n |  TLV: dictionary, user metadata, future extensions
+---------------------------+
| Block 0                   |  24-byte header + payload + checksum
| Block 1                   |
| ...                       |
+---------------------------+
| END_OF_BLOCKS record      |  explicit terminator
+---------------------------+
| INDEX record     optional |  absent only if explicitly disabled
+---------------------------+
| Footer           32 bytes |  fixed, last bytes of the file
+---------------------------+
```

**Every section is written strictly in order, front to back.** The encoder
never seeks backwards to fill in a length. This is the property that makes
G6 hold: a file produced on a pipe and redirected to disk is a normal,
seekable file. Zip cannot do this (local headers get backpatched); we can,
because the index lives *after* the data it describes.

### 4.2 File header (32 bytes)

| off | size | field | notes |
|---|---|---|---|
| 0 | 8 | `magic` | `0x89 'N' 'N' 'N' 0x0D 0x0A 0x1A 0x0A` |
| 8 | 1 | `format_major` | decoder **must** refuse if `> known` |
| 9 | 1 | `format_minor` | additive only; decoder proceeds |
| 10 | 2 | `header_size` | 32 for v1; decoder skips to this offset |
| 12 | 4 | `flags` | bit0 `INDEX_EXPECTED`, bit1 `DICT_PRESENT`, bit2 `UNIFORM_BLOCKS`, rest reserved-zero |
| 16 | 1 | `codec_id` | default block codec |
| 17 | 1 | `level` | informational only |
| 18 | 1 | `checksum_id` | 0 none, 1 crc32c, 2 xxh64 |
| 19 | 1 | `block_size_log` | nominal raw block size `1<<log`; 0 = variable |
| 20 | 8 | `raw_size` | `UINT64_MAX` = unknown (stream-written) |
| 28 | 4 | `header_crc32c` | over bytes `[0,28)` |

**The 8-byte magic is the PNG trick and it is worth the bytes.** `0x89`
detects 7-bit stripping; `\r\n` detects CRLF↔LF translation in *both*
directions (FTP ASCII mode, Windows text-mode redirection, some CI artifact
pipelines); `0x1A` is DOS EOF so `type file.xq` stops instead of spewing;
the trailing `\n` catches LF->CRLF. A short 2-byte signature detects none of
this.

**`header_crc32c` is not optional.** `block_size_log` drives buffer
allocation. Trusting an unverified length from a hostile file is the entire
CVE history of this software category. The decoder validates the header CRC
*and* clamps `block_size_log` against a caller-supplied policy limit before
allocating a single byte.

### 4.3 Meta records (TLV)

Fixed-width framing, no varints - varint framing means the parser must
bounds-check *while* parsing the bounds, which is where bugs live.

| off | size | field |
|---|---|---|
| 0 | 1 | `tag` |
| 1 | 1 | `rflags` - bit0 `CRITICAL` |
| 2 | 8 | `size` (payload bytes) |
| 10 | 4 | `crc32c` over `[0,10)` + payload |
| 14 | .. | payload |

Tags: `0x01 DICT`, `0x02 USER_META`, `0x10 END_OF_BLOCKS`, `0x11 INDEX`.
`0x80-0xFF` reserved for private extensions.

Forward compatibility rule: an unknown record with `CRITICAL` clear is
**skipped**; with `CRITICAL` set the decoder **fails cleanly**. This lets us
add e.g. encryption later without silently producing garbage on old readers.

`DICT` payload: `{kind u8, codec u8, dict_id u32, raw_size u64,
stored_size u64, xxh64 u64, bytes[]}`. `dict_id` is nonzero and each block
that uses a dictionary names it, so a future multi-dictionary format is a
minor-version bump rather than a break.

### 4.4 Block

| off | size | field |
|---|---|---|
| 0 | 2 | `block_magic` = `0x0C 0xB1` |
| 2 | 1 | `bflags` - bit0 `STORED` (payload is raw), bit1 `USES_DICT` |
| 3 | 1 | `codec_id` |
| 4 | 4 | `stored_size` - payload bytes |
| 8 | 4 | `raw_size` - bytes produced |
| 12 | 8 | `raw_offset` - offset of this block's first byte in the uncompressed stream |
| 20 | 4 | `header_crc32c` over `[0,20)` |
| 24 | `stored_size` | payload |
| - | 4 or 8 | payload checksum per `checksum_id` |

Three decisions worth defending:

**Why a separate header CRC?** So the framing can be trusted *before* the
payload length is used. The decoder verifies 20 bytes of CRC, and only then
does it believe `stored_size` enough to read that many bytes. Without this,
a flipped bit in a length field becomes a wild read.

**Why `raw_offset` in every block header (8 bytes/block)?** Because it makes
the index **fully reconstructible by linear scan**, and makes truncation
recovery *exact* - for any surviving block we know precisely which logical
byte range it covers, with no dependence on the footer, the index, or on any
other block being intact. At 1 MiB blocks this costs 0.0008% of the file. It
is the cheapest robustness in the whole design.

**Why a block magic?** Resynchronization. A recovery scan looks for the
2-byte magic, then validates the 20-byte header CRC. False-accept probability
is ~2⁻⁴⁸ per candidate offset, so `--repair` on a badly damaged file is
sound rather than heuristic.

**Why 32-bit sizes?** Caps a block at 4 GiB, which is far beyond any useful
setting for a format built for seeking, and saves 8 bytes. Policy limits will
cap it far lower anyway (default 64 MiB).

**`STORED` flag:** if compression expands a block, store it raw. This bounds
worst-case expansion to `24 + checksum` bytes per block - a hard guarantee we
can state in the docs, which matters for anyone sizing buffers.

### 4.5 Index

`INDEX` payload: `{block_count u64, total_raw u64, total_stored u64,
entries[block_count+1]}` where each entry is a fixed 16 bytes:
`{file_offset u64, raw_offset u64}`.

The trailing sentinel entry holds the end offsets, so block *i*'s extent is
`entry[i+1] - entry[i]` with no special case for the last block.

**Fixed-width, not varint-delta.** A varint index is ~3× smaller
but must be fully parsed into memory before you can search it. Fixed 16-byte
entries can be `mmap`ed and **binary-searched in place, with zero parsing and
zero allocation** - which is precisely what a random-access format needs. The
cost is 16 bytes/block: for a 100 GiB file at 1 MiB blocks, 1.6 MB of index.
That is a rounding error, and it buys O(log n) seeks against untouched pages.

`UNIFORM_BLOCKS` in the file header signals `raw_offset[i] == i << block_size_log`,
which lets a future minor version halve the index. Flagged in v1, exploited later.

### 4.6 Footer (32 bytes, always last)

| off | size | field |
|---|---|---|
| 0 | 4 | `crc32c` over `[4,32)` |
| 4 | 8 | `index_offset` (0 = no index) |
| 12 | 8 | `index_size` |
| 20 | 8 | `stream_checksum` (xxh64 of the whole uncompressed stream, 0 if disabled) |
| 28 | 4 | `footer_magic` |

Open is: `pread(fd, 32, size-32)` → magic → CRC → bounds-check
`index_offset + index_size ≤ size - 32` → `pread` the index → CRC. Two reads
to a fully usable random-access handle, regardless of file size.

### 4.7 Behaviour under stress

| Situation | Behaviour |
|---|---|
| **Truncated** | No footer → decoder reports `TRUNCATED` but can still stream every complete block; `--repair` recovers all of them, and `raw_offset` tells us exactly how much tail was lost. |
| **Block payload corrupted** | Checksum fails → that block errors; every other block still decodes. `read_at` outside it is unaffected. |
| **Block header corrupted** | Header CRC fails → sequential decode stops, but the index still locates every *other* block, so random access survives entirely. |
| **Index corrupted** | Index CRC fails → fall back to a linear header scan to rebuild it. Degraded to O(n) open, zero data loss. |
| **File header corrupted** | Header CRC fails → refuse. Unrecoverable by design; it is 32 bytes and we do not guess. |
| **Dictionary corrupted** | Whole file unreadable. The design's weakest point; see §2.4. |
| **Streamed** | Header has `raw_size = UNKNOWN`; index and footer still written after the data; result is fully seekable. |
| **Very large** | All offsets u64. Index is O(blocks) and `mmap`ed, never fully faulted in. |
| **Tiny blocks** | Header overhead is 28-32 bytes/block; at 64 KiB blocks that is 0.05%. Enforce a floor of 4 KiB. |

---

## 5. Public C API

Single header. Every call returns a status; no `errno`, no thread-local
global error, no hidden state. Deterministic output for identical
`(input, params)`.

```c
/* ---- status ------------------------------------------------------ */
typedef enum {
    XQ_OK = 0, XQ_ERR_BAD_MAGIC, XQ_ERR_UNSUPPORTED_VERSION,
    XQ_ERR_CORRUPT_HEADER, XQ_ERR_CORRUPT_BLOCK, XQ_ERR_CORRUPT_INDEX,
    XQ_ERR_TRUNCATED, XQ_ERR_DST_TOO_SMALL, XQ_ERR_MEMLIMIT,
    XQ_ERR_OOM, XQ_ERR_IO, XQ_ERR_PARAM, XQ_ERR_UNSUPPORTED_CODEC,
} xq_status;
const char *xq_strerror(xq_status);

/* ---- parameters -------------------------------------------------- */
typedef struct {
    int      level;         /* 1..12                                  */
    uint32_t block_size;    /* raw bytes per block; 0 = default 1 MiB */
    uint32_t dict_size;     /* 0 disables the shared dictionary       */
    int      codec;         /* XQ_CODEC_*                             */
    int      checksum;      /* XQ_CHECKSUM_*                          */
    int      threads;       /* 0 = auto, 1 = no threads spawned       */
    uint64_t mem_limit;     /* hard ceiling; XQ_ERR_MEMLIMIT if exceeded */
    const xq_allocator *alloc; /* NULL = malloc/free                  */
} xq_params;
xq_params xq_params_default(void);

/* ---- one-shot ---------------------------------------------------- */
size_t    xq_compress_bound(uint64_t src_size, const xq_params *p);
xq_status xq_compress(void *dst, size_t cap, size_t *out_len,
                      const void *src, size_t src_len, const xq_params *p);
xq_status xq_decompress(void *dst, size_t cap, size_t *out_len,
                        const void *src, size_t src_len, uint64_t mem_limit);

/* ---- random access ----------------------------------------------- */
typedef struct xq_reader xq_reader;
xq_reader *xq_reader_open(const char *path, const xq_reader_opts *, xq_status *);
xq_reader *xq_reader_open_fd(int fd, const xq_reader_opts *, xq_status *);
void       xq_reader_close(xq_reader *);

uint64_t   xq_reader_size(const xq_reader *);          /* uncompressed */
int64_t    xq_reader_pread(xq_reader *, void *buf, size_t n, uint64_t off);
           /* returns bytes read (short at EOF), or -(xq_status) on error */
xq_status  xq_reader_verify(xq_reader *, xq_verify_report *);
```

Design points:

- **`pread` signature, deliberately.** It is the shape every systems
  programmer already has in their fingers, it is stateless, and it is
  trivially thread-safe. `xq_reader_pread` on one handle from N threads is
  safe (internally: `pread(2)` + a mutex-guarded block cache), so callers get
  parallel random access without managing handles.
- **Negative return encodes the error.** `-(xq_status)` keeps the common path
  one branch, matching `read(2)` instincts. The alternative (out-param) makes
  every call site two lines.
- **`mem_limit` is a first-class parameter, not a global.** Decompression
  bombs are prevented by policy, not by hoping.
- **Ownership: the caller owns every buffer, always.** The library never
  retains, frees, or reallocs caller memory. Handles are freed only by their
  matching `_close`. One rule, no exceptions.
- **Streaming** uses a pump with explicit in/out cursors, so
  it never allocates on behalf of the caller and works with any I/O model
  (blocking, epoll, io_uring):

```c
typedef struct { const uint8_t *src; size_t size; size_t pos; } xq_inbuf;
typedef struct { uint8_t *dst;       size_t size; size_t pos; } xq_outbuf;
xq_status xq_cstream_compress(xq_cstream *, xq_outbuf *, xq_inbuf *);
xq_status xq_cstream_finish  (xq_cstream *, xq_outbuf *);  /* until pos==0 remaining */
```

Memory is then a documented formula:
`peak ≈ dict_size + threads × (block_size + bound(block_size)) × queue_depth`
for the encoder, and `dict_size + cache_blocks × block_size` for the reader.

---

## 6. Compression algorithm

### Options considered

| | Ratio | Decode speed | Work | Dependency |
|---|---|---|---|---|
| **A. Depend on zstd** | best | best | days | +1 dep, and the project becomes "a container around zstd" |
| **B. Write LZMA-class from scratch** | best | slow | months | none |
| **C. Own LZ77, byte tokens, no entropy coder** | fast-matcher-class | excellent | ~1 week | none |
| **D. C, then add entropy coding as codec 2** | → zstd-class | very good | staged | none |

### Recommendation: D, staged

`codec_id` is in both the file header and every block header, so codecs are
pluggable from day one:

- `0 STORED` - no compression. Needed for incompressible blocks (§4.4) and
  invaluable for testing the container independently of the codec.
- `1 LZB` - our LZ77: hash-chain matcher, byte-aligned literal/match tokens,
  **varint offsets**. Ships in v1.
- `2 LZE` - LZB + Huffman or rANS on literals and lengths. Later.
- `3 ZSTD` - optional, off by default, compiled in only if zstd is present.
  Gives us an honest ceiling to benchmark our own codec against, and gives
  users an escape hatch.

**Why varint offsets rather than a fixed 16-bit field?** Because an 8 MiB
dictionary needs 23-bit offsets, so 16 bits is simply not an option for the
mechanism in §2. Varints cost a byte-at-a-time decode loop, which is slower
than a fixed-width tight loop. Fixed 3-byte offsets are the alternative and
may win;
that is a measurable question and `codec_id` means we can answer it later
without a format break. v1 takes the simple correct path.

### The non-obvious part: sharing the dictionary's match index

Naively, each compression thread needs a hash chain over
`dict_size + block_size`, i.e. 4 bytes per position:

```
8 MiB dict + 1 MiB block = 9 MiB window  →  36 MiB of chain per thread
                                         →  432 MiB across 12 threads
```

That is unacceptable and would sink G4. But the dictionary is **immutable**,
so its hash table and chain array can be built **once at startup and shared
read-only across every thread**:

```
shared, built once:   dict hash table + chains        ~36 MiB
per thread:           chains over its own block only  ~4 MiB
                      ─────────────────────────────────────────
12 threads:           36 + 48 = 84 MiB   (vs 432 MiB naive)
```

The matcher walks the block's own chain first, then continues into the shared
dictionary chain. No locks - it is read-only after construction. This is the
single most important implementation decision in the encoder and it is what
makes the dictionary practical at high thread counts.

---

## 7. Architecture

```
include/xq.h                  the entire public API, one header

src/
  common/    err.c  bits.h (LE load/store)  checked.h (overflow-safe math)
             alloc.c  crc32c.c  xxh64.c
  platform/  file.c  thread.c  mmap.c        (POSIX + Win32 behind one API)
  format/    header.c  record.c  block.c  index.c  footer.c
  codec/     codec.h (vtable)  stored.c  lzb/{match.c,encode.c,decode.c}
  core/      encoder.c  decoder.c  dict.c  cache.c
             reader.c  cstream.c  dstream.c
             pool.c  mt_encode.c  mt_decode.c

cli/         main.c  cmd_{compress,decompress,verify,info,repair}.c
tests/       unit/  format/  roundtrip/  malformed/  fuzz/
benchmarks/  examples/  docs/
```

**The one architectural rule that matters:** everything under `src/format/` is
**pure** - it parses and serializes byte buffers, performs no I/O, and
allocates nothing. Every function is `(const uint8_t *buf, size_t len,
struct *out) -> xq_status`.

Consequences: the entire on-disk format is fuzzable with a 10-line harness
and no filesystem; malformed-input tests are pure unit tests; and the
attack surface that matters most is the part with the fewest moving parts.
This is the difference between a format parser you can trust and one you hope
about.

**Build:** hand-written portable Makefile (you have `make`, not cmake).
C11, `-std=c11 -Wall -Wextra -Wconversion -Wshadow`, plus ASan/UBSan and
`-fsanitize=fuzzer` targets. Zero dependencies for the default build.

---

## 8. Implementation milestones

Each milestone is independently testable and ends with something that runs.

| # | Deliverable | Proves |
|---|---|---|
| **0** | §11 feasibility spike | that the dictionary premise holds *at all* |
| 1 | `format/` + unit tests + malformed corpus | the container parses safely |
| 2 | `STORED` codec end-to-end, CLI compress/decompress | the whole pipeline works with a trivial codec |
| 3 | crc32c + xxh64, `verify` command | integrity |
| 4 | Index + `xq_reader_pread` + LRU cache | **random access - the headline feature** |
| 5 | `LZB` codec | actual compression |
| 6 | Shared dictionary + shared match index (§6) | **the differentiator** |
| 7 | Thread pool, parallel encode + decode | scaling |
| 8 | Streaming polish, `repair`, `info` | robustness |
| 9 | Fuzzing in CI, benchmark harness | the claims |

Note that random access (4) lands *before* real compression (5). The container
is the product; the codec is replaceable.

---

## 9. Benchmarking

Measure across our own configurations: block size, dictionary size and
construction, compression level, and thread count. Corpora must include
realistic large files, not just a standard test set: a multi-GB database dump,
a tarball of source, a log archive, a VM image, and an incompressible control.

Axes: ratio, compression MB/s, decompression MB/s, **random-access p50/p99
latency**, peak RSS, CPU-seconds, and thread scaling 1→12. Note the M3 Pro is
6P+6E - heterogeneous cores make naive scaling curves lie, so pin threads and
report both.

Rules I'll hold to:
- **No comparative claim without a reproducible script in `benchmarks/`.**
- Report the losses. This will not be the densest format (N1) nor the
  (N4). Saying so is what makes the wins credible.
- The one number that matters most is **random-access latency at a fixed
  ratio** - that is the axis this format is built to win, and if it does not
  win there it has no reason to exist.

---

## 10. Naming

Not deciding this for you. Constraints worth applying: 2-3 chars, free on
GitHub/crates.io/PyPI, and an extension that does not collide.

Already taken: the extensions already in common use by other compression tools.

Plausibly free and typeable: **`kz`**, **`qz`**, **`vz`**, **`nz`**, **`bx`**,
**`zk`**, **`ax`**, **`ez`**. Something evoking *seek* or *index* would fit
the purpose better than another z-word - e.g. **`ix`** / `.ix`, or **`sk`**.

**Resolved: the project is `xq`,** file extension `.xq`. The magic's three
name bytes are `X Q 1`; since the name is two characters, the third is a
format generation marker, distinct from `format_major`.

Confining the name to a symbol prefix and three magic bytes paid off exactly
as intended - the rename was one scripted pass over 27 files, with the only
manual edits being the magic bytes themselves and the prose here.

---

## 11. What I recommend doing first

Before writing any format code, I want to run a **half-day feasibility spike**
that answers the one question the project rests on:

> Does an 8 MiB shared dictionary let 1 MiB blocks approach the ratio of
> 64 MiB blocks, on realistic large files?

This is cheap to answer because zstd already has all the primitives - I can
measure it with `zstd -D` and a shell script on a few GB of real data, with
no library code at all. Possible outcomes:

- **Ratio gap closes substantially** → the premise holds, proceed to milestone 1
  with high confidence.
- **Gap is marginal** → the differentiator is weak. We should either pivot to
  a different one (block-level deduplication is the strongest alternative:
  content-defined chunking so identical regions of a large file are stored
  once, which is what makes backup formats valuable) or reconsider the project.

Spending half a day to avoid building a format around a false premise is the
highest-value work available right now.

---

## 12. Decisions I need from you

1. **Does the shared-dictionary differentiator (§2) match your intent?** It is
   the load-bearing idea. If you want a different axis - dedup, or purely
   simply block structure with a better API - the format changes.
2. **Run the §11 spike first, or go straight to milestone 1?** I recommend the
   spike.
3. **Own codec (recommendation D) or depend on zstd?** D means slower initial
   ratio but zero dependencies and a real systems project.
4. **License.** Apache-2.0 (patent grant, safest for adoption), BSD-2, or
   public-domain-ish (0BSD/CC0), which is common for compression cores.
5. **Name**, whenever convenient - it does not block anything.

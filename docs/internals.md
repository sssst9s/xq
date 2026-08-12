# Internals

Why the code is shaped the way it is. The source files themselves carry no
commentary, so this document is the record of the reasoning behind the
non-obvious decisions. If you are about to "simplify" something in `src/`,
check here first - several of these choices look arbitrary and are not.

Companion documents: [format-spec.md](format-spec.md) is normative for byte
layout; [design/00-proposal.md](design/00-proposal.md) covers goals and
alternatives; [design/01-m0-spike-results.md](design/01-m0-spike-results.md)
holds the measurements that decided the architecture.

---

## 1. Module rules

### `src/format/` is pure

No I/O, no allocation, no globals. Every function takes a byte buffer and a
length and returns a status.

This is a security property, not a style preference. The entire parser for
untrusted bytes is reachable from a 10-line fuzz harness with no filesystem
and no setup, so fuzzing spends its whole budget on the code that faces
hostile input. It also keeps that code small enough to read in one sitting.

### The parsing contract

**Validate before you believe.** A parse function must never return a length
or offset drawn from the input without having first verified the CRC that
covers it.

Callers may treat a successfully parsed struct's fields as internally
consistent. They must still bounds-check those fields against the actual file
size, which the format layer cannot see.

### Ownership

The caller owns every buffer. The library never retains, reallocates or frees
caller memory, and never returns a pointer into it. Handles are released only
by their matching `_close` / `_free`. One rule, no exceptions.

---

## 2. Reading and writing bytes

`xq_bits.h` loads and stores little-endian byte by byte rather than casting
pointers to integers or structs. That cast would be undefined behaviour twice
over - it violates alignment (a real fault on some ARM and MIPS
configurations) and strict aliasing, which modern compilers do exploit - and
it is wrong on big-endian hosts. The helpers compile to a single unaligned
load or a bswap on every compiler we care about, so the safety is free.

`xq_checked.h` exists because of one specific bug shape:

```c
if (offset + size <= file_size) { /* trusted */ }
```

which passes trivially when `offset + size` wraps. Every length and offset in
an `xq` file comes from a potentially hostile source, so the checked helpers
make the test explicit and hard to forget.

---

## 3. Checksums

### Why CRC-32C

Identical strength and cost to other 32-bit CRCs in the portable path, but
CRC-32C has a
single-instruction implementation on essentially every CPU this will run on:
ARMv8 CRC32 (mandatory on ARMv8.1, present on all Apple silicon) and x86
SSE4.2. That is roughly a 10x throughput difference on block payloads for zero
portability cost, because the table path is always compiled in as a fallback.

The table is a compile-time constant, so there is no lazy initialisation and
therefore no thread-safety question.

`xq_crc32c_portable()` is always compiled even when the hardware path is in
use, and the tests assert the two agree at every offset and length. A silent
divergence would produce files that verify only on the machine that wrote
them.

### Why XXH64 as well

CRC-32C is the right default for a 64 KiB block; at that size a 32-bit check
is ample. XXH64 exists for the whole-stream checksum and for very large files,
where 2^-32 collision odds per check start to matter - a 100 GiB file at
64 KiB blocks is 1.6 million checks.

XXH64 is defined in terms of wrapping arithmetic. The build enables
`-fsanitize=unsigned-integer-overflow` because wraparound in *length* code is
almost always a bug, so the XXH64 functions carry an explicit
`no_sanitize("unsigned-integer-overflow")` attribute rather than the check
being weakened globally.

The whole-stream checksum runs over **uncompressed** bytes. That catches a
codec which round-trips every block correctly yet assembles them wrongly - a
failure per-block checksums cannot see.

---

## 4. Format decisions

### Separate CRC on the block header

The header CRC covers bytes `[0,20)`; the payload has its own checksum. This
ordering lets a reader establish that the framing - above all `stored_size`,
which is about to become a read length - is intact *before* using it.
Combining the two would require reading an unverified length first, which is
the entire CVE history of this software category.

### `raw_offset` in every block header

8 bytes per block, 0.012% at the default 64 KiB block size. It buys an index
that is reconstructible by linear scan, and truncation recovery that is exact
- for any surviving block we know precisely which logical byte range it covers
with no dependence on the footer, the index, or any other block.

### Fixed-width index entries

Varint-delta entries would be roughly 3x smaller, but they must be parsed
serially into memory before any lookup. Fixed 16-byte entries can be
memory-mapped and binary-searched **in place**, with no parsing and no
allocation. That is what a random-access format actually needs; the size cost
is 1.6 MB of index on a 100 GiB file.

### Tag `0x0C` is permanently reserved

A sequential decoder distinguishes a block from a record by the first two
bytes: a block starts with `0C B1`. For that test to stay unambiguous, no
record tag may ever be `0x0C`, or a record with tag `0x0C` and rflags `0xB1`
would be indistinguishable from a block header. A static assertion in
`xq_format.h` makes a future violation a compile error rather than a
corruption bug that only appears on files containing such a record.

### Nothing is ever backpatched

Every section is written strictly front to back. This is why a file produced
on a pipe is byte-identical to one produced on disk apart from the header
fields that must differ, and fully seekable either way. It works because the
index lives after the data it describes, so nothing written earlier ever needs
amending.

### `INDEX_EXPECTED` is intent, not fact

The header is written before the encoder knows whether any blocks will follow,
and backpatching is forbidden. The footer's `index_offset` is authoritative.
The flag still helps a sequential reader, which never sees the footer: flag set
plus no INDEX record before EOF means the file was truncated.

---

## 5. Codec interface

Three kinds of state, deliberately separated:

1. **Nothing** - `bound()`, and the STORED codec's compress/decompress.
2. **Working context** - scratch reused between blocks. One per thread, never
   shared.
3. **Digested dictionary** - an immutable, preprocessed form of the shared
   dictionary. Built once, shared read-only by every thread.

The third is the important one. Attaching a raw dictionary per block makes the
codec re-index it every time: **179 s versus 13.1 s** for the same work, a
13.7x difference that compounds with thread count. The vtable therefore makes
"digest once, use many" the *only* way to pass a dictionary - there is no API
through which the slow version can be written by accident.

A codec with `cdict_new == NULL` has no dictionary support, and the encoder
refuses a dictionary for it rather than silently ignoring the request. A caller
who asked for a dictionary and did not get one would otherwise see a
mysterious ratio.

### zstd codec specifics

It uses `ZSTD_STATIC_LINKING_ONLY` for one reason: control over `windowLog` on
the digested dictionary. Without it the window can be smaller than
dictionary + block, silently truncating match distance and destroying the very
effect the dictionary exists to produce.

`refCDict` carries its own `cParams`, so the compression level must **not** be
applied on top - doing so resets the window and defeats the dictionary.

Our levels 1-12 map onto zstd's range with the low end mapped *up*. The M0
spike found the dictionary's benefit is gated on match-search depth: +0.1% at
zstd level 1, +14.7% at level 7 where it switches to `lazy2`. A user asking for
level 1 wants "fast", not "so fast the dictionary stops working".

---

## 6. The shared dictionary

The differentiator. See
[design/01-m0-spike-results.md](design/01-m0-spike-results.md) for the numbers.

**Construction depends on what the encoder can see.** Sampling 64 KiB windows
across the whole input requires a seekable input and a second read pass, and
measured +17.4% against +11.3% for a prefix - long-range redundancy is spread
through a file, not concentrated at its front. On a pipe only the prefix is
reachable. The sampling pass uses positional reads, so the sequential encode
pass still starts at offset zero.

**The size policy** caps the dictionary at one quarter of the input and drops
it below 64 KiB. An 8 MiB dictionary in front of a 1 MiB file stores the same
bytes twice while concentrating all risk in one region.

**It is the format's single point of failure.** Damage it and every block that
references it becomes undecodable, which cuts against the corruption-isolation
goal. It carries its own XXH64 so the damage is detected rather than silently
producing a whole file of plausible wrong output. `dict_size = 0` remains fully
supported for callers who would rather have independence than ratio.

**`xq_compress_bound` must include the DICT record.** It did not, once. On
compressible input the dictionary shrinks and slack elsewhere hides the
omission; it only surfaces on incompressible data, which is exactly the case a
bound exists to guarantee. `test_dict.c` pins this.

---

## 7. Reader

Opening is O(1) in file size: read the last 32 bytes, validate the footer, map
the index. A read is O(log n) to locate the block plus one block decode,
independent of where the offset falls and how large the file is.

Memory is a stated function of configuration, never of input size:

```
resident = block_size * (cache_blocks + 1) + one block of compressed staging
```

The index is mapped, not copied, so a 25 MB index on a 100 GiB file costs only
the pages a binary search actually touches.

A block is read with **one** positional read covering header, payload and
checksum together - the index already gave the exact extent. This measured no
latency change on a warm page cache; it is retained for cold and networked
storage, and because M7 multiplies syscall count by thread count.

The index record's CRC is verified at open, which touches every page of the
index. That is the one time the whole thing is paid for, and it is not
optional: a CRC-unverified index means arbitrary seeks into the file.

`xq_reader_verify` does not stop at the first bad block. With independent
blocks, "4095 of 4096 ok, first bad at raw offset 49938432, 268369920 of
268435456 bytes recoverable" is far more actionable than "corrupt".

**Not yet thread-safe.** Reads are positional and so do not disturb a shared
file offset, but the block cache is unsynchronised and there is a single
decode context. See §9.

---

## 8. Build

Hand-written Makefile, no cmake or meson: the library has zero dependencies,
and a generator would add a build-time dependency to a project whose selling
point is not having any.

`WITH_ZSTD=1` switches to a **separate build directory**. make tracks file
timestamps, not the flags a file was built with, so without that separation,
toggling the flag leaves incompatible objects in place - the link fails, or
worse, succeeds against a mixed build. `-MMD -MP` handles header dependencies.

ASan hangs before `main()` on macOS 26 with Apple clang 17, for every binary
including `int main(void){return 0;}`. Use `make ubsan` locally; ASan coverage
comes from CI on Linux. Apple's clang also ships no libFuzzer runtime, hence
the standalone driver in `tests/fuzz/fuzz_driver.c`, which is structure-aware
because uniformly random bytes never satisfy an 8-byte magic followed by a CRC
and would never reach the field validation underneath.

---

## 9. Threading

**Encoding is pipelined, not batched.** The first implementation ran rounds of
N blocks with a barrier between them, and it scaled badly: 2.4x at four
threads and *worse* beyond. On a machine with both performance and efficiency
cores, a barrier makes every round wait for whichever thread landed on the
slowest core. Replacing it with a slot ring, where each worker claims the next
sequence number as it becomes free, made scaling monotonic.

The slot ring holds `2 * threads` entries indexed by `seq % nslots`. The
producer fills a slot and publishes it; workers claim sequence numbers and
compress; the producer emits in sequence order and frees slots as it goes.
Backpressure is automatic: the producer cannot fill a slot until the sequence
`nslots` earlier has been emitted.

**Output is byte-identical at any thread count.** Only the producer thread
touches the output, the index and the stream checksum, and it does so strictly
in sequence order. Workers only ever compress a buffer and compute that
block's checksum. `tests/test_threads.c` asserts identical bytes across 1, 2,
3, 4, 7, 8 and 16 threads, and this property is not negotiable: without it the
format loses reproducibility.

**Per-block checksums are computed in the worker.** They derive purely from
the payload the worker just produced, so it is free parallelism and it keeps
that work off the serial producer.

**The dictionary must be force-attached.** With zstd, the default heuristic can
copy an 8 MiB dictionary into each context rather than referencing it, which
halved throughput and destroyed scaling. `ZSTD_c_forceAttachDict` with
`ZSTD_dictForceAttach` cut single-threaded time from 20.9 s to 10.1 s on a
256 MiB input.

### Reader

`xq_reader_pread` is safe to call concurrently on one handle.

- Positional reads never disturb a shared file offset.
- The block cache is guarded by a mutex, and the copy out of a cached block
  happens *while holding that mutex*. Returning a pointer into the cache would
  let another thread evict the entry from under the caller.
- Decode work uses a pool of independent slots, each with its own staging
  buffer and codec context, sized by `xq_reader_opts.threads`. A miss decodes
  into the slot's own buffer and then copies into the cache under the lock,
  which costs one extra block copy per miss and keeps the lock off the decode.

A regression worth remembering: an earlier edit left one `r->cbuf` in the
decode path, so every thread staged block payloads into the same buffer. It
passed single-threaded and produced wrong data plus spurious
`XQ_ERR_CORRUPT_BLOCK` from two threads up. Shared scratch is the failure mode
to look for here.

`xq_file_pread` is thread-safe on POSIX. The Windows fallback emulates it with
`lseek` + `read`, which is not; that path needs a descriptor per thread before
Windows is supported under threads.

---

## 10. Repair

`xq repair` exists because the index and footer are the two structures whose
loss stops every other tool, and they sit at the end of the file where
truncation hits first.

It scans for the two-byte block magic and confirms each candidate with the
block header CRC, so a false accept is about 2^-48 per offset. That is what
makes recovery sound rather than heuristic.

Every block header carries its own `raw_offset`, so a surviving block states
exactly which byte range of the uncompressed stream it holds. Recovery
therefore needs neither the index nor the footer, and recovered data can be
placed correctly rather than merely concatenated. This is what the 8 bytes per
block bought.

Scanning advances by the block extent when a block is confirmed, and otherwise
by one byte. Windows overlap by `XQ_BLOCK_HEADER_SIZE - 1` so a header
straddling a window boundary is not missed; a block whose payload runs past
the window leaves the cursor beyond it, and jumping straight there is correct.

Gap filling is not cosmetic. Skipping an unrecoverable range shifts every byte
after it, which silently corrupts anything offset-addressed. Filling keeps
survivors at their original offsets.

Blocks recovered out of order, or overlapping a range already written, are
counted and skipped rather than emitted: a repaired stream has to stay
monotonic.

---

## 11. The lzb codec

LZ77 with hash chains, lazy matching and varint offsets. No entropy coder,
which is milestone 10.

**Lazy matching is required, not an optimisation.** The M0 spike measured a
greedy matcher gaining roughly nothing from the shared dictionary: +0.1% at
the fast end against +14.7% once the matcher evaluates several candidates. A
greedy `lzb` would have made the format's central feature invisible. With lazy
matching it gains **+28.2%** from an 8 MiB dictionary at 64 KiB blocks, which
is more than the reference codec gains, because a weaker baseline leaves more
for the dictionary to recover.

**Varint offsets, not a fixed 16-bit field.** An 8 MiB dictionary needs 23-bit
distances, so a fixed short offset is simply not expressible. Varints cost a
byte-at-a-time decode loop; a fixed 3-byte offset is the obvious alternative
and may win. That is a measurable question and `codec_id` means it can be
answered later without a format break.

**Token layout.** One byte holds a 4-bit literal-length code and a 4-bit
match code, each extending into a varint at 15. Match code 0 is reserved to
mean "literals only, end of block", which is why match lengths are stored
biased by one: without a spare code the terminator would be ambiguous with a
minimum-length match.

**The dictionary digest is a hash chain built once.** `cdict_new` indexes the
whole dictionary and the result is immutable, so every worker thread shares
it. The searcher walks the block's own chain first, then continues into the
dictionary's. Rebuilding that index per block is what made the reference
implementation 13.7x slower in the spike.

**Decoding is the attack surface.** A match distance may reach back past the
start of the block into the dictionary, so the decoder splits such a copy into
a dictionary part and a block part, and bounds-checks the distance against
`op + dict_len` before either. Overlapping copies are byte-wise on purpose:
`dist < mlen` is the run-length case LZ77 depends on. `tests/fuzz/fuzz_lzb.c`
fuzzes the decoder directly, with and without a dictionary.

**Where it stands.** 6.7x against the reference codec's 9.5x on the same data,
decompressing at comparable speed but compressing about three times slower.
The ratio gap is almost entirely the missing entropy stage: every literal
currently costs eight bits and every offset a whole number of bytes.

---

## 12. The lze entropy stage

`lze` runs the `lzb` parser and then Huffman-codes its output. Canonical codes,
lengths capped at 15 bits, tables stored as 256 four-bit lengths in 128 bytes.

**One order-0 model over the whole LZB stream, not three.** The stream mixes
literals, which are highly skewed, with offsets, which are close to uniform, so
a single model is diluted. Splitting into separate literal, token and offset
streams would code each with its own distribution and is the obvious next
improvement. It needs no format change, because `codec_id` distinguishes them.
The simple version was built first because it is obviously correct, and it
already recovers +11.4%.

**A raw fallback is required, not optional.** On dense or already-compressed
data the table plus a near-flat distribution costs more than it saves, so the
encoder compares and emits mode 0 with the plain LZB stream when Huffman does
not win. Without that, `lze` would be worse than `lzb` on incompressible input.

**Table validation is strict.** `xq_huff_load` rejects a length set that is not
an exactly-filled prefix code, and requires the single-symbol case to use a
one-bit code. Over-subscribed lengths would let crafted input produce
overlapping codes; incomplete ones are merely wasteful, but no conforming
encoder emits them, and this codebase rejects what a conforming encoder would
not produce.

**Length limiting is by frequency smoothing.** When Huffman produces a code
longer than 15 bits the frequencies are halved and the tree rebuilt, which
converges because halving flattens the distribution. Package-merge would be
optimal; smoothing costs a little ratio only on pathologically skewed blocks
and is a fraction of the code.

Codes are emitted least-significant-bit first, so the canonical
most-significant-bit-first code is reversed before being written. The decode
table is flat: `1 << max_len` entries, every slot whose low bits match a code
filled with that symbol, so decoding is one indexed lookup rather than a walk.

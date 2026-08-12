# File format specification - version 1.0

**Status: draft.** The format is not frozen and no compatibility promise is
made until version 1.0 is tagged.

This document is normative for byte layout. `src/format/xq_format.h` is the
normative source for constants; the two must agree, and `tests/test_format.c`
checks the parts that can be checked mechanically.

Throughout, "MUST"/"SHOULD"/"MAY" carry their RFC 2119 meanings. The project
is named `xq`; the customary file extension is `.xq`.

---

## 1. Conventions

- All integers are **unsigned little-endian**, stored without alignment
  requirements. Implementations MUST load and store them byte-wise rather than
  by casting pointers to integer or struct types: such casts are undefined
  behaviour under strict aliasing, fault on some architectures, and are wrong
  on big-endian hosts.
- All offsets and sizes are 64-bit unless stated otherwise.
- "CRC" means CRC-32C (Castagnoli, reflected polynomial `0x82F63B78`, initial
  and final XOR `0xFFFFFFFF`). Check value for `"123456789"` is `0xE3069283`.
- Reserved fields MUST be written as zero. Readers MUST reject non-zero
  reserved bits in the file header (see §3) and MUST ignore them elsewhere.

## 2. Layout

A file consists of these sections, in this order:

```
File Header          32 bytes, mandatory
Meta records         0 or more, TLV
Block 0 .. Block N-1 N >= 0
END_OF_BLOCKS record mandatory in a complete file
INDEX record         optional
Footer               32 bytes, mandatory in a complete file
```

Every section is written strictly front to back. A conforming encoder MUST
NOT seek backwards to patch a previously written field. This is what makes a
file produced on a pipe byte-identical to one produced on a seekable output,
and fully seekable once stored.

## 3. File header (32 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `magic` | `89 58 51 31 0D 0A 1A 0A` (`\x89 X Q 1 \r \n \x1a \n`) |
| 8 | 1 | `format_major` | 1 |
| 9 | 1 | `format_minor` | 0 |
| 10 | 2 | `header_size` | 32 in this version |
| 12 | 4 | `flags` | see below |
| 16 | 1 | `codec_id` | default codec for blocks |
| 17 | 1 | `level` | informational only |
| 18 | 1 | `checksum_id` | 0 none, 1 CRC-32C, 2 XXH64 |
| 19 | 1 | `block_size_log` | nominal block size is `1 << n`; 0 means variable |
| 20 | 8 | `raw_size` | uncompressed total, `UINT64_MAX` if unknown |
| 28 | 4 | `crc32c` | over bytes `[0, 28)` |

Each byte of the signature earns its place: `0x89`
detects 7-bit stripping, `0D 0A` detects CRLF↔LF translation in both
directions, `1A` stops `type` on DOS-derived shells, and the trailing `0A`
detects LF→CRLF.

> **`INDEX_EXPECTED` is a statement of intent, not of fact.** The header is
> written before the encoder knows whether any blocks will follow, and the
> format forbids seeking back to amend it. The footer's `index_offset` is
> authoritative. The flag is still useful to a *sequential* reader, which
> never sees the footer: if the flag is set and no INDEX record arrives before
> end of file, the file was truncated. An encoder that emits zero blocks
> writes `index_offset = 0` and MAY leave the flag set.

**Flags**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `INDEX_EXPECTED` | the encoder intended to write an index - see note |
| 1 | `DICT_PRESENT` | a DICT record precedes the first block |
| 2 | `UNIFORM_BLOCKS` | `raw_offset[i] == i << block_size_log` for all i |
| 3 | `STREAM_WRITTEN` | produced without a seekable input |
| 4-31 | reserved | MUST be zero |

**Reader requirements**

1. Verify the magic. Mismatch → not a xq file.
2. Verify `crc32c` **before** interpreting any other field. Nothing in the
   header may be trusted until this passes; `block_size_log` in particular
   drives buffer sizing.
3. Reject `format_major` other than 1. A future major version MAY reorganise
   anything.
4. Accept any `format_minor`. Minor versions only add skippable records.
5. Reject `header_size < 32`. Skip forward to `header_size` before reading
   the first record, so a future minor version may extend the header.
6. Reject unknown `flags` bits: every defined flag is structural, so
   proceeding risks silently misreading the file.
7. Reject `UNIFORM_BLOCKS` combined with `block_size_log == 0`.
8. If `block_size_log != 0` it MUST be in `[12, 28]`. Readers MUST additionally
   apply their own memory policy before allocating.

## 4. Meta records

All non-block sections after the file header share one framing.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `tag` |
| 1 | 1 | `rflags` |
| 2 | 8 | `size` - payload bytes following this header |
| 10 | 4 | `crc32c` - over bytes `[0, 10)` followed by the payload |
| 14 | `size` | payload |

The framing is fixed-width by design. Varint framing would require a parser to
bounds-check while still parsing the bounds, which is where this class of code
grows vulnerabilities.

**Tags**

| Value | Name | Critical |
|---|---|---|
| 0x01 | `DICT` | yes |
| 0x02 | `USER_META` | no |
| 0x10 | `END_OF_BLOCKS` | yes, `size` = 0 |
| 0x11 | `INDEX` | no |
| 0x80-0xFF | private use | writer's choice |

**Record flags:** bit 0 is `CRITICAL`. A reader encountering an unknown tag
MUST fail if `CRITICAL` is set and MUST skip the record otherwise. This is the
forward-compatibility mechanism: it allows features such as encryption to be
added without old readers silently producing wrong output.

Because `size` is covered by a CRC that also covers the payload, a reader
cannot verify it before reading the payload. Readers MUST therefore
bounds-check `size` against the actual file size before allocating or reading,
and MUST reject implausible values (this implementation caps records at 2^40
bytes).

### 4.1 DICT payload

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `kind` - 0 none, 1 prefix, 2 sampled, 3 trained |
| 1 | 1 | `codec_id` - compression applied to the dictionary itself |
| 2 | 4 | `dict_id` - non-zero |
| 6 | 8 | `raw_size` |
| 14 | 8 | `stored_size` |
| 22 | 8 | `xxh64` - of the uncompressed dictionary |
| 30 | `stored_size` | dictionary bytes |

`kind` is informational: a decoder needs only the resulting bytes. It is
recorded because the encoder's choice is constrained by whether its input was
seekable - sampling requires seeking, so a pipe can only produce a prefix -
and that is worth reporting in `info` output.

Encoders SHOULD decline a dictionary that would be disproportionate to the
input. This implementation caps it at one quarter of the uncompressed size and
drops it entirely below 64 KiB, since a dictionary comparable in size to the
data stores the same bytes twice while still concentrating all risk in one
region.

The dictionary is preloaded into the codec's window before each block that
sets `USES_DICT`. Blocks remain independently decodable given the dictionary.

> A corrupted dictionary renders every block that references it undecodable.
> This is the format's principal single point of failure. Encoders SHOULD
> offer redundant storage of the dictionary, and `dict_size = 0` MUST remain
> supported for callers who prioritise corruption independence over ratio.

## 5. Block

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `magic` = `0C B1` |
| 2 | 1 | `bflags` - bit 0 `STORED`, bit 1 `USES_DICT` |
| 3 | 1 | `codec_id` |
| 4 | 4 | `stored_size` - payload bytes, excluding the checksum |
| 8 | 4 | `raw_size` |
| 12 | 8 | `raw_offset` - offset of this block's first byte in the uncompressed stream |
| 20 | 4 | `crc32c` - over bytes `[0, 20)` |
| 24 | `stored_size` | payload |
| - | 0/4/8 | payload checksum, per `checksum_id` |

**The header CRC is separate from the payload checksum on purpose.** It lets a
reader establish that the framing - above all `stored_size`, which is about to
become a read length - is intact before using it. Combining the two would
require reading an unverified length first.

`raw_offset` is stored in every block, costing 8 bytes per block (0.0008% at
1 MiB blocks, 0.012% at 64 KiB blocks). It makes the index reconstructible by
linear scan and makes truncation recovery exact, with no dependence on the
footer, the index, or any other block.

Requirements:

- `raw_size` MUST be greater than zero and MUST NOT exceed 256 MiB.
- If `STORED` is set, `stored_size` MUST equal `raw_size`. Encoders MUST set
  `STORED` when compression would expand a block, which bounds worst-case
  expansion to 24 bytes plus the checksum per block.
- `raw_offset + raw_size` MUST NOT overflow 64 bits.
- Blocks MUST appear in increasing `raw_offset` order and MUST be contiguous:
  block *i+1* begins at `raw_offset[i] + raw_size[i]`.

The 2-byte magic exists for resynchronisation during recovery. A scanner
locates candidate blocks by magic and confirms them with the header CRC; the
false-accept probability is about 2⁻⁴⁸ per candidate offset.

## 6. INDEX record

Payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `block_count` |
| 8 | 8 | `total_raw` |
| 16 | 8 | `total_stored` |
| 24 | 16 × (`block_count` + 1) | entries |

Each entry is `{ file_offset u64, raw_offset u64 }`. There are
`block_count + 1` entries: the final one is a sentinel holding the end
offsets, so block *i* occupies `entry[i] .. entry[i+1]` in both domains with
no special case for the last block.

**Entries are fixed-width rather than varint deltas.**
Varints would be roughly 3× smaller but must be parsed serially into memory
before any lookup. Fixed 16-byte entries can be memory-mapped and binary
searched in place, with no parsing and no allocation, which is what a
random-access format actually needs. The cost is 16 bytes per block - 1.6 MB
of index for a 100 GiB file at 1 MiB blocks.

Readers MUST validate that both offset sequences are strictly increasing and
that the sentinel agrees with `total_raw`. A CRC-valid but internally
inconsistent index would otherwise produce arbitrary seeks.

## 7. Footer (32 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `crc32c` - over bytes `[4, 32)` |
| 4 | 8 | `index_offset` - absolute; 0 means no index |
| 12 | 8 | `index_size` - total record size including its 14-byte header |
| 20 | 8 | `stream_checksum` - XXH64 of the whole uncompressed stream, 0 if disabled |
| 28 | 4 | `magic` = `58 51 46 54` (`"XQFT"`) |

The footer magic differs from the file magic so that a truncated file
concatenated with another cannot be mistaken for a complete one.

Opening for random access is: read the last 32 bytes, check magic, check CRC,
bounds-check `index_offset + index_size <= file_size - 32`, read the index,
check its record CRC. Two reads, independent of file size.

## 8. Behaviour under damage

| Condition | Required behaviour |
|---|---|
| No footer, or footer CRC fails | Report truncation. Readers SHOULD still decode all complete blocks by scanning. |
| Index CRC fails or validation fails | Fall back to a linear scan of block headers. O(n) open, no data loss. |
| Block header CRC fails | That block is unusable. Sequential decode stops; random access to other blocks MUST still work via the index. |
| Block payload checksum fails | That block is unusable; all other blocks remain usable. |
| File header CRC fails | Reject the file. It is 32 bytes and MUST NOT be guessed at. |
| DICT payload checksum fails | Every block with `USES_DICT` is unusable. |
| Truncated mid-block | All preceding complete blocks remain decodable; `raw_offset` identifies exactly what was lost. |

## 9. Streaming

When the input is not seekable, an encoder:

- writes `raw_size = UINT64_MAX` and sets `STREAM_WRITTEN`;
- MAY build a dictionary only from a prefix of the input (`kind = 1`), since
  sampling requires seeking;
- still writes the INDEX and footer, because they follow the data.

The result is a fully seekable file. A decoder reading from a pipe processes
the header, records and blocks in order and simply never sees the index.

## 10. Open questions before 1.0 is frozen

1. Whether `UNIFORM_BLOCKS` should halve the index by dropping `raw_offset`
   from entries. Straightforward but not yet measured against real seek
   patterns.
2. Whether to reserve a record tag for an inline recovery copy of the
   dictionary, versus leaving redundancy to an encoder that writes the DICT
   record twice.
3. Whether `stream_checksum` should be mandatory. It costs a serial pass over
   the uncompressed data, which partially defeats parallel decode.
4. Whether to define a canonical extension for a detached index sidecar, for
   files that were written without one.

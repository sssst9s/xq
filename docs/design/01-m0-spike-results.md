# Milestone 0 - feasibility spike results

**Verdict: the premise holds, but not in the form originally stated, and one
finding forces a change to the implementation plan.**

Hardware: Apple M3 Pro (6P+6E), macOS 15.6. Codec used as a stand-in: zstd
1.5.7 via libzstd, since the question is about *format structure*, not about
our eventual codec. Harness: `benchmarks/spike/` (mirrors of the scratch
tooling used here).

Corpora, all built from real system data except the JSONL pair:

| corpus | size | content |
|---|---|---|
| `srctext` | 345 MB | tar of SDK headers, Python, text config - long-range redundancy |
| `mixed` | 1 GiB | tar of `/opt/homebrew` - realistic mixed tree |
| `binaries` | 283 MB | tar of Mach-O executables and dylibs |
| `jsonl_static` | 1 GiB | synthetic HTTP access log, stationary content |
| `jsonl_drift` | 1 GiB | same, but path/UA vocabularies rotate fully across the file |
| `random` | 256 MB | `/dev/urandom` control |

`jsonl_drift` exists specifically to attack the prefix-dictionary weakness
noted in the proposal (§2.4): content late in the file is deliberately not
represented by content at the front.

---

## Finding 1 - the original hypothesis, as stated, is FALSE

> *"Does an 8 MiB shared dictionary let 1 MiB blocks approach the ratio of
> 64 MiB blocks?"*

**No.** At 1 MiB blocks with an 8 MiB prefix dictionary, level 12:

| corpus | 1 MiB no dict | 1 MiB + 8 MiB prefix dict | 64 MiB no dict | gap closed |
|---|---|---|---|---|
| srctext | 7.944x | 8.334x | 10.368x | 16.1% |
| mixed | 4.863x | 4.866x | 5.809x | 0.4% |
| jsonl_drift | 4.831x | 4.727x | 4.753x | 0% (dict *hurt*) |

Large blocks retain a real ratio advantage that a shared dictionary does not
recover. **Any claim of the form "small blocks compress like large blocks" is
not supported and must not be made.**

A second surprise: on `jsonl_drift`, 1 MiB blocks (4.831x) beat both 64 MiB
blocks (4.753x) and a single unbounded stream (4.738x). That data is
memoryless past ~1 MiB, so extra context buys nothing while costing longer
match offsets. Not all data has long-range redundancy to exploit.

## Finding 2 - the real result: the Pareto frontier moves, substantially

The right question was not "can small blocks match large-block ratio" but
**"at a fixed ratio, how small can blocks get?"** Reframed that way the result
is strong. Level 12, sampled dictionary:

| corpus | 64 KiB blk + dict | 1 MiB blk, no dict | ratio delta | seek p50 (dict) | seek p50 (1 MiB) | **latency win** |
|---|---|---|---|---|---|---|
| srctext | 7.803x (8 MiB) | 7.944x | −1.8% | 48 µs | 443 µs | **9.2x** |
| mixed | 5.447x (32 MiB) | 5.466x | −0.3% | 41 µs | 477 µs | **11.6x** |
| binaries | 3.505x (32 MiB) | 3.508x | −0.1% | 63 µs | 743 µs | **11.8x** |
| jsonl_drift | 4.680x (8 MiB) | 4.838x | −3.3% | 95 µs | 490 µs | 5.2x |

**At equal compression ratio, the shared dictionary buys a 16x smaller block
and roughly 9-12x lower random-access latency.** That is the defensible claim,
it is consistent across four independent corpora, and it is exactly the axis
the format exists to win. The drifting-vocabulary corpus is the weakest case
and still wins 5.2x.

## Finding 3 - sampled dictionaries beat prefix dictionaries decisively

`srctext`, 64 KiB blocks, level 12, 8 MiB dictionary:

| construction | ratio | vs. no dict |
|---|---|---|
| none | 6.649x | - |
| prefix (first 8 MiB) | 7.398x | +11.3% |
| **sampled (64 KiB windows spread across file)** | **7.803x** | **+17.4%** |
| trained (`ZDICT_trainFromBuffer`) | 7.117x | +7.0% |

Sampling wins because long-range redundancy is spread throughout the file, not
concentrated at the front. Training underperforms because zstd's trainer is
built for many small independent samples, not for one large file.

**Format consequence:** none - the format stores dictionary *bytes* either
way. **Encoder consequence:** sampling requires a seekable input, so:

- seekable input (a file) → **sampled** dictionary, the good path
- pipe/stdin → **prefix** dictionary from the first N MiB, or none

The file header records which strategy was used, for `info` and diagnostics.

## Finding 4 - the dictionary must be digested once and shared (confirms §6)

Attaching the dictionary per block (`ZSTD_CCtx_refPrefix`) versus digesting it
once into a shared immutable object (`ZSTD_CDict` + `refCDict`), 64 KiB blocks:

| | compression time |
|---|---|
| per-block re-index | 179.3 s |
| **digested once, shared** | **13.1 s** |

**13.7x.** The proposal's shared read-only match index (§6) is not an
optimization, it is a precondition. Without it the encoder is unusable, and
the effect compounds with thread count.

## Finding 5 - THE PROBLEM: the payoff is gated by match-search depth

`srctext`, 64 KiB blocks, 8 MiB sampled dictionary, by compression level:

| level | no dict | + dict | gain | zstd strategy |
|---|---|---|---|---|
| 1 | 5.717x | 5.722x | **+0.1%** | `fast` |
| 2 | 5.738x | 5.847x | +1.9% | `fast` |
| 3 | 5.989x | 6.176x | +3.1% | `dfast` |
| 4 | 5.994x | 6.247x | +4.2% | `dfast` |
| 5 | 6.228x | 6.721x | +7.9% | `greedy` |
| 6 | 6.410x | 7.014x | +9.4% | `lazy` |
| 7 | 6.478x | 7.433x | **+14.7%** | `lazy2` |
| 9 | 6.551x | 7.636x | +16.6% | `btlazy2` |
| 12 | 6.649x | 7.803x | +17.4% | `btlazy2` |
| 15 | 6.928x | 7.988x | +15.3% | `btopt` |

On `mixed` at level 1 the dictionary is actively **harmful** (4.340x → 4.226x,
−2.6%): marginal far matches cost more to encode than they save.

**Entropy coding is present at every level in this table, so the variable that
moved is match-search strategy.** The payoff appears when the matcher starts
evaluating multiple candidates (`greedy` → `lazy2`), and roughly plateaus
there.

### Why this breaks the original plan

The plan was to ship v1 with `LZB`: LZ77, hash chain, byte-aligned tokens, no
entropy coder - deliberately a fast greedy matcher with no entropy stage. **That codec
would show approximately zero dictionary benefit.** We would have built the
entire differentiator and then been unable to demonstrate it, and the natural
but wrong conclusion would have been "the dictionary idea doesn't work."

This is exactly what the spike was for.

---

## Revised plan

Three changes:

**1. The v1 codec needs a lazy matcher, not a greedy one.** `LZB` becomes
LZ77 with hash chains, configurable search depth, and lazy match evaluation
(depth ≥ 2 candidates). This is maybe 2-3x the work of the fast-matcher-class design
and is not optional - it is the minimum at which the format's differentiator
is visible.

**2. Entropy coding moves from "later" to "required before claiming the
differentiator."** This table cannot separate entropy coding's contribution,
because every level has it. What it does show is that far-offset matches must
be *cheap to encode* or they do not pay for themselves - the level-1 `mixed`
regression is that effect. Our varint offsets are the crudest possible
encoding, so `LZE` (Huffman/FSE on literals, lengths, offset codes) is on the
critical path, not the roadmap.

**3. Validate the container end-to-end with zstd first.** Add `codec_id 3 =
ZSTD` as an *optional, off-by-default, build-time* codec immediately rather
than last. This lets M4-M7 (index, random access, dictionary, threading) be
built and proven against a known-good strong codec, so container bugs and
codec immaturity never get confused for one another. The shipping default
stays dependency-free (`STORED` / `LZB` / `LZE`); zstd is a benchmark ceiling
and an escape hatch, exactly as §6 proposed - just wired up earlier.

### Revised milestone order

| # | was | now |
|---|---|---|
| 1 | format layer | unchanged |
| 2 | STORED end-to-end | unchanged |
| 3 | checksums + verify | unchanged |
| 4 | index + random access | unchanged |
| 5 | LZB (fast-matcher-class) | **optional zstd codec** - proves the container early |
| 6 | dictionary | **dictionary + shared match index** - differentiator provable now |
| 7 | threading | unchanged |
| 8 | streaming, repair, info | unchanged |
| 9 | fuzz + benchmarks | **LZB with lazy matching** |
| 10 | - | **LZE entropy stage**, then re-run this spike against our own codec |

### Parameters to carry into the implementation

- default block size **64 KiB** (not 1 MiB) - the spike shows this is where the
  design wins, and the dictionary makes its ratio cost acceptable
- default dictionary **8 MiB**, configurable; 32 MiB helped `mixed` (+6.0%) and
  `binaries` (+3.7%) but *hurt* `jsonl_drift` (−1.0%), so it must not be
  hardcoded
- dictionary construction: **sampled** when input is seekable, prefix otherwise
- `dict_size = 0` must stay a fully supported configuration

## Confirmed in the implementation (M6, 2026-08-12)

The spike's predictions were checked against the real encoder and reader on
the same 256 MiB `srctext` corpus, `-c zstd`, 64 KiB blocks, 8 MiB sampled
dictionary:

| quantity | spike predicted | implementation measured |
|---|---|---|
| dictionary ratio gain | +17.4% | **+17.2%** |
| seek advantage at matched ratio | 9.2x | **10.2x** |

Finding 4 also held: digesting the dictionary once and sharing it is what
makes the encoder usable, and the vtable now makes "digest once" the only way
to pass one. Finding 3 held too - the encoder samples when the input is
seekable and falls back to a prefix on a pipe.

Finding 5 remains the open risk. Everything above uses zstd, which has an
entropy coder at every level. Whether our own LZB/LZE reproduces it is what
M9 and M10 exist to answer, and this spike gets re-run against them.

## Threats to validity

- Single codec family (zstd). Our own codec may respond differently; finding 5
  is precisely the risk this creates, and milestone 10 re-runs this spike
  against `LZE`.
- Seek latencies are warm-cache, in-process, and exclude I/O. They measure
  decode cost only. Real `pread` latency adds storage time, which *favours*
  small blocks further (less data read per seek), so this bias is conservative.
- Two of six corpora are synthetic. The four real ones agree with each other.
- `srctext` at 345 MB is smaller than the multi-GB target scale.

# Security policy

## Status

**`xq` is pre-1.0 and has not been audited.** Do not use it as the only
barrier between untrusted input and something that matters. The format and API
are unstable.

## Reporting a vulnerability

Report privately through GitHub's security advisory form:

https://github.com/sssst9s/xq/security/advisories/new

Please do not open a public issue for a memory-safety or parsing bug.

Include the input that triggers it if you can - a file, or the fuzz seed and
iteration count. Expect an acknowledgement within a few days.

## Threat model

The decoder treats **every input as hostile**. A `.xq` file may have been
written by anyone, and the library's job is to reject a malformed one cleanly
rather than to trust it.

In scope, and treated as vulnerabilities:

- Any out-of-bounds read or write reachable from a crafted file
- An allocation whose size is taken from a file without a policy check
  (decompression bombs)
- Integer overflow in length or offset arithmetic that leads to either of the
  above
- A crafted file that causes unbounded memory or CPU use despite `mem_limit`
- Silent production of wrong output where the file should have been rejected

Out of scope:

- Passing invalid arguments through the C API. The library validates what it
  can, but a caller who lies about a buffer length is not a security boundary.
- Ratio-based side channels. `xq` performs no encryption; if you compress
  attacker-influenced data alongside secrets, that is a property of
  compression, not of this library.
- Resource use that the caller explicitly permitted via `mem_limit = 0`.

## What the design does about it

Detail in [docs/internals.md](docs/internals.md).

- `src/format/` performs no I/O and no allocation, so the entire parser for
  untrusted bytes is reachable from a fuzz harness with no setup.
- Every fixed structure carries a CRC that is verified **before** any field it
  covers is used. `stored_size` in particular becomes a read length, so a
  false accept there is an overread.
- Length and offset arithmetic goes through overflow-checked helpers, because
  `if (offset + size <= file_size)` passes trivially when the sum wraps.
- Buffer sizes derived from a file are clamped against `mem_limit` before any
  allocation.
- The test suite flips every bit of every fixed structure and asserts none is
  ever accepted, and truncates files at every possible length.
- Builds run under UndefinedBehaviorSanitizer with
  `-fno-sanitize-recover=all`, including unsigned overflow.

## Known gaps

Stated plainly rather than discovered later.

- **Threading is POSIX-only.** The Windows positional-read fallback is not
  thread-safe, so threaded use there is unsupported.
- **The shared dictionary is a single point of failure.** Damage it and every
  block referencing it is undecodable. It carries its own checksum so this is
  detected, never silent, and `dict_size = 0` disables it entirely.
- **AddressSanitizer coverage is CI-only.** The vendor ASan runtime is broken
  on current macOS; local runs use UBSan.
- **No coverage-guided fuzzing on macOS.** Apple's clang ships no libFuzzer
  runtime, so local fuzzing uses the structure-aware standalone driver.

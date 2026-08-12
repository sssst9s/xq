# Contributing

## Before anything else

```sh
make check          # tests, UBSan and fuzzing, both must be clean
make check WITH_ZSTD=1
```

A patch that does not pass both is not ready. There are no warnings in this
codebase and it builds at `-Wall -Wextra -Wpedantic -Wconversion`; keep it
that way.

## House rules

**Read [docs/internals.md](docs/internals.md) first.** Several decisions in
`src/` look arbitrary and are not - the separate block header CRC, the
fixed-width index entries, the reserved record tag `0x0C`, the digest-once
dictionary interface. Each is there for a measured or security reason recorded
in that document.

**The source files carry no comments. Do not add any.** The only thing above
the first line of code is the SPDX identifier. Rationale belongs in
`docs/internals.md` and in the commit message, not inline, so that the
explanation sits in one place instead of drifting out of date next to the code
it describes.

This is enforced, not merely preferred: a patch that adds comments to `src/`,
`cli/`, `tests/` or `benchmarks/` will be sent back. Write the reasoning into
`docs/internals.md` instead, where it can be read as a whole.

**`src/format/` is pure.** No I/O, no allocation, no global state. This is what
makes the untrusted-input parser exhaustively fuzzable. Do not put a `read()`
in it.

**Validate before you believe.** Never return or use a length or offset drawn
from input before verifying the CRC that covers it. Never size an allocation
from untrusted input without checking it against a policy limit first.

**No new dependencies** in the default build. That is the point of the
project. Optional, off-by-default integrations behind a build flag are fine.

## Changing the format

The on-disk format is specified in [docs/format-spec.md](docs/format-spec.md),
which is normative. `src/format/xq_format.h` is normative for constants; the
two must agree.

- Additive change that old readers can skip → bump `format_minor`, add a
  non-critical record.
- Anything that changes how existing bytes are interpreted → bump
  `format_major`, and expect to be asked why.
- New record tags may never be `0x0C`. A static assertion enforces this.

Update the spec in the same commit as the code. A format change without a spec
change will be sent back.

## Tests

Every bug fix gets a regression test. Test the boundary, not the middle:
zero-length input, exactly one block, one byte either side of a block edge,
and the incompressible case. Several real bugs here were invisible on ordinary
data and only appeared at those edges.

For anything touching parsing, add cases to the malformed-input suite in
`tests/test_format.c`. The existing sweeps flip every bit of every fixed
structure and truncate at every possible length; new structures deserve the
same.

## Performance claims

Do not make one without a script in `benchmarks/` that reproduces it, and
report the losses alongside the wins. `xq` is not the densest or the fastest
option available, and being straight about that is what makes the
random-access numbers credible.

## Commits

Explain *why*, not *what* - the diff already says what. Reference the design
document or measurement that motivated the change where one exists.

## Licence

Contributions are accepted under Apache-2.0. By submitting a patch you certify
you have the right to do so.

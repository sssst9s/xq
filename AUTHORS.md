# Authors

`xq` is developed by the people listed here. Additions are welcome - see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Maintainers

- rayan - project lead, original author

## Contributors

<!-- Add yourself in your first pull request, alphabetically by surname. -->

## Acknowledgements

`xq` vendors no third-party code, but it builds on published work and it would
be poor form not to say so.

- **Yann Collet**, for the XXH64 specification that `src/common/xq_xxh64.c`
  implements, and for Zstandard, used as the optional codec and as the
  measurement baseline during the feasibility work.
- **Guy Castagnoli et al.**, for the CRC-32C polynomial.
- The authors of the many block-structured and seekable compression formats
  that came before this one.

# Installing

`xq` has no dependencies. A C11 compiler and `make` are the whole requirement.

- [Quick start](#quick-start)
- [Requirements](#requirements)
- [Build options](#build-options)
- [Installing the binary and library](#installing-the-binary-and-library)
- [Using the library in your project](#using-the-library-in-your-project)
- [Verifying a build](#verifying-a-build)
- [Platform notes](#platform-notes)
- [Troubleshooting](#troubleshooting)

## Quick start

```sh
git clone https://github.com/sssst9s/xq
cd xq
make
./build/xq --help
```

That produces:

| path | what |
|---|---|
| `build/xq` | the command line tool |
| `build/libxq.a` | the static library |
| `include/xq.h` | the public header |

There is no `make install` yet. Copy the binary somewhere on your `PATH`:

```sh
sudo install -m 755 build/xq /usr/local/bin/xq
```

## Requirements

| | minimum | notes |
|---|---|---|
| compiler | C11 | gcc, clang and Apple clang are tested |
| make | any POSIX make | GNU make and BSD make both work |
| threads | pthreads | present on every POSIX system; only needed for `-T` |

No cmake, no meson, no configure script, no vendored third-party code. That is
deliberate: a build-system dependency would undercut the point of a library
whose selling point is not having dependencies.

## Build options

Everything is set on the `make` command line.

```sh
make                      # optimised build, no dependencies
make WITH_ZSTD=1          # additionally build the optional reference codec
make CC=gcc-14            # choose a compiler
make CFLAGS="-O3 -march=native"
make BUILD=out            # build somewhere other than build/
make clean
```

### `WITH_ZSTD=1`

Builds an optional codec that links against libzstd, used as a comparison
baseline. It is **off by default** and never selected implicitly; you have to
ask for it with `-c zstd`.

```sh
make WITH_ZSTD=1
./build/with-zstd/xq compress -c zstd -o out.xq file
```

Note the different output directory. Configurations build into separate
directories on purpose: `make` tracks file timestamps, not the flags a file was
compiled with, so mixing them would produce a link error or, worse, a working
binary built from mismatched objects.

Requires libzstd headers and library. On Debian or Ubuntu,
`apt install libzstd-dev`; on macOS with Homebrew, `brew install zstd`, which
the Makefile finds automatically.

A file written with `-c zstd` records that codec, and a build without it will
refuse the file with `unsupported codec` rather than misreading it.

## Installing the binary and library

There is no install target yet. By hand:

```sh
sudo install -m 755 build/xq          /usr/local/bin/xq
sudo install -m 644 build/libxq.a     /usr/local/lib/libxq.a
sudo install -m 644 include/xq.h      /usr/local/include/xq.h
```

Teach `file(1)` to recognise the format while you are there:

```sh
cat contrib/xq.magic >> ~/.magic
```

## Using the library in your project

Static linking is the intended path; there is no shared library build.

```sh
cc myprog.c -I/usr/local/include -L/usr/local/lib -lxq -lpthread -o myprog
```

Or vendor it directly, which is a reasonable choice given the size and the
absence of dependencies:

```make
XQ_SRC := $(wildcard third_party/xq/src/*/*.c)
CPPFLAGS += -Ithird_party/xq/include $(addprefix -Ithird_party/xq/src/,common format codec core platform)
LDLIBS += -lpthread
```

```c
#include <xq.h>
```

`-lpthread` is needed only if you use more than one thread; on glibc 2.34 and
later it is part of libc and the flag is harmless.

## Verifying a build

```sh
make check
```

Runs the unit and malformed-input tests, the round-trip and reader tests, the
same suite again under UndefinedBehaviorSanitizer, and the fuzzers. It should
end with `all checks passed` and produce no compiler warnings.

Individually:

```sh
make test         # tests only
make ubsan        # tests under UndefinedBehaviorSanitizer
make asan         # tests under AddressSanitizer
make fuzz-run     # fuzzers
make bench        # build the benchmarks
```

A quick functional smoke test:

```sh
./build/xq compress -o /tmp/t.xq README.md
./build/xq verify /tmp/t.xq
./build/xq decompress -o /tmp/t.out /tmp/t.xq
cmp README.md /tmp/t.out && echo ok
```

## Platform notes

### Linux

Builds and tests cleanly with gcc and clang. This is where AddressSanitizer
coverage comes from.

### macOS

Builds with the Apple clang that ships with the Command Line Tools. Two
vendor toolchain limitations, neither specific to this project:

- **`make asan` hangs before `main()`.** The Apple AddressSanitizer runtime
  hangs during startup for every binary on recent macOS, including
  `int main(void){return 0;}`. Use `make ubsan` locally.
- **No libFuzzer runtime.** `make fuzz` detects this and builds a
  structure-aware standalone driver instead, so fuzzing still runs.

### Windows

The library builds, but **threading is not supported**: the positional-read
fallback there is emulated with `lseek` plus `read`, which is not thread-safe.
Use `-T 1`. The Makefile itself assumes a POSIX shell, so use MSYS2 or WSL, or
compile the sources directly with your own build system.

### Big-endian and unusual architectures

All integer access goes through byte-wise load and store helpers, so the
on-disk format is identical everywhere. CRC-32C uses ARMv8 or SSE4.2
instructions where available and a table otherwise; the two are cross-checked
against each other in the test suite, so a file written on one machine
verifies on any other.

## Troubleshooting

**`unsupported codec` when reading a file.** It was written with a codec this
build does not have, almost always `zstd`. Rebuild with `make WITH_ZSTD=1`.

**`make: *** No rule to make target`.** You are probably in the wrong
directory, or using a make too old to handle the wildcards. GNU make 3.81 and
later work.

**Link error mentioning `pthread_create`.** Add `-lpthread`.

**`fatal error: zstd.h: No such file or directory`.** `WITH_ZSTD=1` was
requested without libzstd installed. Either install it, or drop the flag; the
default build needs nothing.

**Sanitiser build hangs on macOS.** Expected, see the platform notes above.
Use `make ubsan`.

**Tests fail after switching `WITH_ZSTD`.** They should not, because the two
configurations build into separate directories. If you overrode `BUILD` to the
same path for both, `make clean` first.

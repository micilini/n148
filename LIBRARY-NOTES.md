<!--
N.148i v1 library implementation notes
Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.
-->

# N.148i v1 library notes

## Outcome

N.148i 1.0.0 is available through the single public header
[`include/n148i.h`](include/n148i.h) and memory-to-memory encode/decode calls.
The v1 file-format number remains 1. PPM loading and saving remain private CLI
utilities.

The existing Makefile targets now link the static library. CMake 3.16 or newer
builds and installs static and shared libraries, the public header, a
`pkg-config` file, and a `find_package(n148i)` package. The Linux shared object
uses `SOVERSION 1`, hidden visibility, and an ELF version script.

## Validation performed on this machine

Host: Intel Core 7 150U, Linux 7.0.0-28-generic x86-64, GCC 13.3.0, and CMake
3.28.3.

- `make`, `make bench`, `make compare`, `make validate`, `make libraries`, and
  `make benchmark-final JXL_PREFIX=/tmp/n148-libjxl-0.12.0/install` built
  successfully.
- `./compare images/example.ppm 50 20` produced the required 2,290-byte N.148i
  stream.
- `./validate` reported `validation PASS (0 failures)`, including scalar versus
  AVX2, every chroma mode, odd dimensions, thread-count determinism, exact
  payload consumption, and the new public API versus the internal serializer.
- `make NOTHREADS=1 validate && ./validate` also passed with zero failures.
- A clean release CMake build passed both registered CTest tests.
- AddressSanitizer plus UndefinedBehaviorSanitizer builds passed the memory
  example and full validator with no report.
- The installed example compiled and ran through both
  `find_package(n148i CONFIG)` and `pkg-config`.
- The public header compiled as C++17, confirming the `extern "C"` boundary.
- `nm -D --defined-only` showed the ABI version node and twelve exported
  `n148i_*` functions; no unprefixed internal function was exported.

Before changing the CLI, four streams were captured from the original source.
The library-backed CLI reproduced every file byte for byte:

| Input and settings | Bytes | SHA-256 |
|---|---:|---|
| `example.ppm`, q50, 4:2:0 | 2,290 | `28887b2681b2556b6087d536fe6957ada7fae8aba07cd5da58cd4d40d0dba267` |
| `corpus-0022.ppm`, q75, 4:4:4 | 53,694 | `4086b92ee17bcff9b78fbfbd3244ccf69cac9df704997507b8aeaedae1b4dd98` |
| `corpus-0073.ppm`, q30, 4:2:2 | 8,148 | `c1f240c744332c705f65fc7322fd89341c33d218ce5345b03401f3835ffe1b63` |
| `corpus-0107.ppm`, q90, 4:2:0 | 107,105 | `1e4c4b1374a90dff3d2eb36a4538f201014cc9e2bf370a254838f64eacd90bd6` |

These checks establish that the library work did not change the v1 bitstream.

## Produced artifacts

The verified native package is in the ignored directory
`builds/linux-x86_64/`. It contains:

- `lib/libn148i.so.1.0.0`, with `libn148i.so.1` and `libn148i.so` symlinks;
- `lib/static/libn148i.a`;
- `include/n148i.h`, `LICENSE`, and `README.txt`;
- `lib/pkgconfig/n148i.pc` and `lib/cmake/n148i/`;
- `bin/n148i`, linked against the static library.

At the time of the final local build, the shared-library SHA-256 was
`1596dee2eaa48c52514b02b270c24022e316966b76ae845bc42881d6913d849e`
and the static-library SHA-256 was
`65b94a67e45e7bac5d5454c3b366ee887595344da6765e742397c96dfd753579`.
Build IDs and archive hashes can legitimately vary with toolchain and path.

## Platform limits and honest status

Windows binaries were not produced locally. MinGW-w64 was absent, and this
machine did not permit noninteractive package installation. The checked-in
toolchain supports an x86-64 MinGW build, while the GitHub Actions workflow
uses a native Windows runner. Windows v1 deliberately compiles the tested
scalar, single-thread fallback because the current worker pool uses pthreads.
The DLL exports the public declarations through `__declspec(dllexport)` and
consumers see `__declspec(dllimport)`.

macOS binaries were not produced locally because an Apple SDK is not available
or redistributable on this Linux host. CMake defines a 10.15 deployment target,
the README contains the native universal-build command, and the workflow uses a
macOS runner to build `x86_64;arm64` artifacts.

The workflow is checked in but was not run from this unpushed branch. Its
Windows and macOS results must therefore be verified in GitHub Actions before
publishing those release archives.

## Deliberate portability-only source changes

No codec algorithm or format rule was changed. The only edits adjacent to the
core were isolated portability changes:

- GCC and Clang still use the same `__builtin_ctzll` operation, now behind a
  helper that supplies the corresponding MSVC intrinsic;
- MSVC reports the scalar path because the v1 target-attributed SIMD routines
  are GCC/Clang implementations;
- Windows selects `N148_NO_THREADS`, and its build avoids the pthread-only
  worker code.

The byte-for-byte comparisons above cover the Linux path affected by the
source refactoring.

## Thread-safety boundary

Header parsing, version/error strings, and deallocation can run concurrently.
Encode and decode calls in v1 require application-level serialization because
the existing implementation shares dispatch state, thread configuration,
encoder histogram scratch space, and a persistent pthread pool. SIMD forcing
and thread-count changes must happen before codec work begins. This limitation
is documented in both the public README and header-facing API description.

## Reproduction entry points

- Library builds and consumption: [`README.md`](README.md#build-the-library)
- Full benchmark reproduction: [`tools/README.md`](tools/README.md)
- Scientific benchmark report: [`BENCHMARK.md`](BENCHMARK.md)
- Native release workflow:
  [`.github/workflows/build-libraries.yml`](.github/workflows/build-libraries.yml)

# N.148i V2 library

The public interface is [include/n148i.h](include/n148i.h). The codec has no
dependency on JPEG, WebP or JPEG XL; those libraries are benchmark references.

## Build and consume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release
cmake -S examples -B consumer-build -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build consumer-build --config Release
ctest --test-dir consumer-build -C Release --output-on-failure
```

On Windows, add the installed `bin` directory to `PATH` before the consumer
tests, or place `n148i.dll` beside the executables. For manual static linkage,
define `N148I_STATIC_DEFINE`. Exported CMake targets set this automatically:

```cmake
find_package(n148i 2 CONFIG REQUIRED)
target_link_libraries(app PRIVATE n148i::n148i)
# Or: n148i::n148i_static
```

Use libraries built with a compatible compiler and runtime. The local MinGW
DLL also requires `libgcc_s_seh-1.dll`; MSVC builds use the MSVC runtime.
See the [Windows package notes](packaging/README-Windows.txt).

The product name and `n148i_library_version()` result are **V2**. CMake,
pkg-config and shared-library loaders require numeric versions: package
`2.0.0`, ABI major `2`. The on-stream format byte remains independently `7`.
V2 reads all seven supported format revisions; their constants are named
`N148I_FORMAT_VERSION_1` through `N148I_FORMAT_VERSION_7`.

## API contract

- Input/output pixels are interleaved 8-bit RGB, without alpha.
- Initialize options with `n148i_encode_options_init()` before setting fields.
- Default: quality 50, 4:2:0, detail reconstruction (`0xd8a6d7ff`), effort 3.
  `chroma_quality = 0` selects the same quality as luma for this profile.
- A nonzero input stride permits padded rows. Decoded rows are tightly packed.
- Free allocations with `n148i_free_buffer()` or `n148i_free_image()`.
- Encode/decode calls within a process require external serialization: the
  implementation shares dispatch, configuration, scratch memory and a pool.
  Internal worker threads do not make concurrent API calls safe.
- Format/header/error queries and deallocation can run independently.
- Configure SIMD and thread count before codec work. MSVC uses scalar,
  single-thread code; Apple Silicon uses scalar code with POSIX threading.
  Intel Linux/macOS builds can dispatch AVX2 at runtime.

V1 output uses format 1 and feature mask zero. Numbered default feature masks
describe compatible presets; the current default is `N148I_DEFAULT_FEATURES`.

## Validation recorded for this delivery

| Target | Evidence |
|---|---|
| Linux x86-64 | All 29 [release gates](benchmarks/release/library-validation/validation-summary.json) passed: normal and thread-free builds, codec validation, ASan/UBSan/leak checks, exported symbols, ABI soname, installation, C/C++ headers, static/shared consumers and example |
| Windows x86-64 | [MinGW cross compilation](benchmarks/release/windows-build.json) passed, including DLL, static/import libraries and separate installed consumers; native MSVC/runtime execution remains pending CI |
| macOS Intel / Apple Silicon | Universal builds and native API/installed-consumer tests configured in [GitHub Actions](.github/workflows/build-libraries.yml); execution remains pending CI |

The Linux suite exercises scalar/AVX2 equivalence, thread determinism, odd
dimensions, chroma layouts, format compatibility and corrupt inputs. Its full
SIMD suite requires an AVX2 Linux host. Public API roundtrip tests are also
configured for Windows and macOS.

In 650 image/quality combinations, the V2 default produced compressed bytes
and decoded RGB identical to the measured detail implementation.
[Equivalence evidence](benchmarks/release/promotion-equivalence.json).

A configured workflow is not a completed Windows/macOS execution. Check the
native results in the build workflow's run history when those jobs execute.

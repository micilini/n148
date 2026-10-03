N.148i V2 library
Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.

Include include/n148i.h and initialize options with
n148i_encode_options_init(). The V2 default is the detail reconstruction
profile (0xd8a6d7ff), effort 3, 4:2:0, with equal luma/chroma quality.
The product name is V2; the numeric package/ABI version is 2.0.0.
The on-stream format identifier is independently 7. Formats 1 through 7
remain readable. Use N148I_FORMAT_VERSION_1 and feature_flags = 0 for
legacy Huffman output; numbered format presets retain their original masks.
Encode/decode calls in a process require external serialization.
Free library allocations with n148i_free_buffer()/n148i_free_image().

CMake consumer:
  find_package(n148i 2 CONFIG REQUIRED)
  target_link_libraries(app PRIVATE n148i::n148i)
Use n148i::n148i_static for static linkage.

Shared library: bin/n148i.dll and lib/n148i.lib (import library).
Static library: lib/static/n148i.lib.
Copy n148i.dll alongside your executable, or add bin/ to PATH.
For a manual static link define N148I_STATIC_DEFINE in the consumer.
The MSVC build uses the portable scalar, single-thread implementation.

MSVC DLL builds require the matching Microsoft C runtime on the target system.
MinGW builds can additionally depend on libgcc_s_seh-1.dll; distribute the
compiler runtime with your application or make it available through PATH.
The local cross-build audit records the actual imported DLLs.

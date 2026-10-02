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

Shared library: lib/libn148i.so (ABI soname libn148i.so.2).
Static library: lib/static/libn148i.a.
  cc app.c -Iinclude -Llib -ln148i -Wl,-rpath,'$ORIGIN/lib' -o app
  cc app.c -DN148I_STATIC_DEFINE -Iinclude lib/static/libn148i.a -pthread -lm -o app
pkg-config metadata is in lib/pkgconfig. Linux x86-64 dispatches AVX2 at runtime.

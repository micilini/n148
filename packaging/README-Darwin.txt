N.148i v1 for macOS
Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.

The shared library is in lib/libn148i.dylib and the static library is in
lib/static/libn148i.a. Include include/n148i.h in your application.

Shared link example:
  cc app.c -Iinclude -Llib -ln148i -Wl,-rpath,@loader_path/lib -o app

Static link example:
  cc app.c -DN148I_STATIC_DEFINE -Iinclude lib/static/libn148i.a \
     -pthread -lm -o app

The release workflow builds one universal binary for x86-64 and Apple Silicon.

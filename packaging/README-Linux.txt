N.148i v1 for Linux
Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.

The shared library is in lib/libn148i.so and the static library is in
lib/static/libn148i.a. Include include/n148i.h in your application.

Shared link example:
  cc app.c -Iinclude -Llib -ln148i -Wl,-rpath,'$ORIGIN/lib' -o app

Static link example:
  cc app.c -DN148I_STATIC_DEFINE -Iinclude lib/static/libn148i.a \
     -pthread -lm -o app

The pkg-config and CMake package files under lib/ can also discover n148i.

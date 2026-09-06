N.148i v1 for Windows x86-64
Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.

The DLL is bin/n148i.dll and its import library is lib/n148i.lib. The static
library is lib/static/n148i.lib. Include include/n148i.h in your application.

When using the shared library, link the import library and place n148i.dll
beside your executable. Define N148I_STATIC_DEFINE before including n148i.h
when linking the static library.

The v1 Windows build uses the tested single-thread fallback because its
parallel implementation is based on POSIX pthreads.

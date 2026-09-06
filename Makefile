# N.148i codec and library - Linux convenience build
#
# Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.
#
# CMake is the portable distribution build. This Makefile preserves the
# original development targets and now links each executable against the
# static N.148i library instead of compiling the codec sources into every tool.

CC       ?= cc
AR       ?= ar
RANLIB   ?= ranlib
CPPFLAGS ?=
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  ?=
LDLIBS   := -lm

CPPFLAGS += -Iinclude -Isrc

JXL_PREFIX ?=
ifeq ($(strip $(JXL_PREFIX)),)
JXL_CFLAGS ?= $(shell pkg-config --cflags libjxl libjxl_threads 2>/dev/null)
JXL_LIBS ?= $(shell pkg-config --libs libjxl libjxl_threads 2>/dev/null)
JXL_LINK_FLAGS ?=
JXL_CHECK = pkg-config --atleast-version=0.8 libjxl && \
            pkg-config --exists libjxl_threads
else
JXL_CFLAGS ?= -I$(JXL_PREFIX)/include
JXL_LIBS ?= -L$(JXL_PREFIX)/lib -ljxl -ljxl_threads
JXL_LINK_FLAGS ?= -Wl,--disable-new-dtags -Wl,-rpath,"$(JXL_PREFIX)/lib"
JXL_CHECK = test -f "$(JXL_PREFIX)/include/jxl/encode.h" -a \
                 -f "$(JXL_PREFIX)/lib/libjxl.so" -a \
                 -f "$(JXL_PREFIX)/lib/libjxl_threads.so"
endif

BUILD_KIND := release
ifdef DEBUG
CFLAGS := -O0 -g -Wall -Wextra -Wno-unused-parameter
BUILD_KIND := debug
endif

ifndef NOTHREADS
CFLAGS  += -pthread
LDFLAGS += -pthread
else
CFLAGS  += -DN148_NO_THREADS
BUILD_KIND := $(BUILD_KIND)-nothreads
endif

SRCDIR   := src
BUILDDIR := .build/make/$(BUILD_KIND)
CORE_SOURCES := $(SRCDIR)/n148i.c $(SRCDIR)/header.c $(SRCDIR)/ppm.c \
                $(SRCDIR)/tables.c $(SRCDIR)/dct.c $(SRCDIR)/cpu.c \
                $(SRCDIR)/parallel.c $(SRCDIR)/huffman.c \
                $(SRCDIR)/encoder.c $(SRCDIR)/decoder.c
CORE_OBJECTS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CORE_SOURCES))
CORE_DEPS    := $(CORE_OBJECTS:.o=.d)

STATIC_LIBRARY := libn148i.a
SHARED_REAL    := libn148i.so.1.0.0
SHARED_SONAME  := libn148i.so.1
SHARED_LIBRARY := libn148i.so

all: n148i

libraries: $(STATIC_LIBRARY) $(SHARED_LIBRARY)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -MMD -MP -c $< -o $@

$(STATIC_LIBRARY): $(CORE_OBJECTS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(SHARED_REAL): $(CORE_OBJECTS) cmake/n148i.map
	$(CC) -shared -Wl,-soname,$(SHARED_SONAME) \
		-Wl,--version-script=cmake/n148i.map $(CORE_OBJECTS) \
		-o $@ $(LDFLAGS) $(LDLIBS)

$(SHARED_SONAME): $(SHARED_REAL)
	ln -sf $(SHARED_REAL) $@

$(SHARED_LIBRARY): $(SHARED_SONAME)
	ln -sf $(SHARED_SONAME) $@

n148i: $(SRCDIR)/main.c $(STATIC_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DN148I_STATIC_DEFINE $< \
		$(STATIC_LIBRARY) -o $@ $(LDFLAGS) $(LDLIBS)

bench: $(SRCDIR)/bench_cli.c $(STATIC_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DN148I_STATIC_DEFINE $< \
		$(STATIC_LIBRARY) -o $@ $(LDFLAGS) $(LDLIBS)

compare: $(SRCDIR)/compare_cli.c $(STATIC_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DN148I_STATIC_DEFINE $< \
		$(STATIC_LIBRARY) -o $@ $(LDFLAGS) $(LDLIBS) -ljpeg

benchmark-final: $(SRCDIR)/benchmark_final_cli.c $(STATIC_LIBRARY)
	@$(JXL_CHECK) || \
	  (echo "libjxl >= 0.8 development files not found via pkg-config or JXL_PREFIX=$(JXL_PREFIX)" >&2; exit 1)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(JXL_CFLAGS) -DN148I_STATIC_DEFINE $< \
		$(STATIC_LIBRARY) -o $@ $(LDFLAGS) $(LDLIBS) \
		-ljpeg $(JXL_LIBS) $(JXL_LINK_FLAGS)

validate: $(SRCDIR)/validate_cli.c $(STATIC_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DN148I_STATIC_DEFINE $< \
		$(STATIC_LIBRARY) -o $@ $(LDFLAGS) $(LDLIBS)

run: n148i
	./n148i

clean:
	rm -rf .build/make
	rm -f n148i bench compare validate benchmark-final \
		$(STATIC_LIBRARY) $(SHARED_LIBRARY) $(SHARED_SONAME) $(SHARED_REAL) \
		$(SRCDIR)/n148i $(SRCDIR)/n148i.exe $(SRCDIR)/bench

-include $(CORE_DEPS)

.PHONY: all libraries run clean bench compare benchmark-final validate

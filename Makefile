# N.148 codec - build rules
#
# The default build is portable: plain -O2, no architecture flags.
# Every vector path is selected at run time through CPUID, so the same
# binary stays correct on an old machine and fast on a modern one.
#
#   make            build the codec
#   make bench      build the benchmark driver
#   make run        build and run the codec
#   make compare    build the direct libjpeg-turbo comparison driver
#   make benchmark-final
#   make benchmark-final JXL_PREFIX=/path/to/libjxl/install
#                   build the direct N.148i/JPEG/JPEG XL benchmark driver
#   make clean      remove build output
#
# Useful switches:
#   make NOTHREADS=1    single threaded build
#   make DEBUG=1        no optimization, assertions friendly

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=
LDLIBS  := -lm

# The final benchmark can use either a system libjxl discovered by pkg-config
# or an explicit source-build installation.  The default codec remains free
# of third-party codec dependencies.
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

ifdef DEBUG
CFLAGS := -O0 -g -Wall -Wextra -Wno-unused-parameter
endif

ifndef NOTHREADS
CFLAGS  += -pthread
LDFLAGS += -pthread
else
CFLAGS  += -DN148_NO_THREADS
endif

SRCDIR  := src
SOURCES := $(SRCDIR)/header.c $(SRCDIR)/ppm.c $(SRCDIR)/tables.c \
           $(SRCDIR)/dct.c $(SRCDIR)/cpu.c $(SRCDIR)/parallel.c \
           $(SRCDIR)/huffman.c $(SRCDIR)/encoder.c $(SRCDIR)/decoder.c

all: n148i

n148i: $(SRCDIR)/main.c $(SOURCES)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

bench: $(SRCDIR)/bench_cli.c $(SOURCES)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

compare: $(SRCDIR)/compare_cli.c $(SOURCES)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS) -ljpeg

benchmark-final: $(SRCDIR)/benchmark_final_cli.c $(SOURCES)
	@$(JXL_CHECK) || \
	  (echo "libjxl >= 0.8 development files not found via pkg-config or JXL_PREFIX=$(JXL_PREFIX)" >&2; exit 1)
	$(CC) $(CFLAGS) $(JXL_CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS) \
	  -ljpeg $(JXL_LIBS) $(JXL_LINK_FLAGS)

validate: $(SRCDIR)/validate_cli.c $(SOURCES)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

run: n148i
	./n148i

clean:
	rm -f n148i bench compare validate benchmark-final $(SRCDIR)/n148i $(SRCDIR)/bench

.PHONY: all run clean bench compare benchmark-final validate

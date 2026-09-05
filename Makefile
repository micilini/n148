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
#   make clean      remove build output
#
# Useful switches:
#   make NOTHREADS=1    single threaded build
#   make DEBUG=1        no optimization, assertions friendly

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=
LDLIBS  := -lm

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

validate: $(SRCDIR)/validate_cli.c $(SOURCES)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

run: n148i
	./n148i

clean:
	rm -f n148i bench compare validate $(SRCDIR)/n148i $(SRCDIR)/bench

.PHONY: all run clean bench compare validate

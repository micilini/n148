#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdio.h>

#define HUFFMAN_TABLE_COUNT 4
#define HUFFMAN_MAX_SYMBOLS 256

enum {
    HUFFMAN_DC_LUMA = 0,
    HUFFMAN_AC_LUMA = 1,
    HUFFMAN_DC_CHROMA = 2,
    HUFFMAN_AC_CHROMA = 3
};

// Canonical Huffman table description. bits[length] stores how many codes
// have that length; values stores their symbols in canonical order.
typedef struct {
    int bits[17];
    unsigned char values[HUFFMAN_MAX_SYMBOLS];
    int value_count;
} HuffSpec;

void huffman_default_specs(HuffSpec specs[HUFFMAN_TABLE_COUNT]);
int huffman_build_optimized(const long frequencies[HUFFMAN_MAX_SYMBOLS],
                            HuffSpec *spec);
int huffman_spec_is_valid(const HuffSpec *spec);

long huffman_tables_size(const HuffSpec specs[HUFFMAN_TABLE_COUNT]);
int write_huffman_tables(FILE *file,
                         const HuffSpec specs[HUFFMAN_TABLE_COUNT]);
int read_huffman_tables(FILE *file,
                        HuffSpec specs[HUFFMAN_TABLE_COUNT]);

#endif

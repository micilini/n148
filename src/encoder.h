#ifndef ENCODER_H
#define ENCODER_H

#include "huffman.h"
#include "ppm.h"

// Statistics filled in by encode_image() so main.c can report them.
typedef struct {
    long blocks_y;
    long blocks_cb;
    long blocks_cr;
    long table_size;
    long data_size;
} EncodeStats;

// Encodes the three planes into a freshly allocated buffer.
// The caller owns and must free *out_buffer.
int encode_image(Plane *y, Plane *cb, Plane *cr, int quality, int optimize,
                 HuffSpec specs[HUFFMAN_TABLE_COUNT],
                 unsigned char **out_buffer, EncodeStats *stats);

#endif

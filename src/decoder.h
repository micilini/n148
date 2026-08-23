#ifndef DECODER_H
#define DECODER_H

#include "huffman.h"
#include "ppm.h"

typedef struct {
    long blocks_y;
    long blocks_cb;
    long blocks_cr;
    long bytes_consumed;
} DecodeStats;

// Decodes a compressed buffer into three newly allocated planes.
// The caller owns and must free the returned planes.
int decode_image(unsigned char *buffer, long buffer_size,
                 int width, int height, int quality, int chroma,
                 const HuffSpec specs[HUFFMAN_TABLE_COUNT],
                 Plane *y, Plane *cb, Plane *cr, DecodeStats *stats);

// Rebuilds an RGB image, upsampling chroma to full resolution.
int merge_channels(Plane *y, Plane *cb, Plane *cr,
                   int smooth, Image *output);

#endif

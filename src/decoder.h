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

/* Reconstruct one plane from format-1-compatible quantized coefficients. */
int n148_reconstruct_coeff_plane(const short *coefficients, long count,
                                 int width, int height, int quality,
                                 int chroma_plane, Plane *output);

int n148_reconstruct_coeff_plane_ex(const short *coefficients, long count,
                                    int width, int height, int quality,
                                    int chroma_plane, int perceptual_color,
                                    Plane *output);

// Rebuilds an RGB image, upsampling chroma to full resolution.
int merge_channels(Plane *y, Plane *cb, Plane *cr,
                   int smooth, Image *output);

/* Rebuild RGB with the flagged perceptual inverse transform. */
int n148_merge_channels_ex(Plane *first, Plane *second, Plane *third,
                           int smooth, int perceptual_color, Image *output);

#endif

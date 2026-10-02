#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

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

/* Internal bridge used by the entropy payload. Values stay in the exact format 1
   zig-zag order so changing the entropy coder cannot change quantization. */
typedef struct {
    short *coefficients;
    unsigned long long *nonzero_masks;
    unsigned char *nonzero_counts;
    long count;
} N148CoeffPlane;

// Encodes the three planes into a freshly allocated buffer.
// The caller owns and must free *out_buffer.
int encode_image(Plane *y, Plane *cb, Plane *cr, int quality, int optimize,
                 HuffSpec specs[HUFFMAN_TABLE_COUNT],
                 unsigned char **out_buffer, EncodeStats *stats);

int n148_quantize_planes(Plane *y, Plane *cb, Plane *cr, int quality,
                         N148CoeffPlane output[3]);

/* Select tables calibrated for the perceptual planes. */
int n148_quantize_planes_ex(Plane *first, Plane *second, Plane *third,
                            int quality, int perceptual_color,
                            N148CoeffPlane output[3]);
void n148_free_coeff_plane(N148CoeffPlane *plane);

#endif

/*
 * N.148i variable transform strategies.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_VARIABLE_TRANSFORM_H
#define N148_VARIABLE_TRANSFORM_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"

#define N148_TRANSFORM_REGION_SIZE 16
#define N148_TRANSFORM_STRATEGY_COUNT 3
#define N148_TRANSFORM_INITIAL_STRATEGY N148_TRANSFORM_DCT8

enum {
    N148_TRANSFORM_DCT4 = 0,
    N148_TRANSFORM_DCT8 = 1,
    N148_TRANSFORM_DCT16 = 2,
};

/* Strategies are concatenated in Y, Cb, Cr order. Geometry is retained so
   prediction never crosses a plane boundary and edge DCT16 entries can be
   rejected before coefficient reconstruction. */
typedef struct {
    uint8_t *strategies;
    size_t count;
    size_t plane_offsets[3];
    int columns[3];
    int rows[3];
    int blocks_x[3];
    int blocks_y[3];
} N148TransformMap;

int n148_transform_map_allocate(int width, int height, int chroma,
                                N148TransformMap *map);
int n148_transform_map_build(const Plane *y, const Plane *cb, const Plane *cr,
                             N148TransformMap *map);
int n148_transform_map_validate(const N148TransformMap *map);
/* The variable-luma profile uses DCT8/DCT16 in luma and leaves chroma at DCT8. */
int n148_luma_transform_map_validate(const N148TransformMap *map);
/* Format 6 represents one 32x32 luma transform as a canonical aligned 2x2 group
   of DCT4 map symbols. DCT4 was unreachable in the format 5 luma grammar, so the
   marker extends the existing three-symbol entropy alphabet without changing
   any format 1-5 syntax. */
int n148_luma_transform_map_validate_extended(const N148TransformMap *map,
                                               int allow_dct32);
int n148_luma_transform_dct32_origin(const N148TransformMap *map,
                                     int macro_x, int macro_y);
int n148_luma_transform_dct32_member(const N148TransformMap *map,
                                     int macro_x, int macro_y);
uint8_t n148_transform_map_predict(const N148TransformMap *map,
                                   size_t index);
int n148_transform_strategy_at(const N148TransformMap *map, int plane,
                               int macro_x, int macro_y, uint8_t *strategy);
void n148_transform_map_release(N148TransformMap *map);

/* Normative Q14 integer DCT primitives used by the 4x4, 16x16, and format 6 32x32
   strategies. Input and output arrays are row-major and may alias. */
/* Continuous synthesis energy of the native 4x4 Q14 basis at a positive
   quantizer step. Used only for encoder distortion costs, before rounding. */
double n148_variable_dct4_energy(int natural, int step);
int n148_variable_forward(const int32_t *input, int size, int32_t *output);
int n148_variable_inverse(const int32_t *input, int size, int32_t *output);
/* The caller supplies exact nonzero frequency-row and frequency-column masks
   for the 16x16 input. */
int n148_variable_inverse_sparse16(const int32_t input[16 * 16],
                                   uint32_t active_rows,
                                   uint32_t active_columns,
                                   int32_t output[16 * 16]);
extern const uint16_t N148_ZIGZAG_4[16];
extern const uint16_t N148_ZIGZAG_16[256];
extern const uint16_t N148_ZIGZAG_32[1024];

static inline int n148_variable_zigzag(int size, int position) {
    if ((size != 4 && size != 16 && size != 32) || position < 0 ||
        position >= size * size) return -1;
    if (size == 4) return N148_ZIGZAG_4[position];
    if (size == 16) return N148_ZIGZAG_16[position];
    return N148_ZIGZAG_32[position];
}
uint64_t n148_variable_transform_matrix_hash(void);
uint64_t n148_variable_transform_matrix32_hash(void);

#endif

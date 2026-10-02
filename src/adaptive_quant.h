/*
 * N.148i adaptive quantization map.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_ADAPTIVE_QUANT_H
#define N148_ADAPTIVE_QUANT_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"

#define N148_AQ_REGION_SIZE 16
#define N148_AQ_LEVEL_COUNT 4
#define N148_AQ_INITIAL_LEVEL 1

typedef struct {
    uint8_t *levels;
    size_t count;
    int columns;
    int rows;
} N148AdaptiveMap;

/* Allocate a zeroed map whose dimensions are implied by the RGB canvas. */
int n148_adaptive_map_allocate(int width, int height, N148AdaptiveMap *map);

/* Select levels from local activity in the full-resolution intensity plane. */
int n148_adaptive_map_build(const Plane *intensity, N148AdaptiveMap *map);

/* Derive the causal left/above predictor for one raster-order map entry. */
uint8_t n148_adaptive_map_predict(const N148AdaptiveMap *map, size_t index);

/* Find the map level for an 8x8 block. Subsampling shifts are zero or one. */
int n148_adaptive_level_for_block(const N148AdaptiveMap *map,
                                  int block_x, int block_y,
                                  int subsample_x, int subsample_y,
                                  uint8_t *level);

/* Apply the normative per-level multiplier to a quality-scaled table. */
int n148_adaptive_adjust_table(const int input[8][8], uint8_t level,
                               int output[8][8]);

void n148_adaptive_map_release(N148AdaptiveMap *map);

#endif

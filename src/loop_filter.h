/*
 * N.148i causal deblocking and deringing filter.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_LOOP_FILTER_H
#define N148_LOOP_FILTER_H

#include <stdint.h>

#include "ppm.h"

typedef struct {
    uint64_t deblocked_samples;
    uint64_t deringed_samples;
} N148LoopFilterStats;

typedef struct {
    int strength;
    int threshold;
    int use_avx2;
} N148LoopFilterPrepared;

typedef enum {
    N148_LOOP_FILTER_OFF = 0,
    N148_LOOP_FILTER_LEGACY = 1,
    N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA = 2
} N148LoopFilterPolicy;

/* Return the normative maximum per-pass sample adjustment. */
int n148_loop_filter_strength(int quality, int plane_index,
                              uint8_t adaptive_level);

/* Derive the quant-adaptive luma strength from the calibrated residual table's nominal
   DC quantization step. This policy deliberately ignores segmentation. */
int n148_loop_filter_quant_luma_strength(int quality);

/* Prepare the invariant parameters once when reconstructing a plane. */
int n148_loop_filter_prepare(int quality, int plane_index,
                             uint8_t adaptive_level, int quant_adaptive_luma,
                             N148LoopFilterPrepared *prepared);
int n148_loop_filter_block_prepared(Plane *plane, int x0, int y0, int size,
                                    const N148LoopFilterPrepared *prepared,
                                    N148LoopFilterStats *stats);

/* Filter a newly reconstructed square before it becomes a prediction
   reference. The square size is 8, 16, or 32. Only already available top/left
   boundaries are crossed, which makes raster-order encoder and decoder
   behavior identical. */
int n148_loop_filter_block(Plane *plane, int x0, int y0, int size,
                           int quality, int plane_index,
                           uint8_t adaptive_level,
                           N148LoopFilterStats *stats);

/* Apply the quant-adaptive policy to luma while retaining the base
   policy for chroma. */
int n148_loop_filter_quant_luma_block(Plane *plane, int x0, int y0, int size,
                              int quality, int plane_index,
                              N148LoopFilterStats *stats);

#endif

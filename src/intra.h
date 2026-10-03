/*
 * N.148i reconstructed-neighbor intra prediction.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_INTRA_H
#define N148_INTRA_H

#include <stddef.h>
#include <stdint.h>

#include "adaptive_quant.h"
#include "encoder.h"
#include "ppm.h"
#include "variable_transform.h"

enum {
    N148_INTRA_DC = 0,
    N148_INTRA_VERTICAL = 1,
    N148_INTRA_HORIZONTAL = 2,
    N148_INTRA_TRUE_MOTION = 3,
    N148_INTRA_MODE_COUNT = 4,
};

/* Quantize prediction residuals in Y, Cb, Cr order. Modes contains one value
   per 8x8 block in the same order and is allocated by this function. */
int n148_intra_quantize_planes(const Plane *y, const Plane *cb,
                               const Plane *cr, int quality,
                               int perceptual_color,
                               const N148AdaptiveMap *adaptive_map,
                               const N148TransformMap *transform_map,
                               int rdo_effort, int loop_filter,
                               N148CoeffPlane coefficients[3],
                               uint8_t **modes, size_t *mode_count);

/* Reconstruct all three planes from residual coefficients and prediction
   modes. The scalar inverse transform is normative for predictor feedback. */
int n148_intra_reconstruct_planes(const N148CoeffPlane coefficients[3],
                                  const uint8_t *modes, size_t mode_count,
                                  int width, int height, int quality,
                                  int chroma, int perceptual_color,
                                  const N148AdaptiveMap *adaptive_map,
                                  const N148TransformMap *transform_map,
                                  int loop_filter,
                                  Plane *y, Plane *cb, Plane *cr);

/* Return the exact number of prediction modes implied by the dimensions. */
int n148_intra_expected_modes(int width, int height, int chroma,
                              size_t *mode_count);

#endif

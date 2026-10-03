/*
 * N.148i luma-profile codec interface
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_LUMA_PROFILE_H
#define N148_LUMA_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"
#include "entropy_payload.h"
#include "chroma_profile.h"

#define N148_FEATURE_CALIBRATED_LUMA_QUANT (1u << 17)
#define N148_FEATURE_LUMA_TRANSFORM_16X16 (1u << 18)
#define N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER (1u << 19)

#define N148_FORMAT_5_SUPPORTED_FEATURES \
    (N148_FORMAT_4_SUPPORTED_FEATURES | N148_FEATURE_CALIBRATED_LUMA_QUANT | \
     N148_FEATURE_LUMA_TRANSFORM_16X16 | \
     N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER)

#define N148_CALIBRATED_LUMA_PROFILE_FEATURES \
    (N148_CALIBRATED_CHROMA_PROFILE_FEATURES | N148_FEATURE_CALIBRATED_LUMA_QUANT)

#define N148_VARIABLE_LUMA_PROFILE_FEATURES \
    (N148_CALIBRATED_LUMA_PROFILE_FEATURES | N148_FEATURE_LUMA_TRANSFORM_16X16)

#define N148_PERCEPTUAL_LUMA_PROFILE_FEATURES \
    (N148_VARIABLE_LUMA_PROFILE_FEATURES | N148_FEATURE_PERCEPTUAL_TRELLIS)

#define N148_QUANT_ADAPTIVE_LUMA_FILTER_PROFILE_FEATURES \
    (N148_VARIABLE_LUMA_PROFILE_FEATURES | N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER)

int n148_format_5_features_valid(uint32_t features);

int n148_encode_format_5(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output);

int n148_decode_format_5(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr);

#endif

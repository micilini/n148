/*
 * N.148i fidelity-profile codec interface
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_FIDELITY_PROFILE_H
#define N148_FIDELITY_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"
#include "entropy_payload.h"
#include "luma_profile.h"

#define N148_FEATURE_FILTERED_INTRA_REFERENCES (1u << 20)
#define N148_FEATURE_PLANAR_INTRA_PREDICTION (1u << 21)
#define N148_FEATURE_STRUCTURAL_RDO (1u << 22)
#define N148_FEATURE_QUALITY_LAMBDA (1u << 23)
#define N148_FEATURE_LUMA_TRANSFORM_32X32 (1u << 24)
#define N148_FEATURE_SECOND_ORDER_DC (1u << 25)
#define N148_FEATURE_FINE_INTRA_DIRECTIONS (1u << 26)

#define N148_FORMAT_6_SUPPORTED_FEATURES \
    (N148_FORMAT_5_SUPPORTED_FEATURES | \
     N148_FEATURE_FILTERED_INTRA_REFERENCES | \
     N148_FEATURE_PLANAR_INTRA_PREDICTION | \
     N148_FEATURE_STRUCTURAL_RDO | \
     N148_FEATURE_QUALITY_LAMBDA | \
     N148_FEATURE_LUMA_TRANSFORM_32X32 | \
     N148_FEATURE_SECOND_ORDER_DC | \
     N148_FEATURE_FINE_INTRA_DIRECTIONS)

#define N148_DCT32_PROFILE_FEATURES \
    (N148_VARIABLE_LUMA_PROFILE_FEATURES | N148_FEATURE_LUMA_TRANSFORM_32X32)
#define N148_FILTERED_REFERENCE_PROFILE_FEATURES \
    (N148_VARIABLE_LUMA_PROFILE_FEATURES | N148_FEATURE_FILTERED_INTRA_REFERENCES)
#define N148_PLANAR_PREDICTION_PROFILE_FEATURES \
    (N148_VARIABLE_LUMA_PROFILE_FEATURES | N148_FEATURE_PLANAR_INTRA_PREDICTION)
#define N148_COMBINED_PREDICTION_PROFILE_FEATURES \
    (N148_VARIABLE_LUMA_PROFILE_FEATURES | N148_FEATURE_FILTERED_INTRA_REFERENCES | \
     N148_FEATURE_PLANAR_INTRA_PREDICTION)
#define N148_STRUCTURAL_RDO_PROFILE_FEATURES \
    (N148_PLANAR_PREDICTION_PROFILE_FEATURES | N148_FEATURE_STRUCTURAL_RDO)
#define N148_QUALITY_LAMBDA_PROFILE_FEATURES \
    (N148_PLANAR_PREDICTION_PROFILE_FEATURES | N148_FEATURE_QUALITY_LAMBDA)

int n148_format_6_features_valid(uint32_t features);

int n148_encode_format_6(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output);

int n148_decode_format_6(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr);

#endif

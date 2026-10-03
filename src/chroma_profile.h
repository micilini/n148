/*
 * N.148i chroma-profile codec interface
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_CHROMA_PROFILE_H
#define N148_CHROMA_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"
#include "entropy_payload.h"
#include "predictive_codec.h"

/* Reconstruction-aware chroma changes the encoder's 4:2:0 source reduction.
   The reconstructed-plane syntax remains the accepted format 3 syntax, so
   older formats stay isolated and format 4 can signal its input preparation. */
#define N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA (1u << 12)
#define N148_FEATURE_JOINT_CHROMA_INTRA (1u << 13)
#define N148_FEATURE_CALIBRATED_CHROMA_QUANT (1u << 14)
#define N148_FEATURE_SEGMENTATION (1u << 15)
#define N148_FEATURE_ADAPTIVE_LOOP_FILTER (1u << 16)

#define N148_FORMAT_4_SUPPORTED_FEATURES \
    (N148_FORMAT_3_SUPPORTED_FEATURES | \
     N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA | \
     N148_FEATURE_JOINT_CHROMA_INTRA | \
     N148_FEATURE_CALIBRATED_CHROMA_QUANT | \
     N148_FEATURE_SEGMENTATION | \
     N148_FEATURE_ADAPTIVE_LOOP_FILTER)

#define N148_RECONSTRUCTION_AWARE_CHROMA_PROFILE_FEATURES \
    (N148_DIRECTIONAL_INTRA_PROFILE_FEATURES | N148_FEATURE_CONTEXTUAL_RDO | \
     N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA)

#define N148_JOINT_CHROMA_PROFILE_FEATURES \
    (N148_RECONSTRUCTION_AWARE_CHROMA_PROFILE_FEATURES | N148_FEATURE_JOINT_CHROMA_INTRA)

#define N148_CALIBRATED_CHROMA_PROFILE_FEATURES \
    (N148_RECONSTRUCTION_AWARE_CHROMA_PROFILE_FEATURES | N148_FEATURE_CALIBRATED_CHROMA_QUANT)

#define N148_SEGMENTED_CHROMA_PROFILE_FEATURES \
    (N148_CALIBRATED_CHROMA_PROFILE_FEATURES | N148_FEATURE_SEGMENTATION)

#define N148_ADAPTIVE_CHROMA_FILTER_PROFILE_FEATURES \
    (N148_SEGMENTED_CHROMA_PROFILE_FEATURES | N148_FEATURE_ADAPTIVE_LOOP_FILTER)

int n148_format_4_features_valid(uint32_t features);

int n148_encode_format_4(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output);

int n148_decode_format_4(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr);

#endif

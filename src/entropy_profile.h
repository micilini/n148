/*
 * N.148i adaptive-entropy profile codec interface
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_ENTROPY_PROFILE_H
#define N148_ENTROPY_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"
#include "entropy_payload.h"
#include "fidelity_profile.h"

#define N148_FEATURE_ADAPTIVE_ENTROPY (1u << 27)
#define N148_FEATURE_RICH_COEFFICIENT_CONTEXTS (1u << 28)
#define N148_FEATURE_MULTIPLE_INTRA_REFERENCES (1u << 29)
#define N148_FEATURE_DIRECTIONAL_SCAN (1u << 30)
#define N148_FEATURE_DIRECTIONAL_TRANSFORM (1u << 31)
/* The format 7 interpretation of a low feature bit previously used only by
   format 2. Format 7 headers make the table choice explicit to the decoder. */
#define N148_FEATURE_SPECTRAL_LUMA_QUANT (1u << 3)
#define N148_FEATURE_REFINED_SPECTRAL_LUMA_QUANT (1u << 4)
#define N148_FEATURE_STRUCTURAL_LUMA_QUANT (1u << 5)
#define N148_FEATURE_BALANCED_RECONSTRUCTION (1u << 31)
#define N148_FEATURE_DETAIL_RECONSTRUCTION (1u << 30)

/* Grow this mask only when an experiment has a complete encoder/decoder
   implementation.  Planned public bits remain rejected until that point. */
#define N148_FORMAT_7_SUPPORTED_FEATURES \
    (N148_FORMAT_6_SUPPORTED_FEATURES | N148_FEATURE_ADAPTIVE_ENTROPY | \
     N148_FEATURE_RICH_COEFFICIENT_CONTEXTS | \
     N148_FEATURE_MULTIPLE_INTRA_REFERENCES | \
     N148_FEATURE_SPECTRAL_LUMA_QUANT | \
     N148_FEATURE_REFINED_SPECTRAL_LUMA_QUANT | \
     N148_FEATURE_STRUCTURAL_LUMA_QUANT | \
     N148_FEATURE_BALANCED_RECONSTRUCTION | \
     N148_FEATURE_DETAIL_RECONSTRUCTION)

#define N148_ADAPTIVE_ENTROPY_PROFILE_FEATURES \
    (N148_PLANAR_PREDICTION_PROFILE_FEATURES | N148_FEATURE_ADAPTIVE_ENTROPY)
#define N148_RICH_CONTEXT_PROFILE_FEATURES \
    (N148_ADAPTIVE_ENTROPY_PROFILE_FEATURES | \
     N148_FEATURE_RICH_COEFFICIENT_CONTEXTS)
#define N148_MULTIPLE_REFERENCE_PROFILE_FEATURES \
    (N148_RICH_CONTEXT_PROFILE_FEATURES | N148_FEATURE_MULTIPLE_INTRA_REFERENCES)
#define N148_DIRECTIONAL_SCAN_PROFILE_FEATURES \
    (N148_MULTIPLE_REFERENCE_PROFILE_FEATURES | N148_FEATURE_DIRECTIONAL_SCAN)
#define N148_DIRECTIONAL_TRANSFORM_PROFILE_FEATURES \
    (N148_DIRECTIONAL_SCAN_PROFILE_FEATURES | N148_FEATURE_DIRECTIONAL_TRANSFORM)

int n148_format_7_features_valid(uint32_t features);

int n148_encode_format_7(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output);

int n148_decode_format_7(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr);

int n148_inspect_format_7(const uint8_t *metadata, size_t metadata_size,
                    const uint8_t *payload, size_t payload_size,
                    int width, int height, int chroma, uint32_t features,
                    N148SyntaxInspection *inspection);

#endif

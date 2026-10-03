/*
 * N.148i fidelity-profile codec
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include "fidelity_profile.h"

#include "predictive_codec.h"
#include "directional_intra.h"

static uint32_t luma_features(uint32_t features) {
    return features & N148_FORMAT_5_SUPPORTED_FEATURES;
}

static uint32_t predictive_features(uint32_t features) {
    return features & N148_FORMAT_3_SUPPORTED_FEATURES;
}

static uint32_t fidelity_tools(uint32_t features) {
    uint32_t tools = 0;
    if (features & N148_FEATURE_FILTERED_INTRA_REFERENCES)
        tools |= N148_FIDELITY_FILTER_REFERENCES;
    if (features & N148_FEATURE_PLANAR_INTRA_PREDICTION)
        tools |= N148_FIDELITY_PLANAR_PREDICTION;
    if (features & N148_FEATURE_STRUCTURAL_RDO)
        tools |= N148_FIDELITY_STRUCTURAL_RDO;
    if (features & N148_FEATURE_QUALITY_LAMBDA)
        tools |= N148_FIDELITY_QUALITY_LAMBDA;
    if (features & N148_FEATURE_LUMA_TRANSFORM_32X32)
        tools |= N148_FIDELITY_DCT32;
    if (features & N148_FEATURE_SECOND_ORDER_DC)
        tools |= N148_FIDELITY_SECOND_ORDER_DC;
    if (features & N148_FEATURE_FINE_INTRA_DIRECTIONS)
        tools |= N148_FIDELITY_FINE_DIRECTIONS;
    return tools;
}

int n148_format_6_features_valid(uint32_t features) {
    if (features & ~N148_FORMAT_6_SUPPORTED_FEATURES) return 0;
    if (!n148_format_5_features_valid(luma_features(features))) return 0;
    uint32_t fidelity = features & ~N148_FORMAT_5_SUPPORTED_FEATURES;
    if (fidelity &&
        (!(features & N148_FEATURE_INTRA) ||
         !(features & N148_FEATURE_CONTEXTUAL_RDO))) return 0;
    if ((features & N148_FEATURE_PLANAR_INTRA_PREDICTION) &&
        !(features & N148_FEATURE_DIRECTIONAL_INTRA)) return 0;
    if ((features & N148_FEATURE_LUMA_TRANSFORM_32X32) &&
        (!(features & N148_FEATURE_LUMA_TRANSFORM_16X16) ||
         !(features & N148_FEATURE_CALIBRATED_LUMA_QUANT))) return 0;
    return 1;
}

int n148_encode_format_6(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 || effort < 0 ||
        effort > 9 || !n148_format_6_features_valid(features)) return 0;
    int joint_chroma =
        (features & N148_FEATURE_JOINT_CHROMA_INTRA) != 0;
    int calibrated_chroma =
        (features & N148_FEATURE_CALIBRATED_CHROMA_QUANT) != 0;
    int segmented = (features & N148_FEATURE_SEGMENTATION) != 0;
    int adaptive_filter =
        (features & N148_FEATURE_ADAPTIVE_LOOP_FILTER) != 0;
    int calibrated_luma =
        (features & N148_FEATURE_CALIBRATED_LUMA_QUANT) != 0;
    int variable_luma =
        (features & N148_FEATURE_LUMA_TRANSFORM_16X16) != 0;
    int quant_adaptive_filter =
        (features & N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER) != 0;
    int dct32 =
        (features & N148_FEATURE_LUMA_TRANSFORM_32X32) != 0;
    if (!calibrated_chroma) chroma_quality = quality;
    if (!n148_fidelity_quality_set(quality) ||
        !n148_fidelity_tools_set(fidelity_tools(features))) return 0;
    int success = n148_predictive_encode(
        y, cb, cr, quality, chroma_quality, predictive_features(features), effort,
        joint_chroma, calibrated_chroma, segmented, adaptive_filter,
        calibrated_luma, variable_luma, 1, quant_adaptive_filter, dct32, 0,
        output);
    n148_fidelity_tools_set(0);
    n148_fidelity_quality_set(50);
    return success;
}

int n148_decode_format_6(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr) {
    if (!n148_format_6_features_valid(features) || chroma_quality < 1 ||
        chroma_quality > 100) return 0;
    int joint_chroma =
        (features & N148_FEATURE_JOINT_CHROMA_INTRA) != 0;
    int calibrated_chroma =
        (features & N148_FEATURE_CALIBRATED_CHROMA_QUANT) != 0;
    int segmented = (features & N148_FEATURE_SEGMENTATION) != 0;
    int adaptive_filter =
        (features & N148_FEATURE_ADAPTIVE_LOOP_FILTER) != 0;
    int calibrated_luma =
        (features & N148_FEATURE_CALIBRATED_LUMA_QUANT) != 0;
    int variable_luma =
        (features & N148_FEATURE_LUMA_TRANSFORM_16X16) != 0;
    int quant_adaptive_filter =
        (features & N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER) != 0;
    int dct32 =
        (features & N148_FEATURE_LUMA_TRANSFORM_32X32) != 0;
    if (!calibrated_chroma) chroma_quality = quality;
    if (!n148_fidelity_tools_set(fidelity_tools(features))) return 0;
    int success = n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, chroma_quality, chroma,
        predictive_features(features), joint_chroma, calibrated_chroma, segmented,
        adaptive_filter, calibrated_luma, variable_luma,
        quant_adaptive_filter, dct32, 0, y, cb, cr);
    n148_fidelity_tools_set(0);
    return success;
}

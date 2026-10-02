/*
 * N.148i luma-profile codec
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include "luma_profile.h"

#include "predictive_codec.h"

static uint32_t chroma_features(uint32_t features) {
    return features & N148_FORMAT_4_SUPPORTED_FEATURES;
}

static uint32_t predictive_features(uint32_t features) {
    return features & N148_FORMAT_3_SUPPORTED_FEATURES;
}

int n148_format_5_features_valid(uint32_t features) {
    if (features & ~N148_FORMAT_5_SUPPORTED_FEATURES) return 0;
    if (!n148_format_4_features_valid(chroma_features(features))) return 0;
    if ((features & N148_FEATURE_CALIBRATED_LUMA_QUANT) &&
        (!(features & N148_FEATURE_INTRA) ||
         !(features & N148_FEATURE_CONTEXTUAL_RDO))) return 0;
    if ((features & N148_FEATURE_LUMA_TRANSFORM_16X16) &&
        (!(features & N148_FEATURE_CONTEXTUAL_RDO) ||
         !(features & N148_FEATURE_INTRA_4X4))) return 0;
    if ((features & N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER) &&
        (!(features & N148_FEATURE_LOOP_FILTER) ||
         !(features & N148_FEATURE_CALIBRATED_LUMA_QUANT))) return 0;
    return 1;
}

int n148_encode_format_5(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 || effort < 0 ||
        effort > 9 || !n148_format_5_features_valid(features)) return 0;
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
    if (!calibrated_chroma) chroma_quality = quality;
    return n148_predictive_encode(
        y, cb, cr, quality, chroma_quality, predictive_features(features), effort,
        joint_chroma, calibrated_chroma, segmented, adaptive_filter,
        calibrated_luma, variable_luma, 1, quant_adaptive_filter, 0, 0,
        output);
}

int n148_decode_format_5(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr) {
    if (!n148_format_5_features_valid(features) || chroma_quality < 1 ||
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
    if (!calibrated_chroma) chroma_quality = quality;
    return n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, chroma_quality, chroma,
        predictive_features(features), joint_chroma, calibrated_chroma, segmented,
        adaptive_filter, calibrated_luma, variable_luma,
        quant_adaptive_filter, 0, 0, y, cb, cr);
}

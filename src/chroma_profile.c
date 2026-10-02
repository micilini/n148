/*
 * N.148i chroma-profile codec
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include "chroma_profile.h"

static uint32_t predictive_features(uint32_t features) {
    return features & N148_FORMAT_3_SUPPORTED_FEATURES;
}

int n148_format_4_features_valid(uint32_t features) {
    if (features & ~N148_FORMAT_4_SUPPORTED_FEATURES) return 0;
    if (!n148_format_3_features_valid(predictive_features(features))) return 0;
    if ((features & N148_FEATURE_JOINT_CHROMA_INTRA) &&
        !(features & N148_FEATURE_CONTEXTUAL_RDO)) return 0;
    if ((features & N148_FEATURE_SEGMENTATION) &&
        (!(features & N148_FEATURE_CALIBRATED_CHROMA_QUANT) ||
         !(features & N148_FEATURE_CONTEXTUAL_RDO))) return 0;
    if ((features & N148_FEATURE_ADAPTIVE_LOOP_FILTER) &&
        (!(features & N148_FEATURE_SEGMENTATION) ||
         !(features & N148_FEATURE_LOOP_FILTER))) return 0;
    return 1;
}

int n148_encode_format_4(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output) {
    if (!n148_format_4_features_valid(features) || chroma_quality < 1 ||
        chroma_quality > 100) return 0;
    int joint_chroma =
        (features & N148_FEATURE_JOINT_CHROMA_INTRA) != 0;
    if (features & N148_FEATURE_ADAPTIVE_LOOP_FILTER)
        return n148_adaptive_filter_encode(
            y, cb, cr, quality, chroma_quality, predictive_features(features),
            effort, joint_chroma, output);
    if (features & N148_FEATURE_SEGMENTATION)
        return n148_segmented_encode(
            y, cb, cr, quality, chroma_quality, predictive_features(features),
            effort, joint_chroma, output);
    if (features & N148_FEATURE_CALIBRATED_CHROMA_QUANT)
        return n148_calibrated_chroma_encode(
            y, cb, cr, quality, chroma_quality, predictive_features(features),
            effort, joint_chroma, output);
    if (features & N148_FEATURE_JOINT_CHROMA_INTRA)
        return n148_joint_chroma_encode(
            y, cb, cr, quality, predictive_features(features), effort, output);
    return n148_encode_format_3(y, cb, cr, quality, predictive_features(features), effort,
                          output);
}

int n148_decode_format_4(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr) {
    if (!n148_format_4_features_valid(features) || chroma_quality < 1 ||
        chroma_quality > 100) return 0;
    int joint_chroma =
        (features & N148_FEATURE_JOINT_CHROMA_INTRA) != 0;
    if (features & N148_FEATURE_ADAPTIVE_LOOP_FILTER)
        return n148_adaptive_filter_decode(
            metadata, metadata_size, payload, payload_size,
            width, height, quality, chroma_quality, chroma,
            predictive_features(features), joint_chroma, y, cb, cr);
    if (features & N148_FEATURE_SEGMENTATION)
        return n148_segmented_decode(
            metadata, metadata_size, payload, payload_size,
            width, height, quality, chroma_quality, chroma,
            predictive_features(features), joint_chroma, y, cb, cr);
    if (features & N148_FEATURE_CALIBRATED_CHROMA_QUANT)
        return n148_calibrated_chroma_decode(
            metadata, metadata_size, payload, payload_size,
            width, height, quality, chroma_quality, chroma,
            predictive_features(features), joint_chroma, y, cb, cr);
    if (features & N148_FEATURE_JOINT_CHROMA_INTRA)
        return n148_joint_chroma_decode(
            metadata, metadata_size, payload, payload_size,
            width, height, quality, chroma, predictive_features(features),
            y, cb, cr);
    return n148_decode_format_3(metadata, metadata_size, payload, payload_size,
                          width, height, quality, chroma,
                          predictive_features(features), y, cb, cr);
}

/*
 * N.148i predictive-codec interface
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_PREDICTIVE_CODEC_H
#define N148_PREDICTIVE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"
#include "entropy_payload.h"
#include "directional_intra.h"

#define N148_FEATURE_DIRECTIONAL_INTRA (1u << 8)
#define N148_FEATURE_INTRA_4X4 (1u << 9)
#define N148_FEATURE_CONTEXTUAL_RDO (1u << 10)
#define N148_FEATURE_PERCEPTUAL_TRELLIS (1u << 11)

#define N148_DIRECTIONAL_INTRA_PROFILE_FEATURES \
    (N148_FEATURE_RANS | N148_FEATURE_CONTEXT | \
     N148_FEATURE_INTRA | N148_FEATURE_RDO | \
     N148_FEATURE_LOOP_FILTER | \
     N148_FEATURE_DIRECTIONAL_INTRA | N148_FEATURE_INTRA_4X4)

#define N148_FORMAT_3_SUPPORTED_FEATURES \
    (N148_FEATURE_RANS | N148_FEATURE_CONTEXT | \
     N148_FEATURE_INTRA | N148_FEATURE_RDO | \
     N148_FEATURE_LOOP_FILTER | N148_FEATURE_DIRECTIONAL_INTRA | \
     N148_FEATURE_INTRA_4X4 | N148_FEATURE_CONTEXTUAL_RDO | \
     N148_FEATURE_PERCEPTUAL_TRELLIS)

/* Format-local entropy behavior selected by the format 7 wrapper. Older formats
   always pass zero and therefore retain byte-identical syntax. */
#define N148_ENTROPY_ADAPTIVE (1u << 0)
#define N148_ENTROPY_RICH_CONTEXT (1u << 1)
#define N148_ENTROPY_MULTIPLE_REFERENCES (1u << 2)
#define N148_ENTROPY_ADAPT_RATE_SHIFT 6u

typedef struct {
    uint64_t mode_histogram[10];
    uint64_t plane_mode_histogram[3][10];
    uint64_t luma_reference_histogram[3];
    uint64_t split_blocks;
    uint64_t plane_split_blocks[3];
    uint64_t total_blocks;
    uint64_t plane_total_blocks[3];
    uint64_t segment_histogram[4];
    uint64_t segment_count;
    uint8_t segment_quant_scale[4];
    uint64_t luma_transform_regions;
    uint64_t luma_transform_4x4;
    uint64_t luma_transform_8x8;
    uint64_t luma_transform_16x16;
    uint64_t luma_transform_32x32;
    uint64_t luma_transform_32x32_opportunities;
} N148SyntaxInspection;

int n148_format_3_features_valid(uint32_t features);

int n148_encode_format_3(Plane *y, Plane *cb, Plane *cr, int quality,
                   uint32_t features, int effort, N148EncodedPayload *output);

int n148_decode_format_3(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma,
                   uint32_t features,
                   Plane *y, Plane *cb, Plane *cr);

/* Joint-chroma payload helpers reuse the accepted format 3 coefficient syntax
   but jointly select and signal one chroma predictor for Cb/Cr. */
int n148_joint_chroma_encode(
    Plane *y, Plane *cb, Plane *cr, int quality,
    uint32_t features, int effort, N148EncodedPayload *output);
int n148_joint_chroma_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma, uint32_t features,
    Plane *y, Plane *cb, Plane *cr);

/* Calibrated chroma quantization keeps the format 3/4 entropy syntax and changes only the normative
   chroma table and independently signalled chroma quality. */
int n148_calibrated_chroma_encode(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma,
    N148EncodedPayload *output);
int n148_calibrated_chroma_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma,
    Plane *y, Plane *cb, Plane *cr);

/* Frame segmentation adds a four-segment quantizer map. */
int n148_segmented_encode(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma,
    N148EncodedPayload *output);
int n148_segmented_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma,
    Plane *y, Plane *cb, Plane *cr);

/* The adaptive chroma filter keeps the segmentation payload grammar and changes the normative
   causal loop-filter strength according to the decoded segment class. */
int n148_adaptive_filter_encode(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma,
    N148EncodedPayload *output);
int n148_adaptive_filter_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma,
    Plane *y, Plane *cb, Plane *cr);

/* One quantization result can feed both format-7 entropy representations.
   The caller owns this cache and releases it after the final encode attempt. */
typedef struct {
    N148CoeffPlane coefficients[3];
    N148PartitionMap partition_map;
    N148SegmentationMap segmentation_map;
    N148TransformMap transform_map;
    uint8_t *prediction_modes;
    size_t prediction_mode_count;
    int valid;
} N148PredictivePreparation;

void n148_predictive_preparation_release(N148PredictivePreparation *prepared);

int n148_predictive_encode_cached(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma, int calibrated_chroma,
    int segmented, int adaptive_filter, int calibrated_luma,
    int variable_luma, int perceptual_luma_trellis, int quant_adaptive_filter,
    int dct32, uint32_t entropy_tools, N148EncodedPayload *output,
    N148PredictivePreparation *prepared);

/* Shared entropy/prediction core. Format 5 adds a format-local luma table
   selector and, when requested, the revision-11 variable-luma grammar. */
int n148_predictive_encode(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma, int calibrated_chroma,
    int segmented, int adaptive_filter, int calibrated_luma,
    int variable_luma, int perceptual_luma_trellis, int quant_adaptive_filter,
    int dct32, uint32_t entropy_tools,
    N148EncodedPayload *output);
int n148_predictive_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma, int calibrated_chroma, int segmented,
    int adaptive_filter, int calibrated_luma, int variable_luma,
    int quant_adaptive_filter, int dct32, uint32_t entropy_tools,
    Plane *y, Plane *cb, Plane *cr);

/* Validation and benchmark hook for syntax usage, not part of the public
   installed API. The full entropy stream is validated while it is counted. */
int n148_directional_intra_inspect(const uint8_t *metadata, size_t metadata_size,
                    const uint8_t *payload, size_t payload_size,
                    int width, int height, int chroma, uint32_t features,
                    N148SyntaxInspection *inspection);
int n148_joint_chroma_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features,
    N148SyntaxInspection *inspection);
int n148_segmented_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    N148SyntaxInspection *inspection);
int n148_adaptive_filter_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    N148SyntaxInspection *inspection);
int n148_predictive_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    int segmented, int adaptive_filter, int variable_luma, int dct32,
    N148SyntaxInspection *inspection);
int n148_entropy_profile_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    int segmented, int adaptive_filter, int variable_luma, int dct32,
    uint32_t entropy_tools, N148SyntaxInspection *inspection);

#endif

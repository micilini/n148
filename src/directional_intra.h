/*
 * N.148i directional and sub-block intra prediction
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_DIRECTIONAL_INTRA_H
#define N148_DIRECTIONAL_INTRA_H

#include <stddef.h>
#include <stdint.h>

#include "encoder.h"
#include "ppm.h"
#include "segmentation.h"
#include "variable_transform.h"

enum {
    N148_DIRECTIONAL_INTRA_DC = 0,
    N148_DIRECTIONAL_INTRA_VERTICAL = 1,
    N148_DIRECTIONAL_INTRA_HORIZONTAL = 2,
    N148_DIRECTIONAL_INTRA_TRUE_MOTION = 3,
    N148_DIRECTIONAL_INTRA_DOWN_LEFT = 4,
    N148_DIRECTIONAL_INTRA_DOWN_RIGHT = 5,
    N148_DIRECTIONAL_INTRA_VERTICAL_RIGHT = 6,
    N148_DIRECTIONAL_INTRA_HORIZONTAL_DOWN = 7,
    N148_DIRECTIONAL_INTRA_VERTICAL_LEFT = 8,
    N148_DIRECTIONAL_INTRA_HORIZONTAL_UP = 9,
    N148_DIRECTIONAL_INTRA_MODE_COUNT = 10,
    N148_INTRA_REFERENCE_COUNT = 3,
    N148_PACKED_INTRA_MODE_COUNT =
        N148_DIRECTIONAL_INTRA_MODE_COUNT * N148_INTRA_REFERENCE_COUNT,
    N148_INTRA_MODE_SLOTS_PER_BLOCK = 4,
};

static inline int n148_intra_base_mode(int packed_mode) {
    return packed_mode % N148_DIRECTIONAL_INTRA_MODE_COUNT;
}

static inline int n148_intra_reference_index(int packed_mode) {
    return packed_mode / N148_DIRECTIONAL_INTRA_MODE_COUNT;
}

/* One causal split decision is stored per 8x8 coefficient cell. Geometry is
   retained so the entropy context resets at plane boundaries. */
typedef struct {
    uint8_t *split;
    size_t count;
    size_t plane_offsets[3];
    int blocks_x[3];
    int blocks_y[3];
} N148PartitionMap;

/* Internal calibration hook. The two transform slots are DCT4 and DCT8,
   respectively. Statistics describe the unquantized luma prediction
   residual selected by the final contextual RDO pass; coefficient positions
   are stored in each transform's natural row-major order. This is not part of
   the public library ABI. */
enum {
    N148_LUMA_STATS_DCT4 = 0,
    N148_LUMA_STATS_DCT8 = 1,
    N148_LUMA_STATS_TRANSFORM_COUNT = 2,
};

typedef struct {
    uint64_t block_count[N148_LUMA_STATS_TRANSFORM_COUNT]
                        [N148_DIRECTIONAL_INTRA_MODE_COUNT];
    double sum[N148_LUMA_STATS_TRANSFORM_COUNT]
              [N148_DIRECTIONAL_INTRA_MODE_COUNT][64];
    double sum_squares[N148_LUMA_STATS_TRANSFORM_COUNT]
                      [N148_DIRECTIONAL_INTRA_MODE_COUNT][64];
    double sum_absolute[N148_LUMA_STATS_TRANSFORM_COUNT]
                       [N148_DIRECTIONAL_INTRA_MODE_COUNT][64];
} N148LumaStatistics;

typedef enum {
    N148_TRELLIS_OFF = 0,
    N148_TRELLIS_LEGACY = 1,
    N148_TRELLIS_PERCEPTUAL_LUMA = 2
} N148TrellisPolicy;

/* Format-local encoder/decoder prediction semantics. Older formats leave
   this mask at zero, so their reconstruction stays byte-for-byte isolated. */
enum {
    N148_FIDELITY_FILTER_REFERENCES = 1u << 0,
    N148_FIDELITY_PLANAR_PREDICTION = 1u << 1,
    N148_FIDELITY_STRUCTURAL_RDO = 1u << 2,
    N148_FIDELITY_QUALITY_LAMBDA = 1u << 3,
    N148_FIDELITY_DCT32 = 1u << 4,
    N148_FIDELITY_SECOND_ORDER_DC = 1u << 5,
    N148_FIDELITY_FINE_DIRECTIONS = 1u << 6,
    N148_FIDELITY_MULTIPLE_REFERENCES = 1u << 7,
    /* Encoder-only search policy; never serialized into the format. */
    N148_FIDELITY_FAST_MODE_SEARCH = 1u << 8,
    N148_FIDELITY_SPECTRAL_LUMA_QUANT = 1u << 9,
    N148_FIDELITY_REFINED_SPECTRAL_LUMA_QUANT = 1u << 10,
    N148_FIDELITY_STRUCTURAL_LUMA_QUANT = 1u << 11,
    N148_FIDELITY_BALANCED_RECONSTRUCTION = 1u << 12,
    N148_FIDELITY_DETAIL_RECONSTRUCTION = 1u << 13,
    N148_FIDELITY_ALL = (1u << 14) - 1u,
};

int n148_fidelity_tools_set(uint32_t tools);
int n148_fidelity_quality_set(int quality);
int n148_fidelity_structural_quant(void);
int n148_fidelity_balanced_reconstruction(void);
int n148_fidelity_detail_reconstruction(void);
/* The public RGB call binds this encoder reference for its complete lifetime.
   Calls retain the existing externally serialized API contract. */
void n148_fidelity_rgb_reference(const unsigned char *pixels, size_t stride,
                                 int width, int height);

int n148_luma_statistics_begin(N148LumaStatistics *stats);
void n148_luma_statistics_end(N148LumaStatistics *stats);

/* Allocate the directional-intra partition map. The quantizer fills each entry with zero
   for one 8x8 transform or one for four 4x4 transforms. */
int n148_partition_map_allocate(int width, int height, int chroma,
                                   N148PartitionMap *map);
int n148_partition_map_validate(const N148PartitionMap *map);
uint8_t n148_partition_map_predict(const N148PartitionMap *map,
                                      size_t index);
void n148_partition_map_release(N148PartitionMap *map);

/* Quantize prediction residuals in Y, Cb, Cr order. Four mode slots are kept
   per 8x8 coefficient cell. DCT8 cells repeat their single mode in all slots;
   DCT4 cells store one causal mode per 4x4 sub-block. */
int n148_directional_intra_quantize_planes(
    const Plane *y, const Plane *cb, const Plane *cr, int quality,
    int chroma_quality, int calibrated_chroma, int calibrated_luma,
    int extended_modes,
    int split_4x4, int contextual_rdo, int effort, int trellis_policy,
    int loop_filter,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    N148TransformMap *transform_map,
    N148CoeffPlane coefficients[3], uint8_t **modes, size_t *mode_count);

/* Joint chroma intra uses one optimized decision for Cb and Cr.
   The returned internal mode layout still mirrors the selected chroma mode
   into both planes so the mature causal reconstruction path can be reused. */
int n148_joint_chroma_intra_quantize_planes(
    const Plane *y, const Plane *cb, const Plane *cr, int quality,
    int chroma_quality, int calibrated_chroma, int calibrated_luma,
    int extended_modes,
    int split_4x4, int contextual_rdo, int effort, int trellis_policy,
    int loop_filter,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    N148TransformMap *transform_map,
    N148CoeffPlane coefficients[3], uint8_t **modes, size_t *mode_count);

/* Reconstruct the three planes using the normative scalar feedback path. */
int n148_directional_intra_reconstruct_planes(
    const N148CoeffPlane coefficients[3], const uint8_t *modes,
    size_t mode_count, int width, int height, int quality,
    int chroma_quality, int calibrated_chroma, int calibrated_luma, int chroma,
    int extended_modes, int split_4x4, int loop_filter,
    const N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    const N148TransformMap *transform_map,
    Plane *y, Plane *cb, Plane *cr);

/* Return four mode slots for every 8x8 coefficient cell in all planes. */
int n148_directional_intra_expected_modes(int width, int height, int chroma,
                                 size_t *mode_count);

#endif

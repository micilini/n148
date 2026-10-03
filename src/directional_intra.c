/*
 * N.148i directional and sub-block intra prediction
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * The directional predictors and the partition search in this file are an
 * original, compact implementation. The encoder always predicts from pixels
 * it has already reconstructed, which gives the decoder the same information
 * and prevents drift.
 */

#include "directional_intra.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dct.h"
#include "cpu.h"
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#include "header.h"
#include "loop_filter.h"
#include "rdo.h"
#include "segmentation.h"
#include "tables.h"
#include "variable_transform.h"

typedef struct {
    float reciprocal[64];
    float multiplier[64];
    double unit_energy[64];
    double perceptual_energy[64];
    int dc_step;
} IntraQuant8;

typedef struct {
    int step[16];
    uint32_t reciprocal_q32[16];
    double unit_energy[16];
    double perceptual_energy[16];
    int dc_step;
} IntraQuant4;

typedef struct {
    int step[16 * 16];
    uint32_t reciprocal_q32[16 * 16];
    int dc_step;
} IntraQuant16;

typedef struct {
    int step[32 * 32];
    uint32_t reciprocal_q32[32 * 32];
    int dc_step;
} IntraQuant32;

typedef struct {
    short coefficients[64];
    uint8_t modes[N148_INTRA_MODE_SLOTS_PER_BLOCK];
    uint8_t pixels[64];
    uint64_t distortion;
    double proxy_bits;
} IntraBlockCandidate;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    uint8_t pixels[18 * 18];
} IntraMacroSnapshot;

typedef struct {
    short coefficients[4][64];
    uint8_t modes[4][N148_INTRA_MODE_SLOTS_PER_BLOCK];
    uint8_t splits[4];
    IntraMacroSnapshot reconstruction;
    uint64_t distortion;
    double proxy_bits;
    int ending_dc_category;
} IntraMacroCandidate;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    uint8_t pixels[34 * 34];
} IntraSuperSnapshot;

typedef struct {
    short coefficients[16][64];
    uint8_t modes[16][N148_INTRA_MODE_SLOTS_PER_BLOCK];
    uint8_t splits[16];
    uint8_t strategies[4];
    IntraSuperSnapshot reconstruction;
    uint64_t distortion;
    double proxy_bits;
    int ending_dc_category;
} IntraSuperCandidate;

#define BASE_TRELLIS_LAMBDA_SCALE 0.04
#define INTRA_RDO_LAMBDA_SCALE 0.85
#define CONTEXTUAL_TRELLIS_LAMBDA_SCALE 0.040
#define STRUCTURAL_TRELLIS_LAMBDA_SCALE (0.85 * BASE_TRELLIS_LAMBDA_SCALE)
#define PERCEPTUAL_LUMA_TRELLIS_LAMBDA_SCALE 0.040
#define PERCEPTUAL_LUMA_LOW_FREQUENCY_BOOST 2.0
#define PERCEPTUAL_LUMA_PROTECTED_FREQUENCY_SUM 2
#define DCT16_QUANT_SCALE_PERCENT 115
#define DCT32_QUANT_SCALE_PERCENT 115
#define INTRA_RATE_DC_BUCKETS 3
#define INTRA_RATE_BANDS 4
#define INTRA_RATE_ACTIVITY_BUCKETS 2
#define INTRA_RATE_DC_MODELS (2 * INTRA_RATE_DC_BUCKETS)
#define INTRA_RATE_AC_MODELS (2 * INTRA_RATE_BANDS * INTRA_RATE_ACTIVITY_BUCKETS)
#define INTRA_RATE_COEFFICIENT_MODELS \
    (INTRA_RATE_DC_MODELS + INTRA_RATE_AC_MODELS)
#define INTRA_RATE_MODE_CONTEXTS (N148_DIRECTIONAL_INTRA_MODE_COUNT + 2)
#define INTRA_RATE_MODE_MODELS (2 * INTRA_RATE_MODE_CONTEXTS)
#define INTRA_RATE_PARTITION_MODEL \
    (INTRA_RATE_COEFFICIENT_MODELS + INTRA_RATE_MODE_MODELS)
#define INTRA_RATE_TRANSFORM_MODEL (INTRA_RATE_PARTITION_MODEL + 1)
#define INTRA_RATE_MODEL_COUNT (INTRA_RATE_TRANSFORM_MODEL + 1)

typedef struct {
    double bits[INTRA_RATE_MODEL_COUNT][256];
} IntraRateModel;

/* Codec calls are externally serialized, so the diagnostic collector follows
   the same lifetime rule as the existing shared dispatch and scratch state.
   Keeping the sink internal avoids adding a thirteenth public ABI symbol. */
static N148LumaStatistics *luma_stats_sink;
static uint32_t fidelity_tools;
static int fidelity_quality = 50;
static double fidelity_luma_rdo_scale = INTRA_RDO_LAMBDA_SCALE;
static N148RgbSource fidelity_rgb_source;

int n148_fidelity_detail_reconstruction(void) {
    return (fidelity_tools & N148_FIDELITY_DETAIL_RECONSTRUCTION) != 0;
}

int n148_fidelity_balanced_reconstruction(void) {
    return (fidelity_tools & N148_FIDELITY_BALANCED_RECONSTRUCTION) != 0;
}

int n148_fidelity_structural_quant(void) {
    return (fidelity_tools & N148_FIDELITY_STRUCTURAL_LUMA_QUANT) != 0;
}

void n148_fidelity_rgb_reference(const unsigned char *pixels, size_t stride,
                                 int width, int height) {
    fidelity_rgb_source = (N148RgbSource){pixels, stride, width, height};
}


static const int (*luma_quant_base(int calibrated))[8] {
    if (!calibrated) return Q_LUMA_BASE;
    if (n148_fidelity_balanced_reconstruction())
        return N148_BALANCED_LUMA_QUANT_BASE;
    if (fidelity_tools & N148_FIDELITY_STRUCTURAL_LUMA_QUANT)
        return N148_STRUCTURAL_LUMA_QUANT_BASE;
    if (fidelity_tools & N148_FIDELITY_REFINED_SPECTRAL_LUMA_QUANT)
        return N148_REFINED_SPECTRAL_LUMA_QUANT_BASE;
    return (fidelity_tools & N148_FIDELITY_SPECTRAL_LUMA_QUANT) ?
        N148_SPECTRAL_LUMA_QUANT_BASE : FORMAT_5_LUMA_QUANT_BASE;
}

static void update_fidelity_luma_rdo_scale(void) {
    fidelity_luma_rdo_scale = INTRA_RDO_LAMBDA_SCALE;
    if (!(fidelity_tools & N148_FIDELITY_QUALITY_LAMBDA)) return;
    if (fidelity_quality <= 45) fidelity_luma_rdo_scale = 0.65;
    else if (fidelity_quality <= 60) fidelity_luma_rdo_scale = 0.70;
    else if (fidelity_quality <= 75) fidelity_luma_rdo_scale = 0.75;
    /* The structural profile balances final syntax decisions with its
       finer coefficient refinement; original profile scales stay intact. */
    if (fidelity_tools & N148_FIDELITY_STRUCTURAL_LUMA_QUANT)
        fidelity_luma_rdo_scale = sqrt(
            fidelity_luma_rdo_scale * STRUCTURAL_TRELLIS_LAMBDA_SCALE);
}

static int luma_mode_count(int extended_modes, int size, int effort) {
    int count = extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    if (extended_modes && size >= 8 && effort >= 5 &&
        (fidelity_tools & N148_FIDELITY_MULTIPLE_REFERENCES))
        count = N148_PACKED_INTRA_MODE_COUNT;
    return count;
}

static int luma_allowed_modes(int extended_modes) {
    if (extended_modes &&
        (fidelity_tools & N148_FIDELITY_MULTIPLE_REFERENCES))
        return N148_PACKED_INTRA_MODE_COUNT;
    return extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
}

int n148_fidelity_tools_set(uint32_t tools) {
    if (tools & ~N148_FIDELITY_ALL) return 0;
    fidelity_tools = tools;
    update_fidelity_luma_rdo_scale();
    return 1;
}

int n148_fidelity_quality_set(int quality) {
    if (quality < 1 || quality > 100) return 0;
    fidelity_quality = quality;
    update_fidelity_luma_rdo_scale();
    return 1;
}

static int fidelity_luma_transform_map_validate(
    const N148TransformMap *map) {
    return n148_luma_transform_map_validate_extended(
        map, (fidelity_tools & N148_FIDELITY_DCT32) != 0);
}

int n148_luma_statistics_begin(N148LumaStatistics *stats) {
    if (!stats || luma_stats_sink) return 0;
    memset(stats, 0, sizeof(*stats));
    luma_stats_sink = stats;
    return 1;
}

void n148_luma_statistics_end(N148LumaStatistics *stats) {
    if (luma_stats_sink == stats) luma_stats_sink = NULL;
}

static int checked_block_count(int width, int height, size_t *count) {
    if (width <= 0 || height <= 0 || !count) return 0;
    size_t blocks_x = (size_t)(width / 8 + (width % 8 != 0));
    size_t blocks_y = (size_t)(height / 8 + (height % 8 != 0));
    if (blocks_y != 0 && blocks_x > SIZE_MAX / blocks_y) return 0;
    *count = blocks_x * blocks_y;
    return *count != 0 && *count <= LONG_MAX;
}

static int plane_geometry(int width, int height, int *blocks_x,
                          int *blocks_y, size_t *count) {
    if (!blocks_x || !blocks_y ||
        !checked_block_count(width, height, count)) return 0;
    *blocks_x = width / 8 + (width % 8 != 0);
    *blocks_y = height / 8 + (height % 8 != 0);
    return 1;
}

int n148_partition_map_allocate(int width, int height, int chroma,
                                   N148PartitionMap *map) {
    if (!map || chroma < CHROMA_444 || chroma > CHROMA_420) return 0;
    memset(map, 0, sizeof(*map));
    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    int widths[3] = {width, chroma_width, chroma_width};
    int heights[3] = {height, chroma_height, chroma_height};
    size_t total = 0;
    for (int plane = 0; plane < 3; plane++) {
        size_t count;
        map->plane_offsets[plane] = total;
        if (!plane_geometry(widths[plane], heights[plane],
                            &map->blocks_x[plane], &map->blocks_y[plane],
                            &count) || count > SIZE_MAX - total) {
            memset(map, 0, sizeof(*map));
            return 0;
        }
        total += count;
    }
    map->split = (uint8_t *) calloc(total, 1);
    if (!map->split) {
        memset(map, 0, sizeof(*map));
        return 0;
    }
    map->count = total;
    return 1;
}

int n148_partition_map_validate(const N148PartitionMap *map) {
    if (!map || !map->split || map->count == 0 ||
        map->plane_offsets[0] != 0) return 0;
    size_t offset = 0;
    for (int plane = 0; plane < 3; plane++) {
        if (map->plane_offsets[plane] != offset ||
            map->blocks_x[plane] <= 0 || map->blocks_y[plane] <= 0 ||
            (size_t) map->blocks_x[plane] >
                SIZE_MAX / (size_t) map->blocks_y[plane]) return 0;
        size_t count = (size_t) map->blocks_x[plane] *
            (size_t) map->blocks_y[plane];
        if (count > map->count - offset) return 0;
        for (size_t index = 0; index < count; index++)
            if (map->split[offset + index] > 1) return 0;
        offset += count;
    }
    return offset == map->count;
}

static int partition_plane_for_index(const N148PartitionMap *map,
                                     size_t index) {
    if (!map || index >= map->count) return -1;
    if (index >= map->plane_offsets[2]) return 2;
    if (index >= map->plane_offsets[1]) return 1;
    return 0;
}

uint8_t n148_partition_map_predict(const N148PartitionMap *map,
                                      size_t index) {
    int plane = partition_plane_for_index(map, index);
    if (plane < 0) return 0;
    size_t local = index - map->plane_offsets[plane];
    int blocks_x = map->blocks_x[plane];
    if (local % (size_t) blocks_x != 0) return map->split[index - 1];
    if (local >= (size_t) blocks_x)
        return map->split[index - (size_t) blocks_x];
    return 0;
}

void n148_partition_map_release(N148PartitionMap *map) {
    if (!map) return;
    free(map->split);
    memset(map, 0, sizeof(*map));
}

int n148_directional_intra_expected_modes(int width, int height, int chroma,
                                 size_t *mode_count) {
    if (!mode_count || chroma < CHROMA_444 || chroma > CHROMA_420) return 0;
    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    size_t y_count, chroma_count;
    if (!checked_block_count(width, height, &y_count) ||
        !checked_block_count(chroma_width, chroma_height, &chroma_count) ||
        chroma_count > (SIZE_MAX - y_count) / 2) return 0;
    size_t blocks = y_count + chroma_count * 2;
    if (blocks > SIZE_MAX / N148_INTRA_MODE_SLOTS_PER_BLOCK) return 0;
    *mode_count = blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK;
    return 1;
}

static int allocate_coefficients(size_t count, N148CoeffPlane *output) {
    if (!output || count == 0 || count > LONG_MAX ||
        count > SIZE_MAX / (64 * sizeof(short)) ||
        count > SIZE_MAX / sizeof(*output->nonzero_masks)) return 0;
    size_t coefficient_bytes = count * 64 * sizeof(short);
    size_t mask_bytes = count * sizeof(*output->nonzero_masks);
    if (coefficient_bytes > SIZE_MAX - mask_bytes) return 0;
    output->coefficients = (short *) calloc(1, coefficient_bytes + mask_bytes);
    if (!output->coefficients) return 0;
    output->nonzero_masks = (unsigned long long *)
        ((uint8_t *) output->coefficients + coefficient_bytes);
    output->nonzero_counts = NULL;
    output->count = (long) count;
    return 1;
}

/* Private hot-loop access. Both quantize entry points and both
   reconstruction paths validate the entire immutable segmentation map before
   reaching this helper. Retain per-access geometry and level checks. */
static int segmentation_level_prevalidated(
    const N148SegmentationMap *map, int block_x, int block_y,
    int subsample_x, int subsample_y, uint8_t *level) {
    if (!map || !map->levels || !level || block_x < 0 || block_y < 0 ||
        (subsample_x != 0 && subsample_x != 1) ||
        (subsample_y != 0 && subsample_y != 1)) return 0;
    size_t region_x = ((size_t) block_x * 8u * (1u << subsample_x)) /
        N148_SEGMENT_REGION_SIZE;
    size_t region_y = ((size_t) block_y * 8u * (1u << subsample_y)) /
        N148_SEGMENT_REGION_SIZE;
    if (region_x >= (size_t)map->columns || region_y >= (size_t)map->rows)
        return 0;
    size_t index = region_y * (size_t)map->columns + region_x;
    if (index >= map->count || map->levels[index] >= N148_SEGMENT_COUNT)
        return 0;
    *level = map->levels[index];
    return 1;
}

static void build_quant8_from_table(const int table[8][8], IntraQuant8 *quant) {
    quant->dc_step = table[0][0];
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int index = row * 8 + column;
            double scale = aan_scale_factor(row, column);
            quant->reciprocal[index] =
                (float)(1.0 / (table[row][column] * scale));
            quant->multiplier[index] =
                (float)(table[row][column] * scale / 64.0);
        }
    }
    for (int position = 0; position < 64; position++) {
        float basis[64] = {0.0f};
        int natural = ZIGZAG[position];
        basis[natural] = quant->multiplier[natural];
        n148_idct_block_scalar(basis, basis);
        double energy = 0.0;
        double gradient_energy = 0.0;
        for (int sample = 0; sample < 64; sample++) {
            double response = basis[sample] - 128.0;
            energy += response * response;
            int row = sample / 8;
            int column = sample % 8;
            if (column > 0) {
                double previous = basis[sample - 1] - 128.0;
                double difference = response - previous;
                gradient_energy += difference * difference;
            }
            if (row > 0) {
                double previous = basis[sample - 8] - 128.0;
                double difference = response - previous;
                gradient_energy += difference * difference;
            }
        }
        quant->unit_energy[position] = energy;
        quant->perceptual_energy[position] =
            energy + 0.25 * gradient_energy;
    }
}

static void build_quant4_from_table(const int table[8][8], IntraQuant4 *quant,
                                    int apply_fidelity) {
    (void) apply_fidelity;
    quant->dc_step = table[0][0];
    for (int row = 0; row < 4; row++) {
        int source_row = (row * 7 + 1) / 3;
        for (int column = 0; column < 4; column++) {
            int source_column = (column * 7 + 1) / 3;
            int position = row * 4 + column;
            quant->step[position] = table[source_row][source_column];
            /* floor((2^32 - 1) / step) fits even for step == 1. The
               quantizer corrects its quotient by at most one. */
            quant->reciprocal_q32[position] =
                UINT32_MAX / (uint32_t) quant->step[position];
        }
    }
    for (int position = 0; position < 16; position++) {
        int32_t transformed[16] = {0};
        int32_t spatial[16];
        int natural = n148_variable_zigzag(4, position);
        transformed[natural] = quant->step[natural];
        quant->unit_energy[position] =
            n148_variable_dct4_energy(natural, quant->step[natural]);
        if (!n148_variable_inverse(transformed, 4, spatial)) {
            quant->perceptual_energy[position] =
                (double) quant->step[natural] * quant->step[natural];
            continue;
        }
        double energy = 0.0;
        double gradient_energy = 0.0;
        for (int sample = 0; sample < 16; sample++) {
            double response = spatial[sample];
            energy += response * response;
            int row = sample / 4;
            int column = sample % 4;
            if (column > 0) {
                double difference = response - spatial[sample - 1];
                gradient_energy += difference * difference;
            }
            if (row > 0) {
                double difference = response - spatial[sample - 4];
                gradient_energy += difference * difference;
            }
        }
        quant->perceptual_energy[position] =
            energy + 0.25 * gradient_energy;
    }
}

/* DCT16 has its own 16x16 step matrix. The accepted residual-luma table is
   sampled over normalized frequency coordinates instead of repeating 8x8
   cells, so equal relative frequencies retain equal quantizer strength. */
static void build_quant16_from_table(const int table[8][8],
                                     IntraQuant16 *quant) {
    for (int row = 0; row < 16; row++) {
        int source_row = (row * 7 + 7) / 15;
        for (int column = 0; column < 16; column++) {
            int source_column = (column * 7 + 7) / 15;
            int value = table[source_row][source_column];
            value = (value * (n148_fidelity_detail_reconstruction() ?
                100 : DCT16_QUANT_SCALE_PERCENT) + 50) / 100;
            if (value > 255) value = 255;
            int position = row * 16 + column;
            quant->step[position] = value;
            quant->reciprocal_q32[position] =
                UINT32_MAX / (uint32_t) value;
        }
    }
    quant->dc_step = quant->step[0];
}

static int build_segment_quantizers16(
    const int base[8][8], int quality,
    const N148SegmentationMap *segmentation_map,
    IntraQuant16 quant16[N148_SEGMENT_COUNT]) {
    int scaled[8][8];
    scale_table(base, quality, scaled);
    int levels = segmentation_map ? N148_SEGMENT_COUNT : 1;
    for (int level = 0; level < levels; level++) {
        int adjusted[8][8];
        if (segmentation_map) {
            if (!n148_segmentation_adjust_table(
                    (const int (*)[8]) scaled, segmentation_map,
                    (uint8_t) level, 0, adjusted)) return 0;
            build_quant16_from_table(
                (const int (*)[8]) adjusted, &quant16[level]);
        } else {
            build_quant16_from_table(
                (const int (*)[8]) scaled, &quant16[level]);
        }
    }
    return 1;
}

static void build_quant32_from_table(const int table[8][8],
                                     IntraQuant32 *quant) {
    for (int row = 0; row < 32; row++) {
        int source_row = (row * 7 + 15) / 31;
        for (int column = 0; column < 32; column++) {
            int source_column = (column * 7 + 15) / 31;
            int value = table[source_row][source_column];
            value = (value * DCT32_QUANT_SCALE_PERCENT + 50) / 100;
            if (value > 255) value = 255;
            int position = row * 32 + column;
            quant->step[position] = value;
            quant->reciprocal_q32[position] =
                UINT32_MAX / (uint32_t) value;
        }
    }
    quant->dc_step = quant->step[0];
}

static int build_segment_quantizers32(
    const int base[8][8], int quality,
    const N148SegmentationMap *segmentation_map,
    IntraQuant32 quant32[N148_SEGMENT_COUNT]) {
    int scaled[8][8];
    scale_table(base, quality, scaled);
    int levels = segmentation_map ? N148_SEGMENT_COUNT : 1;
    for (int level = 0; level < levels; level++) {
        int adjusted[8][8];
        if (segmentation_map) {
            if (!n148_segmentation_adjust_table(
                    (const int (*)[8]) scaled, segmentation_map,
                    (uint8_t) level, 0, adjusted)) return 0;
            build_quant32_from_table((const int (*)[8]) adjusted,
                                     &quant32[level]);
        } else {
            build_quant32_from_table((const int (*)[8]) scaled,
                                     &quant32[level]);
        }
    }
    return 1;
}

static int build_segment_quantizers(
    const int base[8][8], int quality, int plane_index,
    const N148SegmentationMap *segmentation_map,
    IntraQuant8 quant8[N148_SEGMENT_COUNT],
    IntraQuant4 quant4[N148_SEGMENT_COUNT]) {
    int scaled[8][8];
    scale_table(base, quality, scaled);
    int levels = segmentation_map && plane_index == 0 ?
        N148_SEGMENT_COUNT : 1;
    for (int level = 0; level < levels; level++) {
        int adjusted[8][8];
        if (segmentation_map) {
            if (!n148_segmentation_adjust_table(
                    (const int (*)[8]) scaled, segmentation_map,
                    (uint8_t) level,
                    plane_index, adjusted)) return 0;
            build_quant8_from_table(
                (const int (*)[8]) adjusted, &quant8[level]);
            build_quant4_from_table(
                (const int (*)[8]) adjusted, &quant4[level],
                plane_index == 0);
        } else {
            build_quant8_from_table(
                (const int (*)[8]) scaled, &quant8[level]);
            build_quant4_from_table(
                (const int (*)[8]) scaled, &quant4[level],
                plane_index == 0);
        }
    }
    return 1;
}

/* Reconstruction only reads the dequantization multipliers and 4x4 steps.
   The encoder's quantizer setup also builds reciprocal and perceptual-energy
   tables, including 64 inverse transforms per 8x8 quantizer. */
static int build_segment_decode_quantizers(
    const int base[8][8], int quality, int plane_index,
    const N148SegmentationMap *segmentation_map,
    IntraQuant8 quant8[N148_SEGMENT_COUNT],
    IntraQuant4 quant4[N148_SEGMENT_COUNT]) {
    int scaled[8][8];
    scale_table(base, quality, scaled);
    int levels = segmentation_map && plane_index == 0 ?
        N148_SEGMENT_COUNT : 1;
    for (int level = 0; level < levels; level++) {
        int adjusted[8][8];
        const int (*table)[8] = (const int (*)[8]) scaled;
        if (segmentation_map) {
            if (!n148_segmentation_adjust_table(
                    table, segmentation_map, (uint8_t) level,
                    plane_index, adjusted)) return 0;
            table = (const int (*)[8]) adjusted;
        }
        for (int row = 0; row < 8; row++) {
            for (int column = 0; column < 8; column++) {
                int index = row * 8 + column;
                double scale = aan_scale_factor(row, column);
                quant8[level].multiplier[index] =
                    (float)(table[row][column] * scale / 64.0);
            }
        }
        for (int row = 0; row < 4; row++) {
            int source_row = (row * 7 + 1) / 3;
            for (int column = 0; column < 4; column++) {
                int source_column = (column * 7 + 1) / 3;
                quant4[level].step[row * 4 + column] =
                    table[source_row][source_column];
            }
        }
    }
    return 1;
}

static int sample_clamped(const Plane *plane, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= plane->width) x = plane->width - 1;
    if (y >= plane->height) y = plane->height - 1;
    return plane->data[(long) y * plane->width + x];
}

static uint8_t clamp_byte(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t) value;
}

static int average2(int first, int second) {
    return (first + second + 1) >> 1;
}

static int average3(int first, int middle, int last) {
    return (first + 2 * middle + last + 2) >> 2;
}

static int extended_sample(const uint8_t *values, int count, int index) {
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return values[index];
}

/* Boundary coordinate zero is top-left. Positive coordinates walk right on
   the top edge, while negative coordinates walk down the left edge. */
static int diagonal_boundary(const uint8_t *top, const uint8_t *left,
                             int count, int top_left, int coordinate) {
    if (coordinate == 0) return top_left;
    if (coordinate > 0)
        return extended_sample(top, count, coordinate - 1);
    return extended_sample(left, count, -coordinate - 1);
}

static int diagonal_half_sample(const uint8_t *top, const uint8_t *left,
                                int count, int top_left,
                                int coordinate_twice) {
    int lower;
    if (coordinate_twice >= 0) lower = coordinate_twice / 2;
    else lower = -((-coordinate_twice + 1) / 2);
    int remainder = coordinate_twice - 2 * lower;
    int first = diagonal_boundary(top, left, count, top_left, lower);
    if (remainder == 0) return first;
    int second = diagonal_boundary(top, left, count, top_left, lower + 1);
    return average2(first, second);
}

static int edge_half_sample(const uint8_t *edge, int count,
                            int coordinate_twice) {
    if (coordinate_twice < 0) coordinate_twice = 0;
    int index = coordinate_twice / 2;
    int first = extended_sample(edge, count, index);
    if ((coordinate_twice & 1) == 0) return first;
    return average2(first, extended_sample(edge, count, index + 1));
}

/* The 4x4 directional family alternates two-tap half-sample interpolation
   with three-tap filtering at integer positions. Keeping this rule explicit
   avoids the blocky stair steps produced by plain nearest-neighbor rays. */
static int filtered_edge_half_sample(const uint8_t *edge, int count,
                                     int coordinate_twice) {
    if (coordinate_twice < 0) coordinate_twice = 0;
    int index = coordinate_twice / 2;
    if (coordinate_twice & 1)
        return average2(extended_sample(edge, count, index),
                        extended_sample(edge, count, index + 1));
    return average3(extended_sample(edge, count, index - 1),
                    extended_sample(edge, count, index),
                    extended_sample(edge, count, index + 1));
}

static int boundary4(const uint8_t top[34], const uint8_t left[34],
                     int top_left, int coordinate) {
    if (coordinate == 0) return top_left;
    return coordinate > 0 ? top[coordinate - 1] : left[-coordinate - 1];
}

/* These equations are an independent expression of the ten VP8-style 4x4
   predictors. They use only already reconstructed top and left samples. */
static void predict_square4(const uint8_t top[34], const uint8_t left[34],
                            int edge_count, int top_left, int dc, int mode,
                            uint32_t tools,
                            uint8_t prediction[16]) {
    if (mode == N148_DIRECTIONAL_INTRA_DC) {
        memset(prediction, dc, 16);
        return;
    }
    if (mode == N148_DIRECTIONAL_INTRA_VERTICAL) {
        uint8_t predicted_row[4];
        for (int column = 0; column < 4; column++)
            predicted_row[column] = (uint8_t) average3(
                boundary4(top, left, top_left, column),
                boundary4(top, left, top_left, column + 1),
                boundary4(top, left, top_left, column + 2));
        for (int row = 0; row < 4; row++)
            memcpy(prediction + row * 4, predicted_row, 4);
        return;
    }
    if (mode == N148_DIRECTIONAL_INTRA_HORIZONTAL) {
        for (int row = 0; row < 4; row++) {
            int value = average3(
                boundary4(top, left, top_left, -row),
                boundary4(top, left, top_left, -row - 1),
                boundary4(top, left, top_left, -row - 2));
            memset(prediction + row * 4, value, 4);
        }
        return;
    }
    if (mode == N148_DIRECTIONAL_INTRA_TRUE_MOTION) {
        for (int row = 0; row < 4; row++)
            for (int column = 0; column < 4; column++)
                prediction[row * 4 + column] = clamp_byte(
                    left[row] + top[column] - top_left);
        return;
    }
    for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
            int value = dc;
            switch (mode) {
                case N148_DIRECTIONAL_INTRA_DC:
                    break;
                case N148_DIRECTIONAL_INTRA_VERTICAL:
                    value = average3(
                        boundary4(top, left, top_left, column),
                        boundary4(top, left, top_left, column + 1),
                        boundary4(top, left, top_left, column + 2));
                    break;
                case N148_DIRECTIONAL_INTRA_HORIZONTAL:
                    value = average3(
                        boundary4(top, left, top_left, -row),
                        boundary4(top, left, top_left, -row - 1),
                        boundary4(top, left, top_left, -row - 2));
                    break;
                case N148_DIRECTIONAL_INTRA_TRUE_MOTION:
                    /* Keep the sharper true-motion predictor for 4x4
                       detail. The planar-prediction experiment is deliberately
                       limited to larger gradient regions. */
                    value = left[row] + top[column] - top_left;
                    break;
                case N148_DIRECTIONAL_INTRA_DOWN_LEFT: {
                    int index = column + row;
                    value = average3(
                        extended_sample(top, edge_count, index),
                        extended_sample(top, edge_count, index + 1),
                        extended_sample(top, edge_count, index + 2));
                    break;
                }
                case N148_DIRECTIONAL_INTRA_DOWN_RIGHT: {
                    int coordinate = column - row;
                    value = average3(
                        boundary4(top, left, top_left, coordinate + 1),
                        boundary4(top, left, top_left, coordinate),
                        boundary4(top, left, top_left, coordinate - 1));
                    break;
                }
                case N148_DIRECTIONAL_INTRA_VERTICAL_RIGHT:
                    if (row == 0) {
                        value = average2(
                            boundary4(top, left, top_left, column),
                            boundary4(top, left, top_left, column + 1));
                    } else if (row == 1) {
                        value = average3(
                            boundary4(top, left, top_left, column - 1),
                            boundary4(top, left, top_left, column),
                            boundary4(top, left, top_left, column + 1));
                    } else if (column > 0) {
                        value = prediction[(row - 2) * 4 + column - 1];
                    } else {
                        value = average3(
                            left[row - 1], left[row - 2],
                            row == 2 ? top_left : left[row - 3]);
                    }
                    break;
                case N148_DIRECTIONAL_INTRA_HORIZONTAL_DOWN:
                    if (column == 0) {
                        value = average2(
                            left[row], row == 0 ? top_left : left[row - 1]);
                    } else if (column == 1) {
                        value = average3(
                            left[row], row == 0 ? top_left : left[row - 1],
                            row == 0 ? top[0] :
                            (row == 1 ? top_left : left[row - 2]));
                    } else if (row > 0) {
                        value = prediction[(row - 1) * 4 + column - 2];
                    } else {
                        value = average3(top[column - 2], top[column - 1],
                                         top[column]);
                    }
                    break;
                case N148_DIRECTIONAL_INTRA_VERTICAL_LEFT:
                    value = filtered_edge_half_sample(
                        top, edge_count, 2 * column +
                        ((tools & N148_FIDELITY_FINE_DIRECTIONS) ?
                         (row + 1) / 2 : row) + 1);
                    break;
                case N148_DIRECTIONAL_INTRA_HORIZONTAL_UP:
                    value = filtered_edge_half_sample(
                        left, edge_count, 2 * row +
                        ((tools & N148_FIDELITY_FINE_DIRECTIONS) ?
                         (column + 1) / 2 : column) + 1);
                    break;
                default:
                    value = 128;
                    break;
            }
            prediction[row * 4 + column] = clamp_byte(value);
        }
    }
}

typedef struct {
    uint8_t top[66];
    uint8_t left[66];
    int edge_count;
    int top_left;
    int dc;
    uint32_t tools;
} N148PreparedSquare;

static void prepare_square_references(
    const Plane *reconstructed, int x0, int y0, int size, uint8_t neutral,
    int causal_right, uint32_t tools, int reference_index,
    N148PreparedSquare *references) {
    int reference_distance = reference_index + 1;
    int has_top = y0 >= reference_distance;
    int has_left = x0 >= reference_distance;
    uint8_t *top = references->top;
    uint8_t *left = references->left;
    int edge_count = size * 2 + 2;
    int top_limit = size >= 16 || y0 % 8 == 0 ?
        reconstructed->width - 1 : (x0 / 8) * 8 + 7;
    if (causal_right >= x0 && causal_right < top_limit)
        top_limit = causal_right;
    int left_limit = size >= 16 ? y0 + size - 1 :
        (x0 % 8 == 0 ? (y0 / 8) * 8 + 7 : y0 + size - 1);
    if (top_limit >= reconstructed->width) top_limit = reconstructed->width - 1;
    if (left_limit >= reconstructed->height)
        left_limit = reconstructed->height - 1;
    int top_sum = 0;
    int left_sum = 0;
    if (has_top && has_left && x0 >= reference_distance &&
        y0 >= reference_distance &&
        top_limit >= x0 && top_limit < reconstructed->width &&
        left_limit >= y0 && left_limit < reconstructed->height &&
        x0 <= INT_MAX - edge_count && y0 <= INT_MAX - edge_count) {
        const uint8_t *top_row = reconstructed->data +
            (long)(y0 - reference_distance) * reconstructed->width;
        int left_column = x0 - reference_distance;
        for (int index = 0; index < edge_count; index++) {
            int top_x = x0 + index;
            int left_y = y0 + index;
            if (top_x > top_limit) top_x = top_limit;
            if (left_y > left_limit) left_y = left_limit;
            top[index] = top_row[top_x];
            left[index] = reconstructed->data[
                (long) left_y * reconstructed->width + left_column];
            if (index < size) {
                top_sum += top[index];
                left_sum += left[index];
            }
        }
    } else {
        for (int index = 0; index < edge_count; index++) {
            top[index] = has_top ? (uint8_t) sample_clamped(
                reconstructed,
                x0 + index < top_limit ? x0 + index : top_limit,
                y0 - reference_distance) : neutral;
            left[index] = has_left ? (uint8_t) sample_clamped(
                reconstructed, x0 - reference_distance,
                y0 + index < left_limit ? y0 + index : left_limit) : neutral;
            if (index < size) {
                top_sum += top[index];
                left_sum += left[index];
            }
        }
    }
    if ((tools & N148_FIDELITY_FILTER_REFERENCES) && size >= 8) {
        uint8_t original_top[66], original_left[66];
        memcpy(original_top, top, (size_t) edge_count);
        memcpy(original_left, left, (size_t) edge_count);
        if (has_top) {
            for (int index = 0; index < edge_count; index++)
                top[index] = (uint8_t) average3(
                    extended_sample(original_top, edge_count, index - 1),
                    original_top[index],
                    extended_sample(original_top, edge_count, index + 1));
        }
        if (has_left) {
            for (int index = 0; index < edge_count; index++)
                left[index] = (uint8_t) average3(
                    extended_sample(original_left, edge_count, index - 1),
                    original_left[index],
                    extended_sample(original_left, edge_count, index + 1));
        }
        top_sum = 0;
        left_sum = 0;
        for (int index = 0; index < size; index++) {
            top_sum += top[index];
            left_sum += left[index];
        }
    }
    int top_left = has_top && has_left ?
        sample_clamped(reconstructed, x0 - reference_distance,
                       y0 - reference_distance) : neutral;
    int dc = neutral;
    if (has_top && has_left) dc = (top_sum + left_sum + size) / (2 * size);
    else if (has_top) dc = (top_sum + size / 2) / size;
    else if (has_left) dc = (left_sum + size / 2) / size;
    if ((tools & N148_FIDELITY_SECOND_ORDER_DC) && has_top && has_left) {
        int top_mean = (top_sum + size / 2) / size;
        int left_mean = (left_sum + size / 2) / size;
        int extrapolated = top_mean + left_mean - top_left;
        dc = clamp_byte((dc + extrapolated + 1) / 2);
    }
    references->edge_count = edge_count;
    references->top_left = top_left;
    references->dc = dc;
    references->tools = tools;
}

static void predict_square_prepared(const N148PreparedSquare *references,
                                    int size, int mode, uint8_t neutral,
                                    uint8_t *prediction) {
    const uint8_t *top = references->top;
    const uint8_t *left = references->left;
    int edge_count = references->edge_count;
    int top_left = references->top_left;
    int dc = references->dc;
    uint32_t tools = references->tools;

    if (size == 4) {
        predict_square4(top, left, edge_count, top_left, dc, mode, tools,
                        prediction);
        return;
    }

    if (mode == N148_DIRECTIONAL_INTRA_DC) {
        memset(prediction, dc, (size_t) size * size);
        return;
    }
    if (mode == N148_DIRECTIONAL_INTRA_VERTICAL) {
        for (int row = 0; row < size; row++)
            memcpy(prediction + row * size, top, (size_t) size);
        return;
    }
    if (mode == N148_DIRECTIONAL_INTRA_HORIZONTAL) {
        for (int row = 0; row < size; row++)
            memset(prediction + row * size, left[row], (size_t) size);
        return;
    }
    if (mode == N148_DIRECTIONAL_INTRA_TRUE_MOTION) {
        if (tools & N148_FIDELITY_PLANAR_PREDICTION) {
            int shift = size == 8 ? 4 : size == 16 ? 5 : 6;
            for (int row = 0; row < size; row++) {
                int base = (size - 1) * left[row] + top[size] +
                    (row + 1) * left[size] + size;
                int slope = top[size] - left[row];
                int top_weight = size - 1 - row;
                for (int column = 0; column < size; column++)
                    prediction[row * size + column] = (uint8_t)(
                        (base + column * slope +
                         top_weight * top[column]) >> shift);
            }
        } else {
            for (int row = 0; row < size; row++)
                for (int column = 0; column < size; column++)
                    prediction[row * size + column] = clamp_byte(
                        left[row] + top[column] - top_left);
        }
        return;
    }

    /* Each angular ray repeats across rows. Evaluate its interpolation
       once per coordinate, preserving the existing scalar rounding. */
    if (size == 8 && mode >= N148_DIRECTIONAL_INTRA_DOWN_LEFT &&
        mode <= N148_DIRECTIONAL_INTRA_HORIZONTAL_UP) {
        uint8_t ray[22];
        if (mode == N148_DIRECTIONAL_INTRA_DOWN_LEFT) {
            for (int d = 0; d < 15; d++) ray[d] = (uint8_t)average3(
                extended_sample(top, edge_count, d),
                extended_sample(top, edge_count, d+1),
                extended_sample(top, edge_count, d+2));
            for (int row = 0; row < 8; row++) memcpy(prediction + row*8, ray+row, 8);
        } else if (mode == N148_DIRECTIONAL_INTRA_DOWN_RIGHT) {
            for (int c = -7; c <= 7; c++) ray[c+7] = (uint8_t)average3(
                diagonal_boundary(top,left,edge_count,top_left,c-1),
                diagonal_boundary(top,left,edge_count,top_left,c),
                diagonal_boundary(top,left,edge_count,top_left,c+1));
            for (int row = 0; row < 8; row++) memcpy(prediction + row*8, ray+7-row, 8);
        } else if (mode == N148_DIRECTIONAL_INTRA_VERTICAL_RIGHT) {
            for (int c = -6; c <= 15; c++) ray[c+6] = (uint8_t)
                diagonal_half_sample(top,left,edge_count,top_left,c);
            for (int row = 0; row < 8; row++)
                for (int col = 0; col < 8; col++)
                    prediction[row*8+col] = ray[2*col-row+7];
        } else if (mode == N148_DIRECTIONAL_INTRA_HORIZONTAL_DOWN) {
            for (int c = -15; c <= 6; c++) ray[c+15] = (uint8_t)
                diagonal_half_sample(top,left,edge_count,top_left,c);
            for (int row = 0; row < 8; row++) memcpy(prediction + row*8, ray+14-2*row, 8);
        } else {
            const uint8_t *edge = mode == N148_DIRECTIONAL_INTRA_VERTICAL_LEFT ? top : left;
            for (int c = 0; c < 22; c++) ray[c] = (uint8_t)edge_half_sample(edge,edge_count,c+1);
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 8; col++) {
                    int major = mode == N148_DIRECTIONAL_INTRA_VERTICAL_LEFT ? col : row;
                    int minor = mode == N148_DIRECTIONAL_INTRA_VERTICAL_LEFT ? row : col;
                    if (tools & N148_FIDELITY_FINE_DIRECTIONS) minor = (minor+1)/2;
                    prediction[row*8+col] = ray[2*major+minor];
                }
            }
        }
        return;
    }


    for (int row = 0; row < size; row++) {
        for (int column = 0; column < size; column++) {
            int value;
            switch (mode) {
                case N148_DIRECTIONAL_INTRA_DC:
                    value = dc;
                    break;
                case N148_DIRECTIONAL_INTRA_VERTICAL:
                    value = top[column];
                    break;
                case N148_DIRECTIONAL_INTRA_HORIZONTAL:
                    value = left[row];
                    break;
                case N148_DIRECTIONAL_INTRA_TRUE_MOTION:
                    if ((tools & N148_FIDELITY_PLANAR_PREDICTION) &&
                        size >= 8) {
                        value = ((size - 1 - column) * left[row] +
                                 (column + 1) * top[size] +
                                 (size - 1 - row) * top[column] +
                                 (row + 1) * left[size] + size) /
                            (2 * size);
                    } else {
                        value = left[row] + top[column] - top_left;
                    }
                    break;
                case N148_DIRECTIONAL_INTRA_DOWN_LEFT: {
                    int index = column + row + 1;
                    value = average3(
                        extended_sample(top, edge_count, index - 1),
                        extended_sample(top, edge_count, index),
                        extended_sample(top, edge_count, index + 1));
                    break;
                }
                case N148_DIRECTIONAL_INTRA_DOWN_RIGHT: {
                    int coordinate = column - row;
                    value = average3(
                        diagonal_boundary(top, left, edge_count, top_left,
                                          coordinate - 1),
                        diagonal_boundary(top, left, edge_count, top_left,
                                          coordinate),
                        diagonal_boundary(top, left, edge_count, top_left,
                                          coordinate + 1));
                    break;
                }
                case N148_DIRECTIONAL_INTRA_VERTICAL_RIGHT:
                    value = diagonal_half_sample(
                        top, left, edge_count, top_left,
                        2 * column - row + 1);
                    break;
                case N148_DIRECTIONAL_INTRA_HORIZONTAL_DOWN:
                    value = diagonal_half_sample(
                        top, left, edge_count, top_left,
                        column - 2 * row - 1);
                    break;
                case N148_DIRECTIONAL_INTRA_VERTICAL_LEFT:
                    value = edge_half_sample(top, edge_count,
                        2 * column +
                        ((tools & N148_FIDELITY_FINE_DIRECTIONS) ?
                         (row + 1) / 2 : row) + 1);
                    break;
                case N148_DIRECTIONAL_INTRA_HORIZONTAL_UP:
                    value = edge_half_sample(left, edge_count,
                        2 * row +
                        ((tools & N148_FIDELITY_FINE_DIRECTIONS) ?
                         (column + 1) / 2 : column) + 1);
                    break;
                default:
                    value = neutral;
                    break;
            }
            prediction[row * size + column] = clamp_byte(value);
        }
    }
}

static void predict_square(const Plane *reconstructed, int x0, int y0,
                           int size, int mode, uint8_t neutral,
                           int causal_right, int apply_fidelity,
                           uint8_t *prediction) {
    uint32_t tools = apply_fidelity ? fidelity_tools : 0;
    int reference_index = 0;
    if ((tools & N148_FIDELITY_MULTIPLE_REFERENCES) && size >= 8) {
        reference_index = n148_intra_reference_index(mode);
        mode = n148_intra_base_mode(mode);
    }
    N148PreparedSquare references;
    prepare_square_references(reconstructed, x0, y0, size, neutral,
                              causal_right, tools, reference_index,
                              &references);
    predict_square_prepared(&references, size, mode, neutral, prediction);
}

static void hadamard(int *values, int size) {
    for (int span = 1; span < size; span *= 2) {
        for (int first = 0; first < size; first += span * 2) {
            for (int offset = 0; offset < span; offset++) {
                int a = values[first + offset];
                int b = values[first + offset + span];
                values[first + offset] = a + b;
                values[first + offset + span] = a - b;
            }
        }
    }
}

/* Exact sums of squared byte differences for full native transform blocks.
   A row is completed before applying the original predictor cutoff. */
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static uint64_t pixel_error(const uint8_t *a, int stride_a,
                               const uint8_t *b, int stride_b,
                               int size, uint64_t cutoff) {
    uint64_t total = 0;
    for (int row = 0; row < size; row++) {
        __m128i sum;
        if (size <= 8) {
            __m128i left, right;
            if (size == 4) {
                uint32_t x, y;
                memcpy(&x, a, 4); memcpy(&y, b, 4);
                left = _mm_cvtsi32_si128((int)x);
                right = _mm_cvtsi32_si128((int)y);
            } else {
                left = _mm_loadl_epi64((const __m128i *)a);
                right = _mm_loadl_epi64((const __m128i *)b);
            }
            __m128i delta = _mm_sub_epi16(_mm_cvtepu8_epi16(left),
                                        _mm_cvtepu8_epi16(right));
            sum = _mm_madd_epi16(delta, delta);
        } else {
            __m256i accumulated = _mm256_setzero_si256();
            for (int column = 0; column < size; column += 16) {
                __m128i left = _mm_loadu_si128((const __m128i *)(a+column));
                __m128i right = _mm_loadu_si128((const __m128i *)(b+column));
                __m256i delta = _mm256_sub_epi16(_mm256_cvtepu8_epi16(left),
                                                _mm256_cvtepu8_epi16(right));
                accumulated = _mm256_add_epi32(accumulated,
                                               _mm256_madd_epi16(delta, delta));
            }
            sum = _mm_add_epi32(_mm256_castsi256_si128(accumulated),
                                _mm256_extracti128_si256(accumulated, 1));
        }
        sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 8));
        sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 4));
        total += (unsigned)_mm_cvtsi128_si32(sum);
        if (total >= cutoff) break;
        a += stride_a;
        b += stride_b;
    }
    return total;
}
#endif


static uint64_t prediction_sse(const Plane *source, int x0, int y0,
                               int size, const uint8_t *prediction,
                               uint64_t cutoff) {
#if defined(__x86_64__) || defined(__i386__)
    if ((size == 4 || size == 8 || size == 16 || size == 32) &&
        x0 >= 0 && y0 >= 0 && size <= source->width-x0 &&
        size <= source->height-y0 && n148_cpu_level() >= N148_CPU_AVX2)
        return pixel_error(source->data+(long)y0*source->width+x0,
            source->width, prediction, size, size, cutoff);
#endif
    uint64_t error = 0;
    if (x0 >= 0 && y0 >= 0 && size <= source->width - x0 &&
        size <= source->height - y0) {
        for (int row = 0; row < size; row++) {
            const uint8_t *source_row = source->data +
                (long)(y0 + row) * source->width + x0;
            const uint8_t *prediction_row = prediction + row * size;
            for (int column = 0; column < size; column++) {
                int residual = source_row[column] - prediction_row[column];
                error += (uint64_t)(residual * residual);
            }
            if (error >= cutoff) break;
        }
    } else {
        for (int row = 0; row < size; row++) {
            for (int column = 0; column < size; column++) {
                int residual = sample_clamped(source, x0 + column,
                                              y0 + row) -
                               prediction[row * size + column];
                error += (uint64_t)(residual * residual);
            }
            if (error >= cutoff) break;
        }
    }
    return error;
}

static uint64_t prediction_satd(const Plane *source, int x0, int y0,
                                int size, const uint8_t *prediction,
                                uint64_t cutoff) {
    int transformed[32 * 32];
    if (x0 >= 0 && y0 >= 0 && size <= source->width - x0 &&
        size <= source->height - y0) {
        for (int row = 0; row < size; row++) {
            const uint8_t *source_row = source->data +
                (long)(y0 + row) * source->width + x0;
            for (int column = 0; column < size; column++)
                transformed[row * size + column] =
                    source_row[column] - prediction[row * size + column];
            hadamard(transformed + row * size, size);
        }
    } else {
        for (int row = 0; row < size; row++) {
            for (int column = 0; column < size; column++)
                transformed[row * size + column] =
                    sample_clamped(source, x0 + column, y0 + row) -
                    prediction[row * size + column];
            hadamard(transformed + row * size, size);
        }
    }
    uint64_t score = 0;
    for (int column = 0; column < size; column++) {
        int vertical[32];
        for (int row = 0; row < size; row++)
            vertical[row] = transformed[row * size + column];
        hadamard(vertical, size);
        for (int row = 0; row < size; row++) {
            int value = vertical[row];
            score += (uint64_t)(value < 0 ? -value : value);
        }
        if (score >= cutoff) return score;
    }
    return score;
}

static int choose_mode(const Plane *source, const Plane *reconstructed,
                       int x0, int y0, int size, uint8_t neutral,
                       int mode_count, int causal_right,
                       int apply_fidelity, uint8_t *prediction,
                       int *runner_up, uint64_t *prediction_score) {
    int fast_search = (fidelity_tools & N148_FIDELITY_FAST_MODE_SEARCH) != 0;
    if (fast_search && mode_count > 4) mode_count = 4;
    uint64_t best_score = UINT64_MAX;
    uint64_t second_score = UINT64_MAX;
    int best_mode = N148_DIRECTIONAL_INTRA_DC;
    int second_mode = -1;
    uint8_t candidate[32 * 32];
    uint32_t tools = apply_fidelity ? fidelity_tools : 0;
    int multiple_references =
        (tools & N148_FIDELITY_MULTIPLE_REFERENCES) && size >= 8;
    N148PreparedSquare references[N148_INTRA_REFERENCE_COUNT];
    uint8_t prepared[N148_INTRA_REFERENCE_COUNT] = {0};
    for (int mode = 0; mode < mode_count; mode++) {
        int reference_index = multiple_references ?
            n148_intra_reference_index(mode) : 0;
        int base_mode = multiple_references ?
            n148_intra_base_mode(mode) : mode;
        if (!prepared[reference_index]) {
            prepare_square_references(
                reconstructed, x0, y0, size, neutral, causal_right,
                tools, reference_index, &references[reference_index]);
            prepared[reference_index] = 1;
        }
        predict_square_prepared(&references[reference_index], size,
                                base_mode, neutral, candidate);
        uint64_t cutoff = runner_up ? second_score : best_score;
        uint64_t score = fast_search ?
            prediction_sse(source, x0, y0, size, candidate, cutoff) :
            prediction_satd(source, x0, y0, size, candidate, cutoff);
        if (score < best_score) {
            if (best_score != UINT64_MAX) {
                second_score = best_score;
                second_mode = best_mode;
            }
            best_score = score;
            best_mode = mode;
            memcpy(prediction, candidate, (size_t) size * (size_t) size);
        } else if (runner_up && score < second_score) {
            second_score = score;
            second_mode = mode;
        }
    }
    if (runner_up) *runner_up = second_mode;
    if (prediction_score) *prediction_score = best_score;
    return best_mode;
}

/* Every integer transform produces a signed 32-bit level and quantizer steps
   are in [1, 255]. The floor reciprocal can undershoot the exact quotient by
   at most one for the rounded numerator, so one remainder check restores the
   same integer result without a hardware divide in the block hot path. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline int quantize_integer_level(int32_t value, int step,
                                         uint32_t reciprocal_q32,
                                         short *result) {
    uint32_t magnitude = value < 0 ?
        (uint32_t)(-(int64_t) value) : (uint32_t) value;
    uint32_t rounded = magnitude + (uint32_t)(step / 2);
    uint32_t level = (uint32_t)(
        ((uint64_t) rounded * reciprocal_q32) >> 32);
    level += rounded - level * (uint32_t) step >= (uint32_t) step;
    if (level > (value < 0 ? 32768u : 32767u)) return 0;
    *result = (short)(value < 0 ? -(int) level : (int) level);
    return 1;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static void residual8_avx2(const Plane *source, int x0, int y0,
    const uint8_t prediction[64], float block[64]) {
    const __m256 centre = _mm256_set1_ps(128.0f);
    for (int row = 0; row < 8; row++) {
        const uint8_t *input = source->data + (long)(y0 + row) * source->width + x0;
        __m128i src = _mm_loadl_epi64((const __m128i *)input);
        __m128i pred = _mm_loadl_epi64((const __m128i *)(prediction + row * 8));
        __m256i residual = _mm256_sub_epi32(_mm256_cvtepu8_epi32(src),
                                           _mm256_cvtepu8_epi32(pred));
        _mm256_storeu_ps(block + row * 8,
            _mm256_add_ps(_mm256_cvtepi32_ps(residual),centre));
    }
}
__attribute__((target("avx2")))
static int quantize8_avx2(const float transformed[64],
    const IntraQuant8 *quant, short output[64], float ideal_levels[64]) {
    float ideal[64];
    int32_t levels[64];
    const __m256i upper = _mm256_set1_epi32(SHRT_MAX);
    const __m256i lower = _mm256_set1_epi32(SHRT_MIN);
    __m256i invalid = _mm256_setzero_si256();
    for (int i = 0; i < 64; i += 8) {
        __m256 value = _mm256_mul_ps(_mm256_loadu_ps(transformed + i),
                                     _mm256_loadu_ps(quant->reciprocal + i));
        __m256i rounded = _mm256_cvtps_epi32(value);
        invalid = _mm256_or_si256(invalid,_mm256_or_si256(
            _mm256_cmpgt_epi32(rounded,upper),_mm256_cmpgt_epi32(lower,rounded)));
        _mm256_storeu_ps(ideal + i,value);
        _mm256_storeu_si256((__m256i *)(levels + i),rounded);
    }
    if (_mm256_movemask_epi8(invalid)) return 0;
    for (int position = 0; position < 64; position++) {
        int natural = ZIGZAG[position];
        output[position] = (short)levels[natural];
        if (ideal_levels) ideal_levels[position] = ideal[natural];
    }
    return 1;
}
#endif

static int quantize8(const Plane *source, int x0, int y0,
                     const uint8_t prediction[64], const IntraQuant8 *quant,
                     short output[64], float ideal_levels[64]) {
    float block[64], transformed[64];
#if defined(__x86_64__) || defined(__i386__)
    int vector = n148_cpu_level() >= N148_CPU_AVX2;
    if (vector && x0 >= 0 && y0 >= 0 && x0 <= source->width - 8 &&
        y0 <= source->height - 8) {
        residual8_avx2(source,x0,y0,prediction,block);
    } else
#endif
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int index = row * 8 + column;
            block[index] = (float)(sample_clamped(source, x0 + column,
                                                  y0 + row) -
                                   prediction[index] + 128);
        }
    }
    dct_block_fast(block, transformed);
#if defined(__x86_64__) || defined(__i386__)
    if (vector) return quantize8_avx2(
        transformed,quant,output,ideal_levels);
#endif
    for (int position = 0; position < 64; position++) {
        int natural = ZIGZAG[position];
        float ideal = transformed[natural] * quant->reciprocal[natural];
        long value = lrintf(ideal);
        if (value < SHRT_MIN || value > SHRT_MAX) return 0;
        output[position] = (short) value;
        if (ideal_levels) ideal_levels[position] = ideal;
    }
    return 1;
}

static inline int coefficients_all_zero(const short *coefficients,
                                        int count) {
    for (int position = 0; position < count; position++)
        if (coefficients[position] != 0) return 0;
    return 1;
}

static inline int coefficients4_all_zero(const short coefficients[16]) {
    if (sizeof(short) != 2) return coefficients_all_zero(coefficients, 16);
    uint64_t words[4];
    memcpy(words, coefficients, sizeof(words));
    return (words[0] | words[1] | words[2] | words[3]) == 0;
}

static void copy_prediction_block(const uint8_t *prediction, int size,
                                  Plane *output, int x0, int y0) {
    int max_x = output->width - x0;
    int max_y = output->height - y0;
    if (max_x > size) max_x = size;
    if (max_y > size) max_y = size;
    if (max_x <= 0 || max_y <= 0) return;
    for (int row = 0; row < max_y; row++)
        memcpy(output->data + (long)(y0 + row) * output->width + x0,
               prediction + row * size, (size_t) max_x);
}

static int reconstruct8_masked(const short coefficients[64],
                               const uint8_t prediction[64],
                               const IntraQuant8 *quant, Plane *output,
                               int x0, int y0, unsigned long long mask) {
    static const uint8_t inverse_zigzag[64] = {
         0,  1,  5,  6, 14, 15, 27, 28,
         2,  4,  7, 13, 16, 26, 29, 42,
         3,  8, 12, 17, 25, 30, 41, 43,
         9, 11, 18, 24, 31, 40, 44, 53,
        10, 19, 23, 32, 39, 45, 52, 54,
        20, 22, 33, 38, 46, 51, 55, 60,
        21, 34, 37, 47, 50, 56, 59, 61,
        35, 36, 48, 49, 57, 58, 62, 63,
    };
    int first_ac = 1;
    if (mask != ~0ull) {
        first_ac = (mask & ~1ull) == 0 ? 64 : 1;
    } else {
        while (first_ac < 64 && coefficients[first_ac] == 0) first_ac++;
    }
    if (first_ac == 64) {
        if (coefficients[0] == 0) {
            copy_prediction_block(prediction, 8, output, x0, y0);
            return 1;
        }
        float dc = coefficients[0] * quant->multiplier[0];
        int width = output->width - x0;
        int height = output->height - y0;
        if (width > 8) width = 8;
        if (height > 8) height = 8;
#if defined(__x86_64__) || defined(__i386__)
        if (width == 8 && height == 8 &&
            n148_cpu_level() >= N148_CPU_AVX2) {
            idct_block_add_prediction_dc_avx2(
                dc, prediction,
                output->data + (long)y0 * output->width + x0,
                output->width);
            return 1;
        }
#endif
        for (int row = 0; row < height; row++) {
            uint8_t *destination = output->data +
                (long)(y0 + row) * output->width + x0;
            for (int column = 0; column < width; column++) {
                int index = row * 8 + column;
                float value = prediction[index] + (dc + 128.0f);
                value -= 128.0f;
                if (value < 0.0f) value = 0.0f;
                if (value > 255.0f) value = 255.0f;
                destination[column] = (uint8_t)(value + 0.5f);
            }
        }
        return 1;
    }
    /* Read in inverse zig-zag order so the 8x8 dequantization writes
       contiguous natural-order residuals. */
    float residual[64];
    for (int natural = 0; natural < 64; natural++) {
        residual[natural] = coefficients[inverse_zigzag[natural]] *
            quant->multiplier[natural];
    }
    int max_x = output->width - x0;
    int max_y = output->height - y0;
    if (max_x > 8) max_x = 8;
    if (max_y > 8) max_y = 8;
    if (max_x <= 0 || max_y <= 0) return 1;
#if defined(__x86_64__) || defined(__i386__)
    if (max_x == 8 && max_y == 8 &&
        n148_cpu_level() >= N148_CPU_AVX2) {
        idct_block_add_prediction_avx2(
            residual, prediction,
            output->data + (long) y0 * output->width + x0,
            output->width);
        return 1;
    }
#endif
    idct_block_fast(residual, residual);
    for (int row = 0; row < max_y; row++) {
        uint8_t *destination = output->data +
            (long)(y0 + row) * output->width + x0;
        for (int column = 0; column < max_x; column++) {
            int index = row * 8 + column;
            float value = prediction[index] + residual[index] - 128.0f;
            if (value < 0.0f) value = 0.0f;
            if (value > 255.0f) value = 255.0f;
            destination[column] = (uint8_t)(value + 0.5f);
        }
    }
    return 1;
}

static int reconstruct8(const short coefficients[64],
                        const uint8_t prediction[64],
                        const IntraQuant8 *quant, Plane *output,
                        int x0, int y0) {
    return reconstruct8_masked(
        coefficients, prediction, quant, output, x0, y0, ~0ull);
}

#include "quantize4_avx2.inc"
static int quantize4(const Plane *source, int x0, int y0,
                     const uint8_t prediction[16], const IntraQuant4 *quant,
                     short output[16], float ideal_levels[16]) {
    int32_t residual[16], transformed[16];
    int vector=0;
#if defined(__x86_64__) || defined(__i386__)
    vector=n148_cpu_level()>=N148_CPU_AVX2;
    if(vector && x0>=0 && y0>=0 && source->width-x0>=4 && source->height-y0>=4)
        residual4_avx2(source,x0,y0,prediction,residual);
    else
#endif
    for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
            residual[row * 4 + column] =
                sample_clamped(source, x0 + column, y0 + row) -
                prediction[row * 4 + column];
        }
    }
    if (!n148_variable_forward(residual, 4, transformed)) return 0;
#if defined(__x86_64__) || defined(__i386__)
    if(vector)return quantize4_avx2(transformed,quant,output,ideal_levels);
#endif
    for (int position = 0; position < 16; position++) {
        int natural = n148_variable_zigzag(4, position);
        if (natural < 0) return 0;
        int step = quant->step[natural];
        if (!quantize_integer_level(
                transformed[natural], step,
                quant->reciprocal_q32[natural], &output[position])) return 0;
        if (ideal_levels)
            ideal_levels[position] = (float) transformed[natural] /
                (float) step;
    }
    return 1;
}

static int reconstruct4_masked(const short coefficients[16],
                               const uint8_t prediction[16],
                               const IntraQuant4 *quant, Plane *output,
                               int x0, int y0, unsigned int mask) {
    if (mask == 0 ||
        (mask == UINT_MAX && coefficients4_all_zero(coefficients))) {
        copy_prediction_block(prediction, 4, output, x0, y0);
        return 1;
    }
    if (mask == 1) {
        /* The integer 4x4 inverse maps a DC-only coefficient to the same
           rounded quarter-level at every pixel (8192^2 / 2^28 = 1/4). */
        int32_t dc = (int32_t) coefficients[0] * quant->step[0];
        int value = dc >= 0 ? (int)(((int64_t) dc + 2) / 4) :
            -(int)((-(int64_t) dc + 2) / 4);
        int max_x = output->width - x0;
        int max_y = output->height - y0;
        if (max_x > 4) max_x = 4;
        if (max_y > 4) max_y = 4;
        for (int row = 0; row < max_y; row++) {
            uint8_t *destination = output->data +
                (long)(y0 + row) * output->width + x0;
            for (int column = 0; column < max_x; column++)
                destination[column] = clamp_byte(
                    prediction[row * 4 + column] + value);
        }
        return 1;
    }
    if (mask != UINT_MAX && (mask & ~15u) == 0) {
        /* Scan positions 0 through 3 are DC, horizontal AC1, vertical AC1
           and vertical AC2. Evaluate the same separable integer inverse
           with its identical final rounding while skipping zero products. */
        int32_t dc = (int32_t) coefficients[0] * quant->step[0];
        int32_t horizontal = (int32_t) coefficients[1] * quant->step[1];
        int32_t vertical = (int32_t) coefficients[2] * quant->step[4];
        int32_t vertical2 = (int32_t) coefficients[3] * quant->step[8];
        int64_t column[4] = {
            8192ll * dc + 10703ll * vertical + 8192ll * vertical2,
            8192ll * dc + 4433ll * vertical - 8192ll * vertical2,
            8192ll * dc - 4433ll * vertical - 8192ll * vertical2,
            8192ll * dc - 10703ll * vertical + 8192ll * vertical2,
        };
        int64_t row_delta[4] = {
            10703ll * 8192 * horizontal,
            4433ll * 8192 * horizontal,
            -4433ll * 8192 * horizontal,
            -10703ll * 8192 * horizontal,
        };
        int32_t residual[16];
        for (int row = 0; row < 4; row++) {
            for (int column_index = 0; column_index < 4; column_index++) {
                int64_t sum = 8192ll * column[row] +
                    row_delta[column_index];
                int64_t rounded = sum >= 0 ?
                    (sum + 134217728ll) / 268435456ll :
                    -((-sum + 134217728ll) / 268435456ll);
                if (rounded < INT32_MIN || rounded > INT32_MAX) return 0;
                residual[row * 4 + column_index] = (int32_t) rounded;
            }
        }
        int max_x = output->width - x0;
        int max_y = output->height - y0;
        if (max_x > 4) max_x = 4;
        if (max_y > 4) max_y = 4;
        for (int row = 0; row < max_y; row++) {
            uint8_t *destination = output->data +
                (long)(y0 + row) * output->width + x0;
            for (int column_index = 0; column_index < max_x;
                 column_index++)
                destination[column_index] = clamp_byte(
                    prediction[row * 4 + column_index] +
                    residual[row * 4 + column_index]);
        }
        return 1;
    }
    int32_t transformed[16], residual[16];
    for (int position = 0; position < 16; position++) {
        int natural = n148_variable_zigzag(4, position);
        if (natural < 0) return 0;
        transformed[natural] =
            (int32_t) coefficients[position] * quant->step[natural];
    }
    if (!n148_variable_inverse(transformed, 4, residual)) return 0;
    int max_x = output->width - x0;
    int max_y = output->height - y0;
    if (max_x > 4) max_x = 4;
    if (max_y > 4) max_y = 4;
    if (max_x <= 0 || max_y <= 0) return 1;
    for (int row = 0; row < max_y; row++) {
        uint8_t *destination = output->data +
            (long)(y0 + row) * output->width + x0;
        for (int column = 0; column < max_x; column++) {
            int value = prediction[row * 4 + column] +
                residual[row * 4 + column];
            destination[column] = clamp_byte(value);
        }
    }
    return 1;
}

static uint64_t coefficient_unit_mask(const short *values, int length);

static int reconstruct4(const short coefficients[16],
                        const uint8_t prediction[16],
                        const IntraQuant4 *quant, Plane *output,
                        int x0, int y0) {
    return reconstruct4_masked(
        coefficients, prediction, quant, output, x0, y0,
        (unsigned int)coefficient_unit_mask(coefficients,16));
}

static int quantize16(const Plane *source, int x0, int y0,
                      const uint8_t prediction[16 * 16],
                      const IntraQuant16 *quant,
                      short output[16 * 16]) {
    int32_t residual[16 * 16], transformed[16 * 16];
    for (int row = 0; row < 16; row++) {
        for (int column = 0; column < 16; column++) {
            residual[row * 16 + column] =
                sample_clamped(source, x0 + column, y0 + row) -
                prediction[row * 16 + column];
        }
    }
    if (!n148_variable_forward(residual, 16, transformed)) return 0;
    for (int position = 0; position < 16 * 16; position++) {
        int natural = n148_variable_zigzag(16, position);
        if (natural < 0 || !quantize_integer_level(
                transformed[natural], quant->step[natural],
                quant->reciprocal_q32[natural], &output[position])) return 0;
    }
    return 1;
}

static int reconstruct16(const short coefficients[16 * 16],
                         const uint8_t prediction[16 * 16],
                         const IntraQuant16 *quant, Plane *output,
                         int x0, int y0, int use_sparse,
                         const unsigned long long masks[4]) {
    if (masks ? !(masks[0] | masks[1] | masks[2] | masks[3]) :
                coefficients_all_zero(coefficients, 16 * 16)) {
        copy_prediction_block(prediction, 16, output, x0, y0);
        return 1;
    }
    int32_t transformed[16 * 16], residual[16 * 16];
    uint32_t active_rows = 0, active_columns = 0;
    int active_coefficients = 0;
    if (masks && use_sparse) {
#if defined(__GNUC__) || defined(__clang__)
        for (int cell = 0; cell < 4; cell++)
            active_coefficients += __builtin_popcountll(masks[cell]);
#else
        for (int cell = 0; cell < 4; cell++) {
            unsigned long long bits = masks[cell];
            while (bits) { active_coefficients++; bits &= bits - 1; }
        }
#endif
    }
    if (use_sparse && masks && active_coefficients <= 128) {
        memset(transformed, 0, sizeof(transformed));
        for (int cell = 0; cell < 4; cell++) {
            unsigned long long bits = masks[cell];
            while (bits) {
#if defined(__GNUC__) || defined(__clang__)
                int position = __builtin_ctzll(bits);
#else
                int position = 0;
                while (!((bits >> position) & 1ull)) position++;
#endif
                int natural = n148_variable_zigzag(16, cell * 64 + position);
                if (natural < 0) return 0;
                int32_t value = (int32_t) coefficients[cell * 64 + position] *
                    quant->step[natural];
                transformed[natural] = value;
                if (value != 0) {
                    active_rows |= 1u << (natural / 16);
                    active_columns |= 1u << (natural % 16);
                }
                bits &= bits - 1;
            }
        }
    } else if (use_sparse) {
        for (int position = 0; position < 16 * 16; position++) {
            int natural = n148_variable_zigzag(16, position);
            if (natural < 0) return 0;
            transformed[natural] =
                (int32_t) coefficients[position] * quant->step[natural];
            if (transformed[natural] != 0) {
                active_rows |= 1u << (natural / 16);
                active_columns |= 1u << (natural % 16);
            }
        }
    } else {
        for (int position = 0; position < 16 * 16; position++) {
            int natural = n148_variable_zigzag(16, position);
            if (natural < 0) return 0;
            transformed[natural] =
                (int32_t) coefficients[position] * quant->step[natural];
        }
    }
    int active_count = 0;
    if (use_sparse) {
#if defined(__GNUC__) || defined(__clang__)
        active_count = __builtin_popcount(active_rows) +
            __builtin_popcount(active_columns);
#else
        for (int index = 0; index < 16; index++)
            active_count += ((active_rows >> index) & 1u) +
                ((active_columns >> index) & 1u);
#endif
    }
    if (!(use_sparse && active_count <= 20 ? n148_variable_inverse_sparse16(
              transformed, active_rows, active_columns, residual) :
          n148_variable_inverse(transformed, 16, residual))) return 0;
    int max_x = output->width - x0;
    int max_y = output->height - y0;
    if (max_x > 16) max_x = 16;
    if (max_y > 16) max_y = 16;
    if (max_x <= 0 || max_y <= 0) return 1;
    for (int row = 0; row < max_y; row++) {
        uint8_t *destination = output->data +
            (long)(y0 + row) * output->width + x0;
        for (int column = 0; column < max_x; column++) {
            int value = prediction[row * 16 + column] +
                residual[row * 16 + column];
            destination[column] = clamp_byte(value);
        }
    }
    return 1;
}

static int quantize32(const Plane *source, int x0, int y0,
                      const uint8_t prediction[32 * 32],
                      const IntraQuant32 *quant,
                      short output[32 * 32]) {
    int32_t residual[32 * 32], transformed[32 * 32];
    for (int row = 0; row < 32; row++) {
        for (int column = 0; column < 32; column++) {
            residual[row * 32 + column] =
                sample_clamped(source, x0 + column, y0 + row) -
                prediction[row * 32 + column];
        }
    }
    if (!n148_variable_forward(residual, 32, transformed)) return 0;
    for (int position = 0; position < 32 * 32; position++) {
        int natural = n148_variable_zigzag(32, position);
        if (natural < 0 || !quantize_integer_level(
                transformed[natural], quant->step[natural],
                quant->reciprocal_q32[natural], &output[position])) return 0;
    }
    return 1;
}

static int reconstruct32(const short coefficients[32 * 32],
                         const uint8_t prediction[32 * 32],
                         const IntraQuant32 *quant, Plane *output,
                         int x0, int y0) {
    if (coefficients_all_zero(coefficients, 32 * 32)) {
        copy_prediction_block(prediction, 32, output, x0, y0);
        return 1;
    }
    int32_t transformed[32 * 32] = {0}, residual[32 * 32];
    for (int position = 0; position < 32 * 32; position++) {
        int natural = n148_variable_zigzag(32, position);
        if (natural < 0) return 0;
        transformed[natural] =
            (int32_t) coefficients[position] * quant->step[natural];
    }
    if (!n148_variable_inverse(transformed, 32, residual)) return 0;
    int max_x = output->width - x0;
    int max_y = output->height - y0;
    if (max_x > 32) max_x = 32;
    if (max_y > 32) max_y = 32;
    if (max_x <= 0 || max_y <= 0) return 1;
    for (int row = 0; row < max_y; row++) {
        uint8_t *destination = output->data +
            (long)(y0 + row) * output->width + x0;
        for (int column = 0; column < max_x; column++) {
            int value = prediction[row * 32 + column] +
                residual[row * 32 + column];
            destination[column] = clamp_byte(value);
        }
    }
    return 1;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static uint64_t coefficient_unit_mask_avx2(const short *values, int length) {
    const __m256i zero = _mm256_setzero_si256();
    __m256i a = _mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)values), zero);
    if (length == 16) {
        __m128i packed = _mm_packs_epi16(_mm256_castsi256_si128(a),
                                       _mm256_extracti128_si256(a, 1));
        return (unsigned)(~_mm_movemask_epi8(packed)) & 0xffffu;
    }
    __m256i b = _mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)(values + 16)), zero);
    __m256i c = _mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)(values + 32)), zero);
    __m256i d = _mm256_cmpeq_epi16(_mm256_loadu_si256((const __m256i *)(values + 48)), zero);
    __m256i first = _mm256_permute4x64_epi64(_mm256_packs_epi16(a, b), 0xd8);
    __m256i second = _mm256_permute4x64_epi64(_mm256_packs_epi16(c, d), 0xd8);
    uint32_t low = ~(uint32_t)_mm256_movemask_epi8(first);
    uint32_t high = ~(uint32_t)_mm256_movemask_epi8(second);
    return (uint64_t)low | ((uint64_t)high << 32);
}
#endif

static uint64_t coefficient_unit_mask(const short *values, int length) {
#if defined(__x86_64__) || defined(__i386__)
    if ((length == 16 || length == 64) && n148_cpu_level() >= N148_CPU_AVX2)
        return coefficient_unit_mask_avx2(values, length);
#endif
    uint64_t mask = 0;
    for (int i = 0; i < length; i++)
        if (values[i] != 0) mask |= 1ull << i;
    return mask;
}

static unsigned long long coefficient_mask(const short coefficients[64]) {
    return coefficient_unit_mask(coefficients, 64);
}

static int coefficient_category(short value) {
    unsigned int magnitude = value < 0 ?
        0u - (unsigned int)value : (unsigned int)value;
#if defined(__GNUC__) || defined(__clang__)
    return magnitude ? (int)(sizeof(magnitude) * CHAR_BIT) -
        __builtin_clz(magnitude) : 0;
#else
    int bits = 0;
    while (magnitude) { magnitude >>= 1; bits++; }
    return bits;
#endif
}

static int rate_dc_bucket(int previous_category) {
    if (previous_category == 0) return 0;
    return previous_category <= 3 ? 1 : 2;
}

static int rate_band(int position) {
    return (position >= 6) + (position >= 15) + (position >= 28);
}

static int rate_neighbor_active(const N148CoeffPlane *coefficients,
                                long block, int blocks_x, int position) {
    if (block % blocks_x != 0) {
        int value = coefficients->coefficients[(block - 1) * 64 + position];
        if (value <= -4 || value >= 4) return 1;
    }
    if (block >= blocks_x) {
        int value = coefficients->coefficients[
            (block - blocks_x) * 64 + position];
        if (value <= -4 || value >= 4) return 1;
    }
    return 0;
}

static int rate_dc_model(int plane_class, int previous_category) {
    return plane_class * INTRA_RATE_DC_BUCKETS +
        rate_dc_bucket(previous_category);
}

static int rate_ac_model_packed(const N148CoeffPlane *coefficients,
                                long block, int blocks_x, int plane_class,
                                int packed_position, int local_position,
                                int previous_category) {
    int active = previous_category >= 3 || rate_neighbor_active(
        coefficients, block, blocks_x, packed_position);
    return INTRA_RATE_DC_MODELS +
        ((plane_class * INTRA_RATE_BANDS + rate_band(local_position)) *
         INTRA_RATE_ACTIVITY_BUCKETS) + active;
}

static int rate_large_ac_model(int plane_class, int position,
                               int previous_category) {
    int active = previous_category >= 3;
    return INTRA_RATE_DC_MODELS +
        ((plane_class * INTRA_RATE_BANDS + rate_band(position)) *
         INTRA_RATE_ACTIVITY_BUCKETS) + active;
}

static long mode_slot_index(int block_x, int block_y, int part,
                            int blocks_x) {
    return ((long) block_y * blocks_x + block_x) *
        N148_INTRA_MODE_SLOTS_PER_BLOCK + part;
}

static int mode_at_virtual(const uint8_t *modes, int virtual_x,
                           int virtual_y, int blocks_x) {
    int block_x = virtual_x / 2;
    int block_y = virtual_y / 2;
    int part = (virtual_y & 1) * 2 + (virtual_x & 1);
    return modes[mode_slot_index(block_x, block_y, part, blocks_x)];
}

static int mode_reference_prediction(const uint8_t *modes, long block,
                                     int part, int blocks_x,
                                     int base_mode) {
    int block_x = (int)(block % blocks_x);
    int block_y = (int)(block / blocks_x);
    int virtual_x = block_x * 2 + (part & 1);
    int virtual_y = block_y * 2 + (part >> 1);
    int has_left = virtual_x > 0;
    int has_top = virtual_y > 0;
    int left = has_left ?
        mode_at_virtual(modes, virtual_x - 1, virtual_y, blocks_x) : 0;
    int top = has_top ?
        mode_at_virtual(modes, virtual_x, virtual_y - 1, blocks_x) : 0;
    int left_matches = has_left &&
        n148_intra_base_mode(left) == base_mode;
    int top_matches = has_top &&
        n148_intra_base_mode(top) == base_mode;
    if (left_matches && top_matches) {
        int left_reference = n148_intra_reference_index(left);
        int top_reference = n148_intra_reference_index(top);
        return left_reference == top_reference ? left_reference : 0;
    }
    if (left_matches) return n148_intra_reference_index(left);
    if (top_matches) return n148_intra_reference_index(top);
    return 0;
}

static int mode_signal_symbol(const uint8_t *modes, long block, int part,
                              int blocks_x, int mode,
                              int allowed_modes) {
    if (allowed_modes <= N148_DIRECTIONAL_INTRA_MODE_COUNT) return mode;
    int base_mode = n148_intra_base_mode(mode);
    int reference = n148_intra_reference_index(mode);
    int prediction = mode_reference_prediction(
        modes, block, part, blocks_x, base_mode);
    int residual = (reference + N148_INTRA_REFERENCE_COUNT - prediction) %
        N148_INTRA_REFERENCE_COUNT;
    return base_mode + N148_DIRECTIONAL_INTRA_MODE_COUNT * residual;
}

static int rate_mode_context(const uint8_t *modes, long block, int part,
                             int blocks_x) {
    int block_x = (int)(block % blocks_x);
    int block_y = (int)(block / blocks_x);
    int virtual_x = block_x * 2 + (part & 1);
    int virtual_y = block_y * 2 + (part >> 1);
    int has_left = virtual_x > 0;
    int has_top = virtual_y > 0;
    if (!has_left && !has_top) return 0;
    int left = has_left ? n148_intra_base_mode(
        mode_at_virtual(modes, virtual_x - 1, virtual_y, blocks_x)) : -1;
    int top = has_top ? n148_intra_base_mode(
        mode_at_virtual(modes, virtual_x, virtual_y - 1, blocks_x)) : -1;
    if (has_left && has_top && left != top)
        return N148_DIRECTIONAL_INTRA_MODE_COUNT + 1;
    return 1 + (has_left ? left : top);
}

static int rate_mode_model(int plane_class, const uint8_t *modes,
                           long block, int part, int blocks_x) {
    return INTRA_RATE_COEFFICIENT_MODELS +
        plane_class * INTRA_RATE_MODE_CONTEXTS +
        rate_mode_context(modes, block, part, blocks_x);
}

static int add_rate_count(uint32_t counts[INTRA_RATE_MODEL_COUNT][256],
                          int model, int symbol) {
    if (model < 0 || model >= INTRA_RATE_MODEL_COUNT ||
        symbol < 0 || symbol > 255 || counts[model][symbol] == UINT32_MAX)
        return 0;
    counts[model][symbol]++;
    return 1;
}

static int collect_coefficient_unit_counts(
    const N148CoeffPlane *coefficients, long block, int blocks_x,
    int plane_class, int packed_offset, int length,
    int *previous_dc_category,
    uint32_t counts[INTRA_RATE_MODEL_COUNT][256]) {
    const short *values = coefficients->coefficients + block * 64 +
        packed_offset;
    int category = coefficient_category(values[0]);
    if (!add_rate_count(counts,
                        rate_dc_model(plane_class, *previous_dc_category),
                        category)) return 0;
    *previous_dc_category = category;
    int next_position = 1;
    int previous_ac_category = 0;
    uint64_t active = coefficients->nonzero_masks[block] >> packed_offset;
    if (length < 64) active &= (1ull << length) - 1ull;
    active &= ~1ull;
    while (active) {
#if defined(__GNUC__) || defined(__clang__)
        int position = __builtin_ctzll(active);
#else
        int position = 0;
        uint64_t probe = active;
        while (!(probe & 1u)) { position++; probe >>= 1; }
#endif
        active &= active - 1;
        int run = position - next_position;
        while (run > 15) {
            int model = rate_ac_model_packed(
                coefficients, block, blocks_x, plane_class,
                packed_offset + next_position, next_position,
                previous_ac_category);
            if (!add_rate_count(counts, model, 0xf0)) return 0;
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        category = coefficient_category(values[position]);
        int model = rate_ac_model_packed(
            coefficients, block, blocks_x, plane_class,
            packed_offset + next_position, next_position,
            previous_ac_category);
        if (category < 1 || category > 15 ||
            !add_rate_count(counts, model, (run << 4) | category)) return 0;
        next_position = position + 1;
        previous_ac_category = category;
    }
    if (next_position < length) {
        int model = rate_ac_model_packed(
            coefficients, block, blocks_x, plane_class,
            packed_offset + next_position, next_position,
            previous_ac_category);
        if (!add_rate_count(counts, model, 0)) return 0;
    }
    return 1;
}

static int collect_coefficient_counts(
    const N148CoeffPlane *coefficients, const uint8_t *splits,
    int blocks_x, int plane_class,
    uint32_t counts[INTRA_RATE_MODEL_COUNT][256]) {
    int previous_dc_category = 0;
    for (long block = 0; block < coefficients->count; block++) {
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int length = splits[block] ? 16 : 64;
        for (int part = 0; part < parts; part++) {
            if (!collect_coefficient_unit_counts(
                    coefficients, block, blocks_x, plane_class, part * 16,
                    length, &previous_dc_category, counts)) return 0;
        }
    }
    return 1;
}

static int collect_large_coefficient_counts(
    const short *values, int length, int plane_class,
    int *previous_dc_category,
    uint32_t counts[INTRA_RATE_MODEL_COUNT][256]) {
    if (!values || (length != 16 * 16 && length != 32 * 32)) return 0;
    int category = coefficient_category(values[0]);
    if (!add_rate_count(
            counts, rate_dc_model(plane_class, *previous_dc_category),
            category)) return 0;
    *previous_dc_category = category;
    int next_position = 1;
    int previous_ac_category = 0;
    for (int position = 1; position < length; position++) {
        if (values[position] == 0) continue;
        int run = position - next_position;
        while (run > 15) {
            int model = rate_large_ac_model(
                plane_class, next_position, previous_ac_category);
            if (!add_rate_count(counts, model, 0xf0)) return 0;
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        category = coefficient_category(values[position]);
        int model = rate_large_ac_model(
            plane_class, next_position, previous_ac_category);
        if (category < 1 || category > 15 ||
            !add_rate_count(counts, model, (run << 4) | category)) return 0;
        next_position = position + 1;
        previous_ac_category = category;
    }
    if (next_position < length) {
        int model = rate_large_ac_model(
            plane_class, next_position, previous_ac_category);
        if (!add_rate_count(counts, model, 0)) return 0;
    }
    return 1;
}

static int collect_variable_luma_coefficient_counts(
    const N148CoeffPlane *coefficients, const uint8_t *splits,
    const N148TransformMap *transform_map, int blocks_x, int blocks_y,
    uint32_t counts[INTRA_RATE_MODEL_COUNT][256]) {
    if (!coefficients || !splits || !transform_map || blocks_x <= 0 ||
        blocks_y <= 0 ||
        coefficients->count != (long) blocks_x * blocks_y ||
        !fidelity_luma_transform_map_validate(transform_map)) return 0;
    int previous_dc_category = 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                short sequence[32 * 32];
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                if (!collect_large_coefficient_counts(
                        sequence, 32 * 32, 0, &previous_dc_category,
                        counts)) return 0;
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                short sequence[16 * 16];
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_x + (cell & 1);
                    int block_y = first_y + (cell >> 1);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                if (!collect_large_coefficient_counts(
                        sequence, 16 * 16, 0, &previous_dc_category,
                        counts)) return 0;
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    int length = splits[block] ? 16 : 64;
                    for (int part = 0; part < parts; part++)
                        if (!collect_coefficient_unit_counts(
                                coefficients, block, blocks_x, 0,
                                part * 16, length, &previous_dc_category,
                                counts)) return 0;
                }
            }
        }
    }
    return 1;
}

static int collect_mode_counts(
    const uint8_t *modes, long block_count, int blocks_x, int plane_class,
    const uint8_t *splits, int allowed_modes,
    uint32_t counts[INTRA_RATE_MODEL_COUNT][256]) {
    for (long block = 0; block < block_count; block++) {
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int mode_limit = allowed_modes;
        for (int part = 0; part < parts; part++) {
            int mode = modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
            int model = rate_mode_model(
                plane_class, modes, block, part, blocks_x);
            int symbol = mode_signal_symbol(
                modes, block, part, blocks_x, mode, allowed_modes);
            if (mode < 0 || mode >= mode_limit ||
                (splits[block] &&
                 n148_intra_reference_index(mode) != 0) || symbol < 0 ||
                symbol >= allowed_modes ||
                !add_rate_count(counts, model, symbol)) return 0;
        }
    }
    return 1;
}

static int collect_variable_luma_mode_counts(
    const uint8_t *modes, long block_count, int blocks_x, int blocks_y,
    const uint8_t *splits, int allowed_modes,
    const N148TransformMap *transform_map,
    uint32_t counts[INTRA_RATE_MODEL_COUNT][256]) {
    if (!modes || !splits || !transform_map || blocks_x <= 0 ||
        blocks_y <= 0 || block_count != (long) blocks_x * blocks_y ||
        !fidelity_luma_transform_map_validate(transform_map)) return 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                long first_block = (long) first_y * blocks_x + first_x;
                int mode = modes[first_block * N148_INTRA_MODE_SLOTS_PER_BLOCK];
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    for (int part = 0;
                         part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                        if (modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK +
                                  part] != mode) return 0;
                }
                int model = rate_mode_model(
                    0, modes, first_block, 0, blocks_x);
                int symbol = mode_signal_symbol(
                    modes, first_block, 0, blocks_x, mode, allowed_modes);
                if (mode < 0 || mode >= allowed_modes || symbol < 0 ||
                    symbol >= allowed_modes ||
                    !add_rate_count(counts, model, symbol)) return 0;
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                long block = (long) first_y * blocks_x + first_x;
                int mode = modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK];
                int model = rate_mode_model(0, modes, block, 0, blocks_x);
                int symbol = mode_signal_symbol(
                    modes, block, 0, blocks_x, mode, allowed_modes);
                if (mode < 0 || mode >= allowed_modes || symbol < 0 ||
                    symbol >= allowed_modes ||
                    !add_rate_count(counts, model, symbol)) return 0;
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    for (int part = 0; part < parts; part++) {
                        int mode = modes[block *
                            N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
                        int model = rate_mode_model(
                            0, modes, block, part, blocks_x);
                        int symbol = mode_signal_symbol(
                            modes, block, part, blocks_x, mode,
                            allowed_modes);
                        if (mode < 0 || mode >= allowed_modes ||
                            (splits[block] &&
                             n148_intra_reference_index(mode) != 0) ||
                            symbol < 0 || symbol >= allowed_modes ||
                            !add_rate_count(counts, model, symbol)) return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static int build_rate_model(
    const N148CoeffPlane coefficients[3], const uint8_t *modes,
    const N148PartitionMap *partition_map, int allowed_modes,
    int joint_chroma, const N148TransformMap *transform_map,
    IntraRateModel *rate_model) {
    if (!coefficients || !modes || !partition_map || !rate_model ||
        allowed_modes < 1 ||
        allowed_modes > N148_PACKED_INTRA_MODE_COUNT ||
        (joint_chroma != 0 && joint_chroma != 1) ||
        !n148_partition_map_validate(partition_map) ||
        (transform_map &&
         !fidelity_luma_transform_map_validate(transform_map))) return 0;
    uint32_t counts[INTRA_RATE_MODEL_COUNT][256] = {{0}};
    size_t mode_offset = 0;
    for (int plane = 0; plane < 3; plane++) {
        int blocks_x = partition_map->blocks_x[plane];
        int blocks_y = partition_map->blocks_y[plane];
        long block_count = coefficients[plane].count;
        const uint8_t *plane_splits = partition_map->split +
            partition_map->plane_offsets[plane];
        int plane_allowed_modes = plane == 0 ? allowed_modes :
            (allowed_modes < N148_DIRECTIONAL_INTRA_MODE_COUNT ? allowed_modes :
                                                        N148_DIRECTIONAL_INTRA_MODE_COUNT);
        if (plane == 0 && transform_map) {
            if (!collect_variable_luma_coefficient_counts(
                    &coefficients[plane], plane_splits, transform_map,
                    blocks_x, blocks_y, counts) ||
                !collect_variable_luma_mode_counts(
                    modes + mode_offset, block_count, blocks_x, blocks_y,
                    plane_splits, plane_allowed_modes, transform_map,
                    counts))
                return 0;
        } else {
            if (!collect_coefficient_counts(
                    &coefficients[plane], plane_splits, blocks_x,
                    plane == 0 ? 0 : 1, counts)) return 0;
            if (!(joint_chroma && plane == 2) && !collect_mode_counts(
                    modes + mode_offset, block_count, blocks_x,
                    plane == 0 ? 0 : 1, plane_splits,
                    plane_allowed_modes, counts)) return 0;
        }
        mode_offset += (size_t) block_count *
            N148_INTRA_MODE_SLOTS_PER_BLOCK;
    }
    for (size_t index = 0; index < partition_map->count; index++) {
        if (transform_map && index < partition_map->plane_offsets[1]) {
            size_t local = index;
            int block_x = (int)(local %
                (size_t) partition_map->blocks_x[0]);
            int block_y = (int)(local /
                (size_t) partition_map->blocks_x[0]);
            size_t transform_index = (size_t)(block_y / 2) *
                (size_t) transform_map->columns[0] +
                (size_t)(block_x / 2);
            int macro_x = block_x / 2;
            int macro_y = block_y / 2;
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y) ||
                transform_map->strategies[transform_index] ==
                    N148_TRANSFORM_DCT16) continue;
        }
        int symbol = partition_map->split[index] ^
            n148_partition_map_predict(partition_map, index);
        if (!add_rate_count(counts, INTRA_RATE_PARTITION_MODEL, symbol))
            return 0;
    }
    if (transform_map) {
        for (size_t index = 0; index < transform_map->plane_offsets[1];
             index++) {
            uint8_t strategy = transform_map->strategies[index];
            uint8_t prediction = n148_transform_map_predict(
                transform_map, index);
            int symbol = (strategy + N148_TRANSFORM_STRATEGY_COUNT -
                          prediction) % N148_TRANSFORM_STRATEGY_COUNT;
            if (!add_rate_count(counts, INTRA_RATE_TRANSFORM_MODEL, symbol))
                return 0;
        }
    }

    memset(rate_model, 0, sizeof(*rate_model));
    for (int model = 0; model < INTRA_RATE_MODEL_COUNT; model++) {
        int mode_model_offset = model - INTRA_RATE_COEFFICIENT_MODELS;
        if (allowed_modes > N148_DIRECTIONAL_INTRA_MODE_COUNT &&
            mode_model_offset >= 0 &&
            mode_model_offset < INTRA_RATE_MODE_CONTEXTS) {
            uint64_t base_counts[N148_DIRECTIONAL_INTRA_MODE_COUNT] = {0};
            uint64_t total = 0;
            for (int base_mode = 0;
                 base_mode < N148_DIRECTIONAL_INTRA_MODE_COUNT; base_mode++) {
                for (int reference = 0;
                     reference < N148_INTRA_REFERENCE_COUNT;
                     reference++)
                    base_counts[base_mode] += counts[model][base_mode +
                        reference * N148_DIRECTIONAL_INTRA_MODE_COUNT];
                total += base_counts[base_mode];
            }
            const double prior = 0.25;
            double base_denominator = (double) total +
                prior * N148_DIRECTIONAL_INTRA_MODE_COUNT;
            for (int symbol = 0; symbol < 256; symbol++)
                rate_model->bits[model][symbol] = 64.0;
            for (int base_mode = 0;
                 base_mode < N148_DIRECTIONAL_INTRA_MODE_COUNT; base_mode++) {
                double base_bits = -log2(
                    ((double) base_counts[base_mode] + prior) /
                    base_denominator);
                double reference_denominator =
                    (double) base_counts[base_mode] +
                    prior * N148_INTRA_REFERENCE_COUNT;
                for (int reference = 0;
                     reference < N148_INTRA_REFERENCE_COUNT;
                     reference++) {
                    int symbol = base_mode +
                        reference * N148_DIRECTIONAL_INTRA_MODE_COUNT;
                    double reference_bits = -log2(
                        ((double) counts[model][symbol] + prior) /
                        reference_denominator);
                    /* A ternary reference selector carries less than two
                       raw bits. Capping only this RDO proxy prevents a cold
                       first pass from forbidding an otherwise useful line;
                       the final rANS stream still pays its exact cost. */
                    if (reference_bits > 2.0) reference_bits = 2.0;
                    rate_model->bits[model][symbol] =
                        base_bits + reference_bits;
                }
            }
            continue;
        }
        int mode_alphabet = mode_model_offset >= INTRA_RATE_MODE_CONTEXTS ?
            (allowed_modes < N148_DIRECTIONAL_INTRA_MODE_COUNT ? allowed_modes :
                                                        N148_DIRECTIONAL_INTRA_MODE_COUNT) :
            allowed_modes;
        int alphabet = model < INTRA_RATE_DC_MODELS ? 17 :
            (model < INTRA_RATE_COEFFICIENT_MODELS ? 256 :
             (model < INTRA_RATE_PARTITION_MODEL ? mode_alphabet :
              (model == INTRA_RATE_PARTITION_MODEL ? 2 : 3)));
        uint64_t total = 0;
        for (int symbol = 0; symbol < alphabet; symbol++)
            total += counts[model][symbol];
        const double prior = 0.25;
        double denominator = (double) total + prior * alphabet;
        for (int symbol = 0; symbol < 256; symbol++) {
            if (symbol < alphabet) {
                rate_model->bits[model][symbol] = -log2(
                    ((double) counts[model][symbol] + prior) / denominator);
            } else {
                rate_model->bits[model][symbol] = 64.0;
            }
        }
    }
    return 1;
}

static uint64_t block_error(const Plane *source, const Plane *reconstructed,
                            int x0, int y0) {
    int max_x = source->width - x0;
    int max_y = source->height - y0;
    if (max_x > 8) max_x = 8;
    if (max_y > 8) max_y = 8;
    uint64_t error = 0;
    for (int row = 0; row < max_y; row++) {
        for (int column = 0; column < max_x; column++) {
            int difference = sample_clamped(source, x0 + column, y0 + row) -
                sample_clamped(reconstructed, x0 + column, y0 + row);
            error += (uint64_t)(difference * difference);
        }
    }
    return error;
}

static void copy_block_from_plane(const Plane *plane, int x0, int y0,
                                  uint8_t block[64]) {
    for (int row = 0; row < 8; row++)
        for (int column = 0; column < 8; column++)
            block[row * 8 + column] =
                (uint8_t) sample_clamped(plane, x0 + column, y0 + row);
}

static void restore_valid_block(Plane *plane, int x0, int y0,
                                const uint8_t block[64]) {
    int max_x = plane->width - x0;
    int max_y = plane->height - y0;
    if (max_x > 8) max_x = 8;
    if (max_y > 8) max_y = 8;
    for (int row = 0; row < max_y; row++)
        memcpy(plane->data + (long)(y0 + row) * plane->width + x0,
               block + row * 8, (size_t) max_x);
}

static void take_macro_snapshot(const Plane *plane, int x0, int y0,
                                IntraMacroSnapshot *snapshot) {
    snapshot->x = x0 >= 2 ? x0 - 2 : 0;
    snapshot->y = y0 >= 2 ? y0 - 2 : 0;
    int right = x0 + 16;
    int bottom = y0 + 16;
    if (right > plane->width) right = plane->width;
    if (bottom > plane->height) bottom = plane->height;
    snapshot->width = right - snapshot->x;
    snapshot->height = bottom - snapshot->y;
    for (int row = 0; row < snapshot->height; row++)
        memcpy(snapshot->pixels + row * 18,
               plane->data + (long)(snapshot->y + row) * plane->width +
                   snapshot->x,
               (size_t) snapshot->width);
}

static void restore_macro_snapshot(Plane *plane,
                                   const IntraMacroSnapshot *snapshot) {
    for (int row = 0; row < snapshot->height; row++)
        memcpy(plane->data +
                   (long)(snapshot->y + row) * plane->width + snapshot->x,
               snapshot->pixels + row * 18,
               (size_t) snapshot->width);
}

static void take_super_snapshot(const Plane *plane, int x0, int y0,
                                IntraSuperSnapshot *snapshot) {
    snapshot->x = x0 >= 2 ? x0 - 2 : 0;
    snapshot->y = y0 >= 2 ? y0 - 2 : 0;
    int right = x0 + 32;
    int bottom = y0 + 32;
    if (right > plane->width) right = plane->width;
    if (bottom > plane->height) bottom = plane->height;
    snapshot->width = right - snapshot->x;
    snapshot->height = bottom - snapshot->y;
    for (int row = 0; row < snapshot->height; row++)
        memcpy(snapshot->pixels + row * 34,
               plane->data + (long)(snapshot->y + row) * plane->width +
                   snapshot->x,
               (size_t) snapshot->width);
}

static void restore_super_snapshot(Plane *plane,
                                   const IntraSuperSnapshot *snapshot) {
    for (int row = 0; row < snapshot->height; row++)
        memcpy(plane->data +
                   (long)(snapshot->y + row) * plane->width + snapshot->x,
               snapshot->pixels + row * 34,
               (size_t) snapshot->width);
}

static uint64_t region_error(const Plane *source,
                             const Plane *reconstructed,
                             int x0, int y0, int size) {
#if defined(__x86_64__) || defined(__i386__)
    if ((size == 4 || size == 8 || size == 16 || size == 32) &&
        x0 >= 0 && y0 >= 0 && size <= source->width-x0 &&
        size <= source->height-y0 && size <= reconstructed->width-x0 &&
        size <= reconstructed->height-y0 && n148_cpu_level() >= N148_CPU_AVX2)
        return pixel_error(source->data+(long)y0*source->width+x0,
            source->width, reconstructed->data+(long)y0*reconstructed->width+x0,
            reconstructed->width, size, UINT64_MAX);
#endif
    int max_x = source->width - x0;
    int max_y = source->height - y0;
    if (max_x > size) max_x = size;
    if (max_y > size) max_y = size;
    uint64_t error = 0;
    for (int row = 0; row < max_y; row++) {
        for (int column = 0; column < max_x; column++) {
            int difference = sample_clamped(source, x0 + column, y0 + row) -
                sample_clamped(reconstructed, x0 + column, y0 + row);
            error += (uint64_t)(difference * difference);
        }
    }
    return error;
}

static uint64_t fidelity_variance_sqrt(uint64_t value) {
    /* This is only called for the variance of at most 32x32 8-bit pixels.
       The variance is below 2^37, so double represents the input exactly
       and both candidate squares fit in uint64_t. Correct the hardware
       square-root estimate to keep the RDO decision bit-for-bit identical. */
    uint64_t result = (uint64_t) sqrt((double) value);
    if (result * result > value) result--;
    else if ((result + 1) * (result + 1) <= value) result++;
    return result;
}

/* A deterministic contrast term approximates SSIM's local-variance factor.
   sqrt(n*sum(x^2)-sum(x)^2) equals n times the population standard
   deviation. Four times its squared mismatch is added on the SSE scale. The
   term is encoder-only and applies exclusively to luma RDO. */
static uint64_t fidelity_region_error(
    const Plane *source, const Plane *reconstructed,
    int x0, int y0, int size, int apply_fidelity) {
    uint64_t error = region_error(source, reconstructed, x0, y0, size);
    if (!apply_fidelity ||
        !(fidelity_tools & N148_FIDELITY_STRUCTURAL_RDO)) return error;
    int max_x = source->width - x0;
    int max_y = source->height - y0;
    if (max_x > size) max_x = size;
    if (max_y > size) max_y = size;
    uint64_t samples = (uint64_t) max_x * (uint64_t) max_y;
    if (samples == 0) return error;
    uint64_t source_sum = 0, reconstructed_sum = 0;
    uint64_t source_squares = 0, reconstructed_squares = 0;
    for (int row = 0; row < max_y; row++) {
        for (int column = 0; column < max_x; column++) {
            uint64_t original = (uint64_t) sample_clamped(
                source, x0 + column, y0 + row);
            uint64_t decoded = (uint64_t) sample_clamped(
                reconstructed, x0 + column, y0 + row);
            source_sum += original;
            reconstructed_sum += decoded;
            source_squares += original * original;
            reconstructed_squares += decoded * decoded;
        }
    }
    uint64_t source_variance = samples * source_squares -
        source_sum * source_sum;
    uint64_t reconstructed_variance = samples * reconstructed_squares -
        reconstructed_sum * reconstructed_sum;
    uint64_t source_root = fidelity_variance_sqrt(source_variance);
    uint64_t reconstructed_root = fidelity_variance_sqrt(
        reconstructed_variance);
    uint64_t difference = source_root > reconstructed_root ?
        source_root - reconstructed_root :
        reconstructed_root - source_root;
    uint64_t denominator = (samples + 2u) / 4u;
    if (denominator == 0) denominator = 1;
    uint64_t penalty = (difference * difference + denominator / 2u) /
        denominator;
    return UINT64_MAX - error < penalty ? UINT64_MAX : error + penalty;
}

static double fidelity_rdo_lambda(int dc_step, int apply_fidelity) {
    double scale = apply_fidelity ? fidelity_luma_rdo_scale :
        INTRA_RDO_LAMBDA_SCALE;
    if (apply_fidelity && n148_fidelity_detail_reconstruction()) scale *= 0.9;
    return scale * dc_step * dc_step;
}

static double estimate_coefficient_unit_bits(
    const IntraRateModel *rate_model, const N148CoeffPlane *plane,
    long block, int blocks_x, int plane_class, int previous_dc_category,
    const short values[64], int packed_offset, int length,
    int *ending_dc_category) {
    const short *unit = values + packed_offset;
    int category = coefficient_category(unit[0]);
    int model = rate_dc_model(plane_class, previous_dc_category);
    double bits = rate_model->bits[model][category] + category;
    if (ending_dc_category) *ending_dc_category = category;
    int next_position = 1;
    int previous_ac_category = 0;
    uint64_t active = coefficient_unit_mask(unit, length) & ~1ull;
    while (active) {
#if defined(__GNUC__) || defined(__clang__)
        int position = __builtin_ctzll(active);
#else
        int position = 0;
        uint64_t probe = active;
        while (!(probe & 1u)) { position++; probe >>= 1; }
#endif
        active &= active - 1;
        int run = position - next_position;
        while (run > 15) {
            model = rate_ac_model_packed(
                plane, block, blocks_x, plane_class,
                packed_offset + next_position, next_position,
                previous_ac_category);
            bits += rate_model->bits[model][0xf0];
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        category = coefficient_category(unit[position]);
        model = rate_ac_model_packed(
            plane, block, blocks_x, plane_class,
            packed_offset + next_position, next_position,
            previous_ac_category);
        bits += rate_model->bits[model][(run << 4) | category] + category;
        next_position = position + 1;
        previous_ac_category = category;
    }
    if (next_position < length) {
        model = rate_ac_model_packed(
            plane, block, blocks_x, plane_class,
            packed_offset + next_position, next_position,
            previous_ac_category);
        bits += rate_model->bits[model][0];
    }
    return bits;
}

static double estimate_coefficient_bits(
    const IntraRateModel *rate_model, const N148CoeffPlane *plane,
    long block, int blocks_x, int plane_class, int previous_dc_category,
    const short values[64], int split) {
    int parts = split ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
    int length = split ? 16 : 64;
    double bits = 0.0;
    int dc_category = previous_dc_category;
    for (int part = 0; part < parts; part++) {
        bits += estimate_coefficient_unit_bits(
            rate_model, plane, block, blocks_x, plane_class, dc_category,
            values, part * 16, length, &dc_category);
    }
    return bits;
}

static double estimate_large_coefficient_bits(
    const IntraRateModel *rate_model, int plane_class,
    int previous_dc_category, const short *values, int length) {
    if (!values || (length != 16 * 16 && length != 32 * 32)) return DBL_MAX;
    int category = coefficient_category(values[0]);
    double bits = rate_model->bits[
        rate_dc_model(plane_class, previous_dc_category)][category] +
        category;
    int next_position = 1;
    int previous_ac_category = 0;
    for (int position = 1; position < length; position++) {
        if (values[position] == 0) continue;
        int run = position - next_position;
        while (run > 15) {
            int model = rate_large_ac_model(
                plane_class, next_position, previous_ac_category);
            bits += rate_model->bits[model][0xf0];
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        category = coefficient_category(values[position]);
        int model = rate_large_ac_model(
            plane_class, next_position, previous_ac_category);
        bits += rate_model->bits[model][(run << 4) | category] + category;
        next_position = position + 1;
        previous_ac_category = category;
    }
    if (next_position < length) {
        int model = rate_large_ac_model(
            plane_class, next_position, previous_ac_category);
        bits += rate_model->bits[model][0];
    }
    return bits;
}

typedef struct {
    double cost;
    int previous_position;
    int previous_category;
    short value;
} IntraTrellisState;

typedef struct {
    double cost;
    int category;
} IntraTrellisPredecessor;

static int add_trellis_candidate(short candidates[4], int count, int value) {
    if (value == 0 || value < SHRT_MIN || value > SHRT_MAX) return count;
    for (int index = 0; index < count; index++)
        if (candidates[index] == value) return count;
    if (count < 4) candidates[count++] = (short) value;
    return count;
}

/* lrintf followed by int conversion, specialized only for the finite range
   where the SSE2 conversion has the same result and floating-point flags.
   It observes the current hardware rounding mode. Out-of-range values keep
   the existing library path. */
static inline int trellis_round(float value) {
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SSE2__)
    if (value >= -2147483648.0f && value < 2147483648.0f)
        return _mm_cvtss_si32(_mm_set_ss(value));
#endif
    return (int)lrintf(value);
}


static int trellis_candidates(float ideal, short input, int effort,
                              int trellis_policy,
                              short candidates[4]) {
    int count = 0;
    int rounded = trellis_round(ideal);
    count = add_trellis_candidate(candidates, count, input);
    count = add_trellis_candidate(candidates, count, rounded);
    if (rounded > 1)
        count = add_trellis_candidate(candidates, count, rounded - 1);
    else if (rounded < -1)
        count = add_trellis_candidate(candidates, count, rounded + 1);
    if (trellis_policy == N148_TRELLIS_PERCEPTUAL_LUMA || effort >= 8) {
        if (rounded > 0)
            count = add_trellis_candidate(candidates, count, rounded + 1);
        else if (rounded < 0)
            count = add_trellis_candidate(candidates, count, rounded - 1);
        else if (ideal != 0.0f)
            count = add_trellis_candidate(
                candidates, count, ideal < 0.0f ? -1 : 1);
    }
    return count;
}

static double trellis_coefficient_distortion(float ideal, short value,
                                              double energy) {
    double difference = (double) ideal - value;
    return difference * difference * energy;
}

static double trellis_frequency_energy(double energy, int length,
                                       int position, int trellis_policy) {
    if (trellis_policy != N148_TRELLIS_PERCEPTUAL_LUMA) return energy;
    int size = length == 16 ? 4 : 8;
    int natural = size == 4 ? n148_variable_zigzag(4, position) :
        ZIGZAG[position];
    int frequency = natural / size + natural % size;
    if (frequency > PERCEPTUAL_LUMA_PROTECTED_FREQUENCY_SUM) return energy;
    return energy * (1.0 + PERCEPTUAL_LUMA_LOW_FREQUENCY_BOOST /
                      (frequency + 1.0));
}

typedef struct {
    const IntraRateModel *rate_model;
    int base_model[64];
    uint8_t neighbor_active[64];
} IntraTrellisRateCache;

static int trellis_ac_model(const IntraTrellisRateCache *cache,
                            int position, int previous_category) {
    return cache->base_model[position] +
        (previous_category >= 3 || cache->neighbor_active[position]);
}

static double estimate_trellis_event_bits(
    const IntraTrellisRateCache *cache,
    int next_position, int previous_category, int position, int category) {
    int run = position - next_position;
    double bits = 0.0;
    while (run > 15) {
        int model = trellis_ac_model(cache, next_position, previous_category);
        bits += cache->rate_model->bits[model][0xf0];
        next_position += 16;
        previous_category = 0;
        run -= 16;
    }
    int model = trellis_ac_model(cache, next_position, previous_category);
    return bits + cache->rate_model->bits[model][(run << 4) | category] +
        category;
}

static double estimate_trellis_eob_bits(
    const IntraTrellisRateCache *cache,
    int next_position, int previous_category, int length) {
    if (next_position >= length) return 0.0;
    int model = trellis_ac_model(cache, next_position, previous_category);
    return cache->rate_model->bits[model][0];
}

static double cached_trellis_unit_bits(
    const IntraTrellisRateCache *cache, int plane_class, int previous_dc_category,
    const short values[64], int packed_offset, int length, int *ending_dc_category) {
    const short *unit = values + packed_offset;
    int category = coefficient_category(unit[0]);
    int model = rate_dc_model(plane_class, previous_dc_category);
    double bits = cache->rate_model->bits[model][category] + category;
    if (ending_dc_category) *ending_dc_category = category;
    int next_position = 1;
    int previous_ac_category = 0;
    uint64_t active = coefficient_unit_mask(unit, length) & ~1ull;
    while (active) {
#if defined(__GNUC__) || defined(__clang__)
        int position = __builtin_ctzll(active);
#else
        int position = 0;
        uint64_t probe = active;
        while (!(probe & 1u)) { position++; probe >>= 1; }
#endif
        active &= active - 1;
        int run = position - next_position;
        while (run > 15) {
            model = trellis_ac_model(cache, next_position, previous_ac_category);
            bits += cache->rate_model->bits[model][0xf0];
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        category = coefficient_category(unit[position]);
        model = trellis_ac_model(cache, next_position, previous_ac_category);
        bits += cache->rate_model->bits[model][(run << 4) | category] + category;
        next_position = position + 1;
        previous_ac_category = category;
    }
    if (next_position < length) {
        model = trellis_ac_model(cache, next_position, previous_ac_category);
        bits += cache->rate_model->bits[model][0];
    }
    return bits;
}

static double cached_trellis_objective(
    const IntraTrellisRateCache *cache, int plane_class, int previous_dc_category,
    const float *ideal_levels, const double *effective_energy,
    const short values[64], int packed_offset, int length, double lambda) {
    double distortion = 0.0;
    for (int position = 1; position < length; position++)
        distortion += trellis_coefficient_distortion(
            ideal_levels[position], values[packed_offset + position], effective_energy[position]);
    double bits = cached_trellis_unit_bits(cache, plane_class, previous_dc_category,
                                          values, packed_offset, length, NULL);
    return distortion + lambda * bits;
}

static double trellis_unit_objective(
    const IntraRateModel *rate_model, const N148CoeffPlane *plane,
    long block, int blocks_x, int plane_class, int previous_dc_category,
    const float *ideal_levels, const double *energy,
    const short values[64], int packed_offset, int length, double lambda,
    int trellis_policy) {
    double distortion = 0.0;
    for (int position = 1; position < length; position++)
        distortion += trellis_coefficient_distortion(
            ideal_levels[position], values[packed_offset + position],
            trellis_frequency_energy(
                energy[position], length, position, trellis_policy));
    int ignored_dc_category;
    double bits = estimate_coefficient_unit_bits(
        rate_model, plane, block, blocks_x, plane_class,
        previous_dc_category, values, packed_offset, length,
        &ignored_dc_category);
    return distortion + lambda * bits;
}

/* Dynamic programming over run/category states. Unlike the base proxy, this
   pass prices the exact contextual symbols that the current frame will emit
   and weights transform directions by their spatial gradient energy. The
   perceptual-luma profile's
   policy additionally protects selected low frequencies. */
static int refine_coefficients_contextually(
    const IntraRateModel *rate_model, const N148CoeffPlane *plane,
    long block, int blocks_x, int plane_class, int previous_dc_category,
    const float *ideal_levels, const double *energy, int packed_offset,
    int length, int effort, int trellis_policy, int structural_sse,
    short values[64]) {
    if (!rate_model || !plane || !ideal_levels || !energy || !values ||
        length < 2 || length > 64 || packed_offset < 0 ||
        packed_offset + length > 64 || effort < 0 || effort > 9 ||
        trellis_policy < N148_TRELLIS_LEGACY ||
        trellis_policy > N148_TRELLIS_PERCEPTUAL_LUMA)
        return 0;
    double lambda = (structural_sse ? STRUCTURAL_TRELLIS_LAMBDA_SCALE :
        trellis_policy == N148_TRELLIS_PERCEPTUAL_LUMA ?
        PERCEPTUAL_LUMA_TRELLIS_LAMBDA_SCALE :
        CONTEXTUAL_TRELLIS_LAMBDA_SCALE) * energy[0];
    if (structural_sse && n148_fidelity_balanced_reconstruction())
        lambda = (plane_class ? 0.068 : 0.068) * energy[0];
    /* With no AC input and no available nonzero level, the DP has
       only its all-zero path. Check the same objective's finiteness once. */
    if (trellis_policy == N148_TRELLIS_LEGACY && effort < 8 &&
        !(coefficient_unit_mask(values + packed_offset, length) & ~1ull)) {
        int only_zero = 1;
        for (int position = 1; position < length; ++position) {
            if (trellis_round(ideal_levels[position]) != 0) {
                only_zero = 0;
                break;
            }
        }
        if (only_zero) return isfinite(trellis_unit_objective(
            rate_model, plane, block, blocks_x, plane_class,
            previous_dc_category, ideal_levels, energy, values,
            packed_offset, length, lambda, trellis_policy));
    }
    IntraTrellisRateCache cache = {.rate_model = rate_model};
    double effective_energy[64];
    for (int position = 1; position < length; position++) {
        cache.base_model[position] = INTRA_RATE_DC_MODELS +
            ((plane_class * INTRA_RATE_BANDS + rate_band(position)) *
             INTRA_RATE_ACTIVITY_BUCKETS);
        cache.neighbor_active[position] = (uint8_t) rate_neighbor_active(
            plane, block, blocks_x, packed_offset + position);
        effective_energy[position] = trellis_frequency_energy(
            energy[position], length, position, trellis_policy);
    }
    double input_distortion = 0.0;
    double zero_prefix[65] = {0.0};
    for (int position = 1; position < length; position++) {
        zero_prefix[position + 1] = zero_prefix[position] +
            trellis_coefficient_distortion(
                ideal_levels[position], 0, effective_energy[position]);
        input_distortion += trellis_coefficient_distortion(
            ideal_levels[position], values[packed_offset + position], effective_energy[position]);
    }
    /* Only two classes of predecessor category affect any later event.
       Retain one state per class, and a separate terminal state so final
       floating-point ties are still resolved over every category. */
    IntraTrellisState states[64][2];
    int active_positions[64];
    int active_count = 0;
    double prefix_minimum_base[64], prefix_maximum_magnitude[64];
    double best = zero_prefix[length] - zero_prefix[1] + lambda *
        estimate_trellis_eob_bits(&cache, 1, 0, length);
    int best_position = -1;
    int best_category = 0;
    IntraTrellisState terminal = {0};

    for (int position = 1; position < length; position++) {
        short candidates[4];
        int candidate_count = trellis_candidates(
            ideal_levels[position], values[packed_offset + position],
            effort, trellis_policy, candidates);
        if (!candidate_count) continue;
        IntraTrellisState current[4];
        int categories[4];
        int current_count = 0;
        int first_category = coefficient_category(candidates[0]);
        double first_distortion = trellis_coefficient_distortion(
            ideal_levels[position], candidates[0], effective_energy[position]);
        for (int candidate_index = 0; candidate_index < candidate_count;
             candidate_index++) {
            short value = candidates[candidate_index];
            int category = coefficient_category(value);
            if (category <= 0 || category > 15) continue;
            double coefficient_cost = trellis_coefficient_distortion(
                ideal_levels[position], value, effective_energy[position]);
            /* Same category means identical transition rates. A later
               candidate with no smaller distortion cannot beat the first one,
               including floating-point ties, which retain the earlier value. */
            if (candidate_index && category == first_category &&
                coefficient_cost >= first_distortion) continue;
            double current_best = zero_prefix[position] - zero_prefix[1] +
                coefficient_cost + lambda * estimate_trellis_event_bits(
                    &cache, 1, 0, position, category);
            int previous_position = -1;
            int previous_bucket = 0;
            for (int active = active_count - 1; active >= 0; active--) {
                int previous = active_positions[active];
                double group_bound = prefix_minimum_base[active] +
                    zero_prefix[position] + coefficient_cost;
                /* Conservative allowance for the reassociated lower bound.
                   Overflow disables pruning. Original costs and tie order
                   are still used for every surviving predecessor. */
                double slack = 64.0 * DBL_EPSILON *
                    (prefix_maximum_magnitude[active] + zero_prefix[position] +
                     coefficient_cost + fabs(current_best) + 1.0);
                if (group_bound > current_best + slack) break;
                for (int bucket = 0; bucket < 2; bucket++) {
                    const IntraTrellisState *prior = &states[previous][bucket];
                    if (prior->cost == DBL_MAX) continue;
                    double lower_bound = prior->cost + zero_prefix[position] -
                        zero_prefix[previous + 1] + coefficient_cost;
                    /* Same evaluation order as the full cost, before adding
                       its nonnegative rate. Strict bound leaves equal-cost
                       predecessor ties available for original order. */
                    if (lower_bound > current_best) continue;
                    double cost = lower_bound + lambda *
                        estimate_trellis_event_bits(
                            &cache, previous + 1, bucket ? 3 : 1,
                            position, category);
                    if (cost < current_best ||
                        (cost == current_best && previous_position >= 0 &&
                         (previous < previous_position ||
                          (previous == previous_position && bucket < previous_bucket)))) {
                        current_best = cost;
                        previous_position = previous;
                        previous_bucket = bucket;
                    }
                }
            }
            int slot = 0;
            while (slot < current_count && categories[slot] != category) slot++;
            if (slot == current_count) {
                categories[slot] = category;
                current[slot].cost = DBL_MAX;
                current_count++;
            }
            if (current_best < current[slot].cost) {
                current[slot].cost = current_best;
                current[slot].previous_position = previous_position;
                current[slot].previous_category = previous_bucket;
                current[slot].value = value;
            }
        }
        states[position][0].cost = DBL_MAX;
        states[position][1].cost = DBL_MAX;
        states[position][0].value = states[position][1].value = 0;
        for (int slot = 0; slot < current_count; slot++) {
            const IntraTrellisState *node = &current[slot];
            if (node->cost == DBL_MAX) continue;
            int category = categories[slot];
            int bucket = category >= 3;
            IntraTrellisState *kept = &states[position][bucket];
            if (node->cost < kept->cost ||
                (node->cost == kept->cost &&
                 category < coefficient_category(kept->value)))
                *kept = *node;
            double cost = node->cost + zero_prefix[length] -
                zero_prefix[position + 1] + lambda *
                estimate_trellis_eob_bits(&cache, position + 1, category, length);
            if (cost < best || (cost == best && best_position == position &&
                                category < best_category)) {
                best = cost;
                best_position = position;
                best_category = category;
                terminal = *node;
            }
        }
        if (states[position][0].cost != DBL_MAX ||
            states[position][1].cost != DBL_MAX) {
            double minimum = active_count ? prefix_minimum_base[active_count-1] : DBL_MAX;
            double magnitude = active_count ? prefix_maximum_magnitude[active_count-1] : 0.0;
            for (int bucket = 0; bucket < 2; bucket++) {
                double cost = states[position][bucket].cost;
                if (cost == DBL_MAX) continue;
                double base = cost - zero_prefix[position + 1];
                if (base < minimum) minimum = base;
                double bound = fabs(cost) + zero_prefix[position + 1];
                if (bound > magnitude) magnitude = bound;
            }
            prefix_minimum_base[active_count] = minimum;
            prefix_maximum_magnitude[active_count] = magnitude;
            active_positions[active_count++] = position;
        }
    }

    short input[64];
    memcpy(input, values, sizeof(input));
    memset(values + packed_offset + 1, 0,
           (size_t)(length - 1) * sizeof(*values));
    if (best_position >= 1) {
        values[packed_offset + best_position] = terminal.value;
        int previous_position = terminal.previous_position;
        int previous_bucket = terminal.previous_category;
        while (previous_position >= 1) {
            const IntraTrellisState *node = &states[previous_position][previous_bucket];
            values[packed_offset + previous_position] = node->value;
            previous_position = node->previous_position;
            previous_bucket = node->previous_category;
        }
    }
    double input_cost = input_distortion + lambda * cached_trellis_unit_bits(
        &cache, plane_class, previous_dc_category, input, packed_offset, length, NULL);
    /* Equal coefficient vectors have exactly equal objectives. Preserve the
       original nonfinite rejection while avoiding its duplicate calculation. */
    if (memcmp(input, values, sizeof(input)) == 0) return isfinite(input_cost);
    double output_cost = cached_trellis_objective(
        &cache, plane_class, previous_dc_category, ideal_levels, effective_energy,
        values, packed_offset, length, lambda);
    if (!isfinite(input_cost) || !isfinite(output_cost) ||
        output_cost > input_cost + 1e-9 * (fabs(input_cost) + 1.0)) {
        memcpy(values, input, sizeof(input));
        return 0;
    }
    return 1;
}

static int candidate_mode_at_virtual(
    const uint8_t *committed_modes, long current_block,
    const uint8_t candidate_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK],
    int virtual_x, int virtual_y, int blocks_x) {
    int block_x = virtual_x / 2;
    int block_y = virtual_y / 2;
    int part = (virtual_y & 1) * 2 + (virtual_x & 1);
    long block = (long) block_y * blocks_x + block_x;
    if (block == current_block) return candidate_modes[part];
    return committed_modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
}

static int candidate_reference_prediction(
    const uint8_t *committed_modes, long block,
    const uint8_t candidate_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK],
    int part, int blocks_x, int base_mode) {
    int block_x = (int)(block % blocks_x);
    int block_y = (int)(block / blocks_x);
    int virtual_x = block_x * 2 + (part & 1);
    int virtual_y = block_y * 2 + (part >> 1);
    int has_left = virtual_x > 0;
    int has_top = virtual_y > 0;
    int left = has_left ? candidate_mode_at_virtual(
        committed_modes, block, candidate_modes,
        virtual_x - 1, virtual_y, blocks_x) : 0;
    int top = has_top ? candidate_mode_at_virtual(
        committed_modes, block, candidate_modes,
        virtual_x, virtual_y - 1, blocks_x) : 0;
    int left_matches = has_left &&
        n148_intra_base_mode(left) == base_mode;
    int top_matches = has_top &&
        n148_intra_base_mode(top) == base_mode;
    if (left_matches && top_matches) {
        int left_reference = n148_intra_reference_index(left);
        int top_reference = n148_intra_reference_index(top);
        return left_reference == top_reference ? left_reference : 0;
    }
    if (left_matches) return n148_intra_reference_index(left);
    if (top_matches) return n148_intra_reference_index(top);
    return 0;
}

static int candidate_mode_context(
    const uint8_t *committed_modes, long block,
    const uint8_t candidate_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK],
    int part, int blocks_x) {
    int block_x = (int)(block % blocks_x);
    int block_y = (int)(block / blocks_x);
    int virtual_x = block_x * 2 + (part & 1);
    int virtual_y = block_y * 2 + (part >> 1);
    int has_left = virtual_x > 0;
    int has_top = virtual_y > 0;
    if (!has_left && !has_top) return 0;
    int left = has_left ? n148_intra_base_mode(candidate_mode_at_virtual(
        committed_modes, block, candidate_modes,
        virtual_x - 1, virtual_y, blocks_x)) : -1;
    int top = has_top ? n148_intra_base_mode(candidate_mode_at_virtual(
        committed_modes, block, candidate_modes,
        virtual_x, virtual_y - 1, blocks_x)) : -1;
    if (has_left && has_top && left != top)
        return N148_DIRECTIONAL_INTRA_MODE_COUNT + 1;
    return 1 + (has_left ? left : top);
}

static double estimate_mode_bits_from(
    const IntraRateModel *rate_model, int plane_class,
    const uint8_t *committed_modes, long block,
    const uint8_t candidate_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK],
    int first_part, int parts, int blocks_x, double bits) {
    for (int part = first_part; part < parts; part++) {
        int context = candidate_mode_context(
            committed_modes, block, candidate_modes, part, blocks_x);
        int model = INTRA_RATE_COEFFICIENT_MODELS +
            plane_class * INTRA_RATE_MODE_CONTEXTS + context;
        int mode = candidate_modes[part];
        int symbol = mode;
        if (plane_class == 0 &&
            (fidelity_tools & N148_FIDELITY_MULTIPLE_REFERENCES)) {
            int base_mode = n148_intra_base_mode(mode);
            int prediction = candidate_reference_prediction(
                committed_modes, block, candidate_modes, part, blocks_x,
                base_mode);
            int residual = (n148_intra_reference_index(mode) +
                N148_INTRA_REFERENCE_COUNT - prediction) %
                N148_INTRA_REFERENCE_COUNT;
            symbol = base_mode + N148_DIRECTIONAL_INTRA_MODE_COUNT * residual;
        }
        bits += rate_model->bits[model][symbol];
    }
    return bits;
}

static double estimate_mode_bits(
    const IntraRateModel *rate_model, int plane_class,
    const uint8_t *committed_modes, long block,
    const uint8_t candidate_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK],
    int parts, int blocks_x) {
    return estimate_mode_bits_from(
        rate_model, plane_class, committed_modes, block, candidate_modes,
        0, parts, blocks_x, 0.0);
}

static double estimate_partition_bits(
    const IntraRateModel *rate_model, const N148PartitionMap *partition_map,
    size_t map_index, int split) {
    int prediction = n148_partition_map_predict(partition_map, map_index);
    return rate_model->bits[INTRA_RATE_PARTITION_MODEL][split ^ prediction];
}

/* Rank four base modes and two angular candidates using quantized SSE
   plus the initial contextual rate; reconstruct and trellis the winner. */
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static uint64_t activity_mask_avx2(const short *values) {
    uint64_t result = 0;
    __m256i upper = _mm256_set1_epi16(3), lower = _mm256_set1_epi16(-3);
    for (int i = 0; i < 64; i += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(values+i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(values+i+16));
        a = _mm256_or_si256(_mm256_cmpgt_epi16(a,upper),_mm256_cmpgt_epi16(lower,a));
        b = _mm256_or_si256(_mm256_cmpgt_epi16(b,upper),_mm256_cmpgt_epi16(lower,b));
        __m256i packed = _mm256_permute4x64_epi64(_mm256_packs_epi16(a,b),0xd8);
        result |= (uint64_t)(uint32_t)_mm256_movemask_epi8(packed) << i;
    }
    return result;
}
#endif
static uint64_t activity_mask(const short *values) {
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) return activity_mask_avx2(values);
#endif
    uint64_t mask = 0;
    for (int i=0;i<64;i++) if (values[i] <= -4 || values[i] >= 4) mask |= 1ull<<i;
    return mask;
}
static uint64_t coefficient_neighbor_mask(const N148CoeffPlane *plane,
                                         long block, int blocks_x) {
    uint64_t mask = 0;
    if (block % blocks_x) mask |= activity_mask(plane->coefficients+(block-1)*64);
    if (block >= blocks_x) mask |= activity_mask(plane->coefficients+(block-blocks_x)*64);
    return mask;
}

static double cached_coefficient8_bits(
    const IntraRateModel *rate_model, uint64_t neighbor_mask,
    int previous_dc_category, const short values[64]) {
    const short *unit = values;
    const int plane_class = 0, length = 64;
    int category = coefficient_category(unit[0]);
    int model = rate_dc_model(plane_class, previous_dc_category);
    double bits = rate_model->bits[model][category] + category;
    int next_position = 1;
    int previous_ac_category = 0;
    uint64_t active = coefficient_unit_mask(unit, length) & ~1ull;
    while (active) {
#if defined(__GNUC__) || defined(__clang__)
        int position = __builtin_ctzll(active);
#else
        int position = 0;
        uint64_t probe = active;
        while (!(probe & 1u)) { position++; probe >>= 1; }
#endif
        active &= active - 1;
        int run = position - next_position;
        while (run > 15) {
            model = (INTRA_RATE_DC_MODELS + rate_band(next_position) *
                INTRA_RATE_ACTIVITY_BUCKETS +
                (previous_ac_category >= 3 || ((neighbor_mask >> next_position) & 1u)));
            bits += rate_model->bits[model][0xf0];
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        category = coefficient_category(unit[position]);
        model = (INTRA_RATE_DC_MODELS + rate_band(next_position) *
                INTRA_RATE_ACTIVITY_BUCKETS +
                (previous_ac_category >= 3 || ((neighbor_mask >> next_position) & 1u)));
        bits += rate_model->bits[model][(run << 4) | category] + category;
        next_position = position + 1;
        previous_ac_category = category;
    }
    if (next_position < length) {
        model = (INTRA_RATE_DC_MODELS + rate_band(next_position) *
                INTRA_RATE_ACTIVITY_BUCKETS +
                (previous_ac_category >= 3 || ((neighbor_mask >> next_position) & 1u)));
        bits += rate_model->bits[model][0];
    }
    return bits;
}


/* Compute independent products in SIMD, then accumulate in the original
 * scalar order. A partial nonnegative sum can reject only a losing trial. */
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static double quantized_error_avx2(const float *ideal, const short *levels,
    const double *energy, int length, double mode_cost, double best_cost) {
    double distortion=0.0;
    for (int k=0; k<length; k+=4) {
        __m128i packed=_mm_loadl_epi64((const __m128i *)(levels+k));
        __m256d value=_mm256_cvtepi32_pd(_mm_cvtepi16_epi32(packed));
        __m256d target=_mm256_cvtps_pd(_mm_loadu_ps(ideal+k));
        __m256d delta=_mm256_sub_pd(target,value);
        __m256d term=_mm256_mul_pd(_mm256_mul_pd(delta,delta),_mm256_loadu_pd(energy+k));
        double terms[4];
        _mm256_storeu_pd(terms,term);
        distortion+=terms[0];
        distortion+=terms[1];
        distortion+=terms[2];
        distortion+=terms[3];
        if (distortion+mode_cost>=best_cost) break;
    }
    return distortion;
}
#endif

static double quantized_error(const float *ideal, const short *levels,
    const double *energy, int length, double mode_cost, double best_cost) {
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level()>=N148_CPU_AVX2)
        return quantized_error_avx2(ideal,levels,energy,length,mode_cost,best_cost);
#endif
    double distortion=0.0;
    for (int k=0; k<length; k++) {
        double error=(double)ideal[k]-levels[k];
        distortion+=error*error*energy[k];
        if ((k&3)==3 && distortion+mode_cost>=best_cost) break;
    }
    return distortion;
}

static int choose_quantized_mode8(
    const Plane *source, const Plane *reconstructed, int x0, int y0,
    uint8_t neutral, int mode_count, int causal_right, const IntraQuant8 *quant,
    const IntraRateModel *rate_model, const N148CoeffPlane *coefficient_plane,
    const uint8_t *committed_modes, long block, int blocks_x,
    int previous_dc_category, uint8_t prediction[64],
    short best_coefficients[64], float best_ideal[64]) {
    N148PreparedSquare references;
    prepare_square_references(reconstructed, x0, y0, 8, neutral,
                              causal_right, fidelity_tools, 0, &references);
    if (mode_count > 10) mode_count = 10;
    /* Keep the four established quantized trials, then shortlist two of
       the six existing angular modes by prediction SSE. Only the shortlist
       omits transforms; the selected modes keep the full quantized score. */
    uint8_t extra_prediction[6][64];
    int first_extra = -1, second_extra = -1;
    uint64_t first_sse = UINT64_MAX, second_sse = UINT64_MAX;
    for (int mode = 4; mode < mode_count; mode++) {
        predict_square_prepared(&references, 8, mode, neutral, extra_prediction[mode-4]);
        uint64_t score = prediction_sse(source, x0, y0, 8,
                                       extra_prediction[mode-4], second_sse);
        if (score < first_sse) {
            second_sse = first_sse; second_extra = first_extra;
            first_sse = score; first_extra = mode;
        } else if (score < second_sse) {
            second_sse = score; second_extra = mode;
        }
    }
    uint64_t neighbor_mask = coefficient_neighbor_mask(coefficient_plane, block, blocks_x);
    double best_cost = DBL_MAX;
    int best_mode = -1;
    double lambda = fidelity_rdo_lambda(quant->dc_step, 1);
    for (int mode = 0; mode < mode_count; mode++) {
        if (mode >= 4 && mode != first_extra && mode != second_extra) continue;
        uint8_t trial[64], modes[N148_INTRA_MODE_SLOTS_PER_BLOCK];
        short coefficients[64];
        float ideal[64];
        memset(modes, mode, sizeof(modes));
        double mode_rate = estimate_mode_bits(rate_model, 0, committed_modes,
                                              block, modes, 1, blocks_x);
        /* Distortion and coefficient rate are nonnegative. Later mode ties
           keep the earlier mode, so this bound preserves the full search. */
        if (lambda * mode_rate >= best_cost) continue;
        if (mode >= 4) memcpy(trial, extra_prediction[mode-4], 64);
        else predict_square_prepared(&references, 8, mode, neutral, trial);
        if (!quantize8(source, x0, y0, trial, quant, coefficients, ideal))
            return -1;
        double distortion = quantized_error(ideal, coefficients,
            quant->unit_energy, 64, lambda * mode_rate, best_cost);
        if (distortion + lambda * mode_rate >= best_cost) continue;
        double rate = cached_coefficient8_bits(
            rate_model, neighbor_mask, previous_dc_category, coefficients) + mode_rate;
        double cost = distortion + lambda * rate;
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = mode;
            memcpy(prediction, trial, 64);
            memcpy(best_coefficients, coefficients, sizeof(coefficients));
            memcpy(best_ideal, ideal, sizeof(ideal));
        }
    }
    return best_mode;
}

static int make_unsplit_rdo_candidate(
    const Plane *source, Plane *reconstructed, int x0, int y0,
    uint8_t neutral, int mode_count, int causal_right,
    const IntraQuant8 *quant, int effort,
    int trellis_policy,
    const IntraRateModel *rate_model, const N148CoeffPlane *coefficient_plane,
    const uint8_t *committed_modes, long block, int blocks_x,
    int plane_class, int previous_dc_category,
    const N148PartitionMap *partition_map, size_t map_index,
    IntraBlockCandidate *candidate) {
    uint8_t saved[64];
    copy_block_from_plane(reconstructed, x0, y0, saved);
    int shortlist_mode = -1;
    int runner_up = -1;
    uint64_t prediction_score = 0;
    uint8_t shortlist_prediction[64];
    short shortlist_coefficients[64];
    float shortlist_ideal[64];
    int quantized_shortlist = effort == 3 && plane_class == 0 &&
        (fidelity_tools & N148_FIDELITY_STRUCTURAL_LUMA_QUANT) &&
        (fidelity_tools & N148_FIDELITY_FAST_MODE_SEARCH);
    if (quantized_shortlist) {
        shortlist_mode = choose_quantized_mode8(
            source, reconstructed, x0, y0, neutral, mode_count, causal_right,
            quant, rate_model, coefficient_plane, committed_modes, block,
            blocks_x, previous_dc_category, shortlist_prediction,
            shortlist_coefficients, shortlist_ideal);
        if (shortlist_mode < 0) return 0;
    } else if (effort < 5) {
        int refined_effort3 = effort == 3 &&
            (fidelity_tools & N148_FIDELITY_REFINED_SPECTRAL_LUMA_QUANT);
        int effort4_selective = effort == 4;
        int selective = (effort4_selective || refined_effort3) &&
            plane_class == 0 &&
            (fidelity_tools & N148_FIDELITY_FAST_MODE_SEARCH) &&
            (fidelity_tools & N148_FIDELITY_QUALITY_LAMBDA);
        shortlist_mode = choose_mode(source, reconstructed, x0, y0, 8,
                                     neutral, mode_count, causal_right,
                                     plane_class == 0,
                                     shortlist_prediction,
                                     effort4_selective && selective ?
                                         &runner_up : NULL,
                                     selective ? &prediction_score : NULL);
        /* Effort four spends a second full mode trial only when the fast
           predictor leaves more than three quantizer-sized MSE units. The
           refined effort-three profile uses a higher threshold so its
           small quality correction stays in the standard speed class. */
        if (selective) {
            uint64_t threshold = (refined_effort3 ? 640ull : 192ull) *
                (uint64_t) quant->dc_step * (uint64_t) quant->dc_step;
            if (prediction_score <= threshold) {
                runner_up = -1;
            } else if (refined_effort3) {
                shortlist_mode = choose_mode(
                    source, reconstructed, x0, y0, 8, neutral, mode_count,
                    causal_right, plane_class == 0, shortlist_prediction,
                    &runner_up, NULL);
            }
        }
    }
    double best_cost = DBL_MAX;
    uint64_t nearest_reference_distortion = UINT64_MAX;
    for (int mode = 0; mode < mode_count; mode++) {
        if (shortlist_mode >= 0 && mode != shortlist_mode &&
            mode != runner_up) continue;
        uint8_t prediction[64];
        if (shortlist_mode == mode) memcpy(prediction, shortlist_prediction, 64);
        else predict_square(reconstructed, x0, y0, 8, mode, neutral,
                            causal_right, plane_class == 0, prediction);
        short coefficients[64];
        float ideal_levels[64];
        if (quantized_shortlist) {
            memcpy(coefficients, shortlist_coefficients, sizeof(coefficients));
            memcpy(ideal_levels, shortlist_ideal, sizeof(ideal_levels));
        } else if (!quantize8(source, x0, y0, prediction, quant, coefficients,
                              ideal_levels)) {
            restore_valid_block(reconstructed, x0, y0, saved);
            return 0;
        }
        if (effort >= 3) {
            /* The structural profile prices its ordinary spatial SSE with
               the initial frame's contextual run/category rate model. */
            int structural_context = effort == 3 &&
                (plane_class == 0 || n148_fidelity_balanced_reconstruction()) &&
                trellis_policy == N148_TRELLIS_OFF &&
                (fidelity_tools & N148_FIDELITY_STRUCTURAL_LUMA_QUANT);
            int refined = (trellis_policy != N148_TRELLIS_OFF || structural_context) ?
                refine_coefficients_contextually(
                    rate_model, coefficient_plane, block, blocks_x,
                    plane_class, previous_dc_category, ideal_levels,
                    structural_context ? quant->unit_energy : quant->perceptual_energy,
                    0, 64, effort,
                    structural_context ? N148_TRELLIS_LEGACY : trellis_policy,
                    structural_context, coefficients) :
                n148_rdo_refine_coefficients_trusted(
                    ideal_levels, quant->unit_energy,
                    0.85 * quant->unit_energy[0] *
                        BASE_TRELLIS_LAMBDA_SCALE,
                    effort, coefficients, NULL);
            if (!refined) {
                restore_valid_block(reconstructed, x0, y0, saved);
                return 0;
            }
        }
        if (!reconstruct8(coefficients, prediction, quant, reconstructed,
                          x0, y0)) {
            restore_valid_block(reconstructed, x0, y0, saved);
            return 0;
        }
        uint64_t distortion = fidelity_region_error(
            source, reconstructed, x0, y0, 8, plane_class == 0);
        if (mode < N148_DIRECTIONAL_INTRA_MODE_COUNT) {
            if (distortion < nearest_reference_distortion)
                nearest_reference_distortion = distortion;
        } else if (distortion > nearest_reference_distortion) {
            restore_valid_block(reconstructed, x0, y0, saved);
            continue;
        }
        uint8_t candidate_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK];
        for (int part = 0; part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
            candidate_modes[part] = (uint8_t) mode;
        double rate = estimate_coefficient_bits(
            rate_model, coefficient_plane, block, blocks_x, plane_class,
            previous_dc_category, coefficients, 0) +
            estimate_mode_bits(rate_model, plane_class, committed_modes,
                               block, candidate_modes, 1, blocks_x) +
            estimate_partition_bits(rate_model, partition_map, map_index, 0);
        double lambda = fidelity_rdo_lambda(
            quant->dc_step, plane_class == 0);
        double cost = (double) distortion + lambda * rate;
        if (cost < best_cost) {
            best_cost = cost;
            memcpy(candidate->coefficients, coefficients,
                   sizeof(candidate->coefficients));
            memcpy(candidate->modes, candidate_modes,
                   sizeof(candidate->modes));
            candidate->distortion = distortion;
            candidate->proxy_bits = rate;
            copy_block_from_plane(reconstructed, x0, y0,
                                  candidate->pixels);
        }
        restore_valid_block(reconstructed, x0, y0, saved);
    }
    return best_cost < DBL_MAX;
}

/* Evaluate one explicitly selected 8x8 mode. Joint chroma uses this primitive to price
   Cb and Cr together while preserving separate causal reconstruction buffers
   and coefficient contexts for both planes. Mode signaling is deliberately
   excluded from proxy_bits because the pair pays that cost only once. */
static int make_fixed_unsplit_candidate(
    const Plane *source, Plane *reconstructed, int x0, int y0,
    uint8_t neutral, int mode, const IntraQuant8 *quant, int effort,
    int trellis_policy, const IntraRateModel *rate_model,
    const N148CoeffPlane *coefficient_plane, long block, int blocks_x,
    int previous_dc_category, const uint8_t *precomputed_prediction,
    IntraBlockCandidate *candidate) {
    if (!source || !reconstructed || !quant || !coefficient_plane ||
        !candidate || mode < 0 || mode >= N148_DIRECTIONAL_INTRA_MODE_COUNT ||
        effort < 0 || effort > 9 ||
        (trellis_policy != N148_TRELLIS_OFF && !rate_model)) return 0;
    uint8_t saved[64];
    uint8_t prediction[64];
    float ideal_levels[64];
    copy_block_from_plane(reconstructed, x0, y0, saved);
    if (precomputed_prediction)
        memcpy(prediction, precomputed_prediction, sizeof(prediction));
    else
        predict_square(reconstructed, x0, y0, 8, mode, neutral, -1, 0,
                       prediction);
    if (!quantize8(source, x0, y0, prediction, quant,
                   candidate->coefficients, ideal_levels)) {
        restore_valid_block(reconstructed, x0, y0, saved);
        return 0;
    }
    if (effort >= 3) {
        int refined = rate_model && trellis_policy != N148_TRELLIS_OFF ?
            refine_coefficients_contextually(
                rate_model, coefficient_plane, block, blocks_x, 1,
                previous_dc_category, ideal_levels,
                quant->perceptual_energy, 0, 64, effort, trellis_policy, 0,
                candidate->coefficients) :
            n148_rdo_refine_coefficients_trusted(
                ideal_levels, quant->unit_energy,
                0.85 * quant->unit_energy[0] * BASE_TRELLIS_LAMBDA_SCALE,
                effort, candidate->coefficients, NULL);
        if (!refined) {
            restore_valid_block(reconstructed, x0, y0, saved);
            return 0;
        }
    }
    if (!reconstruct8(candidate->coefficients, prediction, quant,
                      reconstructed, x0, y0)) {
        restore_valid_block(reconstructed, x0, y0, saved);
        return 0;
    }
    for (int part = 0; part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
        candidate->modes[part] = (uint8_t) mode;
    candidate->distortion = region_error(
        source, reconstructed, x0, y0, 8);
    candidate->proxy_bits = rate_model ? estimate_coefficient_bits(
        rate_model, coefficient_plane, block, blocks_x, 1,
        previous_dc_category, candidate->coefficients, 0) :
        n148_rdo_estimate_block_bits(candidate->coefficients);
    copy_block_from_plane(reconstructed, x0, y0, candidate->pixels);
    restore_valid_block(reconstructed, x0, y0, saved);
    return 1;
}

/* Approximate the RGB impact of simultaneous Cb/Cr residual error. The
   coefficients come from the established BT.601 inverse matrix and are
   normalized so the two diagonal weights average to one. Keeping the cross
   term matters when both chroma residuals move the reconstructed green
   channel in the same direction. */
static double joint_chroma_distortion(
    const Plane *cb, const Plane *cr,
    const IntraBlockCandidate candidate[2], int x0, int y0) {
    int max_x = cb->width - x0;
    int max_y = cb->height - y0;
    if (max_x > 8) max_x = 8;
    if (max_y > 8) max_y = 8;
    double error = 0.0;
    for (int row = 0; row < max_y; row++) {
        for (int column = 0; column < max_x; column++) {
            int offset = row * 8 + column;
            int cb_error = sample_clamped(cb, x0 + column, y0 + row) -
                candidate[0].pixels[offset];
            int cr_error = sample_clamped(cr, x0 + column, y0 + row) -
                candidate[1].pixels[offset];
            error += 1.1364 * cb_error * cb_error +
                0.8636 * cr_error * cr_error +
                0.1715 * cb_error * cr_error;
        }
    }
    return error;
}

/* Native 4x4 predictor ranking by quantized spatial error and native syntax rate.
   The coefficient DP still runs only on the selected predictor. */
static double cached_coefficient4_bits(
    const IntraRateModel *rate_model, uint64_t neighbor_mask,
    int previous_dc_category, const short values[16]) {
    const short *unit = values;
    const int plane_class = 0, length = 16;
    int category = coefficient_category(unit[0]);
    int model = rate_dc_model(plane_class, previous_dc_category);
    double bits = rate_model->bits[model][category] + category;
    int next_position = 1;
    int previous_ac_category = 0;
    uint64_t active = coefficient_unit_mask(unit, length) & ~1ull;
    while (active) {
#if defined(__GNUC__) || defined(__clang__)
        int position = __builtin_ctzll(active);
#else
        int position = 0;
        uint64_t probe = active;
        while (!(probe & 1u)) { position++; probe >>= 1; }
#endif
        active &= active - 1;
        int run = position - next_position;
        while (run > 15) {
            model = (INTRA_RATE_DC_MODELS + rate_band(next_position) *
                INTRA_RATE_ACTIVITY_BUCKETS +
                (previous_ac_category >= 3 || ((neighbor_mask >> next_position) & 1u)));
            bits += rate_model->bits[model][0xf0];
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        category = coefficient_category(unit[position]);
        model = (INTRA_RATE_DC_MODELS + rate_band(next_position) *
                INTRA_RATE_ACTIVITY_BUCKETS +
                (previous_ac_category >= 3 || ((neighbor_mask >> next_position) & 1u)));
        bits += rate_model->bits[model][(run << 4) | category] + category;
        next_position = position + 1;
        previous_ac_category = category;
    }
    if (next_position < length) {
        model = (INTRA_RATE_DC_MODELS + rate_band(next_position) *
                INTRA_RATE_ACTIVITY_BUCKETS +
                (previous_ac_category >= 3 || ((neighbor_mask >> next_position) & 1u)));
        bits += rate_model->bits[model][0];
    }
    return bits;
}

static int choose_quantized_mode4(
    const Plane *source, const Plane *reconstructed, int x0, int y0,
    uint8_t neutral, int mode_count, int causal_right, const IntraQuant4 *quant,
    const IntraRateModel *rate_model, const N148CoeffPlane *coefficient_plane,
    const uint8_t *committed_modes, long block, int blocks_x, int previous_dc_category,
    int part, const IntraBlockCandidate *committed, uint8_t prediction[16],
    short best_coefficients[16], float best_ideal[16]) {
    N148PreparedSquare references;
    prepare_square_references(reconstructed, x0, y0, 4, neutral,
                              causal_right, fidelity_tools, 0, &references);
    if (mode_count > 4) mode_count = 4;
    uint8_t extra_prediction[6][16];
    int first_extra = -1, second_extra = -1;
    uint64_t first_sse = UINT64_MAX, second_sse = UINT64_MAX;
    for (int mode = 4; mode < mode_count; mode++) {
        predict_square_prepared(&references, 4, mode, neutral, extra_prediction[mode-4]);
        uint64_t score = prediction_sse(source, x0, y0, 4,
                                       extra_prediction[mode-4], second_sse);
        if (score < first_sse) {
            second_sse = first_sse; second_extra = first_extra;
            first_sse = score; first_extra = mode;
        } else if (score < second_sse) {
            second_sse = score; second_extra = mode;
        }
    }
    short values[64];
    uint8_t modes[N148_INTRA_MODE_SLOTS_PER_BLOCK];
    uint64_t neighbor_mask = coefficient_neighbor_mask(coefficient_plane, block, blocks_x) >> (part*16);
    memcpy(values, committed->coefficients, sizeof(values));
    memcpy(modes, committed->modes, sizeof(modes));
    double best_cost = DBL_MAX;
    int best_mode = -1;
    double lambda = fidelity_rdo_lambda(quant->dc_step, 1);
    for (int mode = 0; mode < mode_count; mode++) {
        if (mode >= 4 && mode != first_extra && mode != second_extra) continue;
        modes[part] = (uint8_t)mode;
        double mode_rate = estimate_mode_bits_from(rate_model, 0, committed_modes,
                                                   block, modes, part, part+1, blocks_x, 0.0);
        if (lambda * mode_rate >= best_cost) continue;
        uint8_t trial[16];
        short coefficients[16];
        float ideal[16];
        if (mode >= 4) memcpy(trial, extra_prediction[mode-4], 16);
        else predict_square_prepared(&references, 4, mode, neutral, trial);
        if (!quantize4(source, x0, y0, trial, quant, coefficients, ideal)) return -1;
        double distortion = quantized_error(ideal, coefficients,
            quant->unit_energy, 16, lambda * mode_rate, best_cost);
        if (distortion + lambda * mode_rate >= best_cost) continue;
        memcpy(values + part*16, coefficients, sizeof(coefficients));
        double rate = cached_coefficient4_bits(rate_model, neighbor_mask, previous_dc_category, coefficients) + mode_rate;
        double cost = distortion + lambda * rate;
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = mode;
            memcpy(prediction, trial, 16);
            memcpy(best_coefficients, coefficients, sizeof(coefficients));
            memcpy(best_ideal, ideal, sizeof(ideal));
        }
    }
    return best_mode;
}


static int make_split_rdo_candidate(
    const Plane *source, Plane *reconstructed, int x0, int y0,
    uint8_t neutral, int mode_count, int causal_right,
    const IntraQuant4 *quant, int effort,
    int trellis_policy,
    const IntraRateModel *rate_model, const N148CoeffPlane *coefficient_plane,
    const uint8_t *committed_modes, long block, int blocks_x,
    int plane_class, int previous_dc_category,
    const N148PartitionMap *partition_map, size_t map_index,
    double competing_cost, double decision_lambda,
    IntraBlockCandidate *candidate) {
    uint8_t original[64], committed[64];
    copy_block_from_plane(reconstructed, x0, y0, original);
    memcpy(committed, original, sizeof(committed));
    memset(candidate->coefficients, 0, sizeof(candidate->coefficients));
    memset(candidate->modes, 0, sizeof(candidate->modes));
    /* Later 4x4 trials leave the already committed regions unchanged. */
    uint64_t committed_distortion = 0;
    double committed_coefficient_bits = 0.0;
    double committed_mode_bits = 0.0;
    uint64_t committed_sse = 0;
    int committed_dc_category = previous_dc_category;
    double partition_bits = estimate_partition_bits(
        rate_model, partition_map, map_index, 1);
    double lambda = fidelity_rdo_lambda(
        quant->dc_step, plane_class == 0);
    for (int part = 0; part < 4; part++) {
        int part_x = x0 + (part & 1) * 4;
        int part_y = y0 + (part >> 1) * 4;
        int shortlist_mode = -1;
        uint8_t shortlist_prediction[16];
        short shortlist_coefficients[16];
        float shortlist_ideal[16];
        int quantized_shortlist = effort == 3 && plane_class == 0 &&
            n148_fidelity_detail_reconstruction();
        if (quantized_shortlist) {
            shortlist_mode = choose_quantized_mode4(
                source, reconstructed, part_x, part_y, neutral, mode_count,
                causal_right, quant, rate_model, coefficient_plane,
                committed_modes, block, blocks_x, committed_dc_category,
                part, candidate, shortlist_prediction, shortlist_coefficients, shortlist_ideal);
            if (shortlist_mode < 0) {
                restore_valid_block(reconstructed, x0, y0, original);
                return 0;
            }
        } else if (effort < 5) {
            shortlist_mode = choose_mode(
                source, reconstructed, part_x, part_y, 4, neutral,
                mode_count, causal_right, plane_class == 0,
                shortlist_prediction, NULL, NULL);
        }
        double best_cost = DBL_MAX;
        uint64_t best_part_distortion = 0;
        double best_coefficient_bits = 0.0;
        double best_mode_bits = 0.0;
        int best_dc_category = 0;
        short best_coefficients[16];
        uint8_t best_pixels[64];
        int best_mode = -1;
        for (int mode = 0; mode < mode_count; mode++) {
            if (shortlist_mode >= 0 && mode != shortlist_mode) continue;
            /* Effort 3 evaluates exactly the shortlisted mode. Its trial is
               therefore the committed reconstruction and can stay in place.
               Higher efforts still restore the full block between modes. */
            if (shortlist_mode < 0)
                restore_valid_block(reconstructed, x0, y0, committed);
            uint8_t prediction[16];
            if (shortlist_mode == mode) memcpy(prediction, shortlist_prediction, 16);
            else predict_square(reconstructed, part_x, part_y, 4, mode,
                                neutral, causal_right, plane_class == 0,
                                prediction);
            short coefficients[16];
            float ideal_levels[16];
            if (quantized_shortlist) {
                memcpy(coefficients, shortlist_coefficients, sizeof(coefficients));
                memcpy(ideal_levels, shortlist_ideal, sizeof(ideal_levels));
            } else if (!quantize4(source, part_x, part_y, prediction, quant,
                           coefficients, ideal_levels)) {
                restore_valid_block(reconstructed, x0, y0, original);
                return 0;
            }
            short trial_storage[64];
            short *trial_coefficients;
            if (shortlist_mode >= 0) {
                memcpy(candidate->coefficients + part * 16, coefficients,
                       sizeof(coefficients));
                trial_coefficients = candidate->coefficients;
            } else {
                memcpy(trial_storage, candidate->coefficients,
                       sizeof(trial_storage));
                memcpy(trial_storage + part * 16, coefficients,
                       sizeof(coefficients));
                trial_coefficients = trial_storage;
            }
            int structural_context = effort == 3 &&
                n148_fidelity_detail_reconstruction() &&
                trellis_policy == N148_TRELLIS_OFF;
            if (trellis_policy != N148_TRELLIS_OFF || structural_context) {
                int unit_previous_dc = part == 0 ? previous_dc_category :
                    coefficient_category(
                        candidate->coefficients[(part - 1) * 16]);
                if (!refine_coefficients_contextually(
                        rate_model, coefficient_plane, block, blocks_x,
                        plane_class, unit_previous_dc, ideal_levels,
                        structural_context ? quant->unit_energy : quant->perceptual_energy,
                        part * 16, 16, effort,
                        structural_context ? N148_TRELLIS_LEGACY : trellis_policy,
                        structural_context,
                        trial_coefficients)) {
                    restore_valid_block(reconstructed, x0, y0, original);
                    return 0;
                }
                memcpy(coefficients, trial_coefficients + part * 16,
                       sizeof(coefficients));
            }
            if (!reconstruct4(coefficients, prediction, quant, reconstructed,
                              part_x, part_y)) {
                restore_valid_block(reconstructed, x0, y0, original);
                return 0;
            }
            uint8_t trial_mode_storage[N148_INTRA_MODE_SLOTS_PER_BLOCK];
            uint8_t *trial_modes;
            if (shortlist_mode >= 0) {
                candidate->modes[part] = (uint8_t) mode;
                trial_modes = candidate->modes;
            } else {
                memcpy(trial_mode_storage, candidate->modes,
                       sizeof(trial_mode_storage));
                trial_mode_storage[part] = (uint8_t) mode;
                trial_modes = trial_mode_storage;
            }
            uint64_t part_distortion = fidelity_region_error(
                source, reconstructed, part_x, part_y, 4,
                plane_class == 0);
            uint64_t distortion = committed_distortion + part_distortion;
            double coefficient_bits = committed_coefficient_bits;
            int dc_category = committed_dc_category;
            double selected_coefficient_bits = 0.0;
            int selected_dc_category = 0;
            /* A shortlisted search has exactly one mode at this part.
               Suffix units cannot affect that choice; their rates are
               evaluated after the next part is committed. */
            int last_unit = shortlist_mode >= 0 ? part + 1 : 4;
            for (int unit = part; unit < last_unit; unit++) {
                coefficient_bits += estimate_coefficient_unit_bits(
                    rate_model, coefficient_plane, block, blocks_x,
                    plane_class, dc_category, trial_coefficients,
                    unit * 16, 16, &dc_category);
                if (unit == part) {
                    selected_coefficient_bits = coefficient_bits;
                    selected_dc_category = dc_category;
                }
            }
            double mode_bits = estimate_mode_bits_from(
                rate_model, plane_class, committed_modes, block,
                trial_modes, part, part + 1, blocks_x,
                committed_mode_bits);
            double rate = coefficient_bits + mode_bits + partition_bits;
            double cost = (double) distortion + lambda * rate;
            if (cost < best_cost) {
                best_cost = cost;
                best_part_distortion = part_distortion;
                best_coefficient_bits = selected_coefficient_bits;
                best_mode_bits = mode_bits;
                best_dc_category = selected_dc_category;
                best_mode = mode;
                if (shortlist_mode < 0)
                    memcpy(best_coefficients, coefficients,
                           sizeof(best_coefficients));
                if (shortlist_mode < 0)
                    copy_block_from_plane(reconstructed, x0, y0, best_pixels);
            }
        }
        if (best_mode < 0) {
            restore_valid_block(reconstructed, x0, y0, original);
            return 0;
        }
        candidate->modes[part] = (uint8_t) best_mode;
        if (shortlist_mode < 0)
            memcpy(candidate->coefficients + part * 16, best_coefficients,
                   sizeof(best_coefficients));
        if (shortlist_mode < 0) {
            memcpy(committed, best_pixels, sizeof(committed));
            restore_valid_block(reconstructed, x0, y0, committed);
        }
        committed_distortion += best_part_distortion;
        committed_coefficient_bits = best_coefficient_bits;
        committed_mode_bits = best_mode_bits;
        committed_dc_category = best_dc_category;
        if (competing_cost < DBL_MAX && part < 3) {
            /* The structural penalty and all remaining SSE/rate are
               nonnegative. Completed 4x4 pixels cannot change in later
               trials, so this is a lower bound on the final 8x8 cost. */
            committed_sse += plane_class == 0 &&
                (fidelity_tools & N148_FIDELITY_STRUCTURAL_RDO) ?
                region_error(source, reconstructed, part_x, part_y, 4) :
                best_part_distortion;
            double lower_bound = (double) committed_sse +
                decision_lambda * (partition_bits +
                    committed_coefficient_bits + committed_mode_bits);
            if (lower_bound > competing_cost + 1.0) {
                candidate->proxy_bits = DBL_MAX;
                restore_valid_block(reconstructed, x0, y0, original);
                return 1;
            }
        }
    }
    candidate->distortion = plane_class == 0 &&
        (fidelity_tools & N148_FIDELITY_STRUCTURAL_RDO) ?
        fidelity_region_error(source, reconstructed, x0, y0, 8, 1) :
        committed_distortion;
    candidate->proxy_bits = estimate_coefficient_bits(
        rate_model, coefficient_plane, block, blocks_x, plane_class,
        previous_dc_category, candidate->coefficients, 1) +
        estimate_mode_bits(rate_model, plane_class, committed_modes, block,
                           candidate->modes, 4, blocks_x) +
        estimate_partition_bits(rate_model, partition_map, map_index, 1);
    copy_block_from_plane(reconstructed, x0, y0, candidate->pixels);
    restore_valid_block(reconstructed, x0, y0, original);
    return 1;
}

static int make_unsplit_candidate(
    const Plane *source, Plane *reconstructed, int x0, int y0,
    uint8_t neutral, int mode_count, const IntraQuant8 *quant, int effort,
    int apply_fidelity,
    IntraBlockCandidate *candidate) {
    uint8_t prediction[64];
    float ideal_levels[64];
    int mode = choose_mode(source, reconstructed, x0, y0, 8, neutral,
                                mode_count, -1, apply_fidelity, prediction,
                                NULL, NULL);
    if (!quantize8(source, x0, y0, prediction, quant,
                   candidate->coefficients, ideal_levels)) return 0;
    if (effort >= 3 && !n148_rdo_refine_coefficients_trusted(
            ideal_levels, quant->unit_energy,
            0.85 * quant->unit_energy[0] * BASE_TRELLIS_LAMBDA_SCALE,
            effort, candidate->coefficients, NULL)) return 0;
    for (int slot = 0; slot < N148_INTRA_MODE_SLOTS_PER_BLOCK; slot++)
        candidate->modes[slot] = (uint8_t) mode;
    uint8_t saved[64];
    copy_block_from_plane(reconstructed, x0, y0, saved);
    if (!reconstruct8(candidate->coefficients, prediction, quant,
                      reconstructed, x0, y0)) return 0;
    candidate->distortion = block_error(source, reconstructed, x0, y0);
    candidate->proxy_bits =
        n148_rdo_estimate_block_bits(candidate->coefficients) + 3.0;
    copy_block_from_plane(reconstructed, x0, y0, candidate->pixels);
    restore_valid_block(reconstructed, x0, y0, saved);
    return 1;
}

static int make_split_candidate(
    const Plane *source, Plane *reconstructed, int x0, int y0,
    uint8_t neutral, int mode_count, const IntraQuant4 *quant,
    int apply_fidelity,
    IntraBlockCandidate *candidate) {
    uint8_t saved[64];
    copy_block_from_plane(reconstructed, x0, y0, saved);
    memset(candidate->coefficients, 0, sizeof(candidate->coefficients));
    candidate->proxy_bits = 1.0;
    for (int part = 0; part < 4; part++) {
        int part_x = x0 + (part & 1) * 4;
        int part_y = y0 + (part >> 1) * 4;
        uint8_t prediction[16];
        int mode = choose_mode(source, reconstructed, part_x, part_y, 4,
                                    neutral, mode_count, -1,
                                    apply_fidelity, prediction, NULL, NULL);
        candidate->modes[part] = (uint8_t) mode;
        if (!quantize4(source, part_x, part_y, prediction, quant,
                       candidate->coefficients + part * 16, NULL) ||
            !reconstruct4(candidate->coefficients + part * 16, prediction,
                          quant, reconstructed, part_x, part_y)) {
            restore_valid_block(reconstructed, x0, y0, saved);
            return 0;
        }
        short packed[64] = {0};
        memcpy(packed, candidate->coefficients + part * 16,
               16 * sizeof(*packed));
        candidate->proxy_bits += n148_rdo_estimate_block_bits(packed) + 3.0;
    }
    candidate->distortion = block_error(source, reconstructed, x0, y0);
    copy_block_from_plane(reconstructed, x0, y0, candidate->pixels);
    restore_valid_block(reconstructed, x0, y0, saved);
    return 1;
}

static int choose_split(const IntraBlockCandidate *unsplit,
                        const IntraBlockCandidate *split, int quality) {
    /* Directional intra deliberately uses a coarse rate proxy. Contextual RDO replaces this
       gate and mode selection with contextual rate-distortion costs. */
    double quantizer = quality < 50 ? (double)(101 - quality) :
        (double)(202 - 2 * quality);
    double lambda = 0.14 * quantizer * quantizer;
    double unsplit_cost = (double) unsplit->distortion +
        lambda * unsplit->proxy_bits;
    double split_cost = (double) split->distortion +
        lambda * split->proxy_bits;
    return split_cost < unsplit_cost;
}

static int filter_reconstructed_block(
    Plane *plane, int x0, int y0, int quality, int plane_index,
    int loop_filter_policy, uint8_t segment,
    const N148SegmentationMap *segmentation_map) {
    if (loop_filter_policy == N148_LOOP_FILTER_OFF) return 1;
    uint8_t filter_level = 1;
    if (loop_filter_policy == N148_LOOP_FILTER_LEGACY &&
        plane_index == 0 && segmentation_map &&
        segmentation_map->adaptive_filter &&
        !n148_segmentation_filter_level(
            segmentation_map, segment, &filter_level)) return 0;
    if (loop_filter_policy == N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA)
        return n148_loop_filter_quant_luma_block(
            plane, x0, y0, 8, quality, plane_index, NULL);
    return n148_loop_filter_block(
        plane, x0, y0, 8, quality, plane_index, filter_level, NULL);
}

static int filter_reconstructed_macro(
    Plane *plane, int x0, int y0, int quality, int loop_filter_policy,
    uint8_t segment, const N148SegmentationMap *segmentation_map) {
    if (loop_filter_policy == N148_LOOP_FILTER_OFF) return 1;
    uint8_t filter_level = 1;
    if (loop_filter_policy == N148_LOOP_FILTER_LEGACY && segmentation_map &&
        segmentation_map->adaptive_filter &&
        !n148_segmentation_filter_level(
            segmentation_map, segment, &filter_level)) return 0;
    if (loop_filter_policy == N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA)
        return n148_loop_filter_quant_luma_block(
            plane, x0, y0, 16, quality, 0, NULL);
    return n148_loop_filter_block(
        plane, x0, y0, 16, quality, 0, filter_level, NULL);
}

static int filter_reconstructed_super(
    Plane *plane, int x0, int y0, int quality, int loop_filter_policy,
    uint8_t segment, const N148SegmentationMap *segmentation_map) {
    if (loop_filter_policy == N148_LOOP_FILTER_OFF) return 1;
    uint8_t filter_level = 1;
    if (loop_filter_policy == N148_LOOP_FILTER_LEGACY && segmentation_map &&
        segmentation_map->adaptive_filter &&
        !n148_segmentation_filter_level(
            segmentation_map, segment, &filter_level)) return 0;
    if (loop_filter_policy == N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA)
        return n148_loop_filter_quant_luma_block(
            plane, x0, y0, 32, quality, 0, NULL);
    return n148_loop_filter_block(
        plane, x0, y0, 32, quality, 0, filter_level, NULL);
}

static int prepare_reconstruction_filters(
    int quality, int plane_index, int policy,
    const N148SegmentationMap *segmentation_map,
    N148LoopFilterPrepared filters[N148_SEGMENT_COUNT]) {
    if (policy == N148_LOOP_FILTER_OFF) return 1;
    int levels = segmentation_map ? N148_SEGMENT_COUNT : 1;
    for (int segment = 0; segment < levels; segment++) {
        uint8_t adaptive_level = 1;
        if (policy == N148_LOOP_FILTER_LEGACY && plane_index == 0 &&
            segmentation_map && segmentation_map->adaptive_filter &&
            !n148_segmentation_filter_level(
                segmentation_map, (uint8_t) segment, &adaptive_level))
            return 0;
        if (!n148_loop_filter_prepare(
                quality, plane_index, adaptive_level,
                policy == N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA,
                &filters[segment])) return 0;
    }
    return 1;
}

static int filter_reconstructed_prepared(
    Plane *plane, int x0, int y0, int size, int policy, uint8_t segment,
    const N148LoopFilterPrepared filters[N148_SEGMENT_COUNT]) {
    if (policy == N148_LOOP_FILTER_OFF) return 1;
    return n148_loop_filter_block_prepared(
        plane, x0, y0, size, &filters[segment], NULL);
}

static void accumulate_luma_coefficient(int transform, int mode,
                                        int position, double value) {
    N148LumaStatistics *stats = luma_stats_sink;
    stats->sum[transform][mode][position] += value;
    stats->sum_squares[transform][mode][position] += value * value;
    stats->sum_absolute[transform][mode][position] += fabs(value);
}

/* Replay the selected block against the pre-block causal reconstruction.
   DCT4 needs replay because each sub-block predicts from the earlier selected
   sub-blocks. The caller subsequently installs the already saved candidate,
   so this diagnostic path cannot perturb the normative reconstruction. */
static int collect_luma_block_statistics(
    const Plane *source, Plane *reconstructed, int x0, int y0,
    int use_split, const IntraBlockCandidate *chosen,
    const IntraQuant4 *quant4) {
    if (!luma_stats_sink) return 1;
    if (!source || !reconstructed || !chosen || !quant4) return 0;
    if (!use_split) {
        int mode = chosen->modes[0];
        int base_mode = n148_intra_base_mode(mode);
        uint8_t prediction[64];
        float block[64], transformed[64];
        predict_square(reconstructed, x0, y0, 8, mode, 128, -1, 1,
                       prediction);
        for (int row = 0; row < 8; row++) {
            for (int column = 0; column < 8; column++) {
                int position = row * 8 + column;
                block[position] = (float)(sample_clamped(
                    source, x0 + column, y0 + row) -
                    prediction[position] + 128);
            }
        }
        dct_block_fast(block, transformed);
        luma_stats_sink->block_count[N148_LUMA_STATS_DCT8][base_mode]++;
        for (int natural = 0; natural < 64; natural++) {
            double value = transformed[natural] /
                aan_scale_factor(natural / 8, natural % 8);
            accumulate_luma_coefficient(
                N148_LUMA_STATS_DCT8, base_mode, natural, value);
        }
        return 1;
    }

    for (int part = 0; part < 4; part++) {
        int mode = chosen->modes[part];
        int base_mode = n148_intra_base_mode(mode);
        int part_x = x0 + (part & 1) * 4;
        int part_y = y0 + (part >> 1) * 4;
        uint8_t prediction[16];
        int32_t residual[16], transformed[16];
        predict_square(reconstructed, part_x, part_y, 4, mode, 128, -1, 1,
                       prediction);
        for (int row = 0; row < 4; row++) {
            for (int column = 0; column < 4; column++) {
                int position = row * 4 + column;
                residual[position] = sample_clamped(
                    source, part_x + column, part_y + row) -
                    prediction[position];
            }
        }
        if (!n148_variable_forward(residual, 4, transformed)) return 0;
        luma_stats_sink->block_count[N148_LUMA_STATS_DCT4][base_mode]++;
        for (int natural = 0; natural < 16; natural++)
            accumulate_luma_coefficient(
                N148_LUMA_STATS_DCT4, base_mode, natural,
                (double) transformed[natural]);
        if (!reconstruct4(chosen->coefficients + part * 16, prediction,
                          quant4, reconstructed, part_x, part_y)) return 0;
    }
    return 1;
}


static int quantize_plane(
    const Plane *source, int quality, int plane_index, int extended_modes,
    int split_4x4, int effort, int trellis_policy, int loop_filter,
    int calibrated_quant, int subsample_x, int subsample_y,
    const IntraRateModel *rate_model,
    N148PartitionMap *partition_map, N148CoeffPlane *coefficients,
    uint8_t *modes, const N148SegmentationMap *segmentation_map, Plane *capture) {
    size_t count;
    if (!source || !source->data || !partition_map || !modes ||
        plane_index < 0 || plane_index > 2 || quality < 1 || quality > 100 ||
        (calibrated_quant != 0 && calibrated_quant != 1) ||
        (extended_modes != 0 && extended_modes != 1) ||
        (split_4x4 != 0 && split_4x4 != 1) ||
        trellis_policy < N148_TRELLIS_OFF ||
        trellis_policy > N148_TRELLIS_PERCEPTUAL_LUMA ||
        (subsample_x != 0 && subsample_x != 1) ||
        (subsample_y != 0 && subsample_y != 1) ||
        effort < 0 || effort > 9 ||
        loop_filter < N148_LOOP_FILTER_OFF ||
        loop_filter > N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA ||
        (segmentation_map &&
         !n148_segmentation_map_validate(segmentation_map)) ||
        !checked_block_count(source->width, source->height, &count) ||
        !allocate_coefficients(count, coefficients)) return 0;
    int blocks_x = source->width / 8 + (source->width % 8 != 0);
    int blocks_y = source->height / 8 + (source->height % 8 != 0);
    if (partition_map->blocks_x[plane_index] != blocks_x ||
        partition_map->blocks_y[plane_index] != blocks_y) {
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    Plane reconstructed = create_plane(source->width, source->height);
    if (!reconstructed.data) {
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    IntraQuant8 quant8[N148_SEGMENT_COUNT];
    IntraQuant4 quant4[N148_SEGMENT_COUNT];
    const int (*base)[8] = plane_index ?
        (calibrated_quant ? FORMAT_4_CHROMA_QUANT_BASE : Q_CHROMA_BASE) :
        luma_quant_base(calibrated_quant);
    if (!build_segment_quantizers(base, quality, plane_index,
                                  segmentation_map, quant8, quant4)) {
        free_plane(&reconstructed);
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    uint8_t neutral = 128;
    int unsplit_mode_count = plane_index == 0 && rate_model ?
        luma_mode_count(extended_modes, 8, effort) :
        (extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4);
    int split_mode_count = extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    int can_split = split_4x4 && plane_index == 0;
    int success = 1;
    int previous_dc_category = 0;
    for (int block_y = 0; block_y < blocks_y && success; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            long block = (long) block_y * blocks_x + block_x;
            int x0 = block_x * 8;
            int y0 = block_y * 8;
            uint8_t segment = 0;
            if (segmentation_map && !segmentation_level_prevalidated(
                    segmentation_map, block_x, block_y,
                    subsample_x, subsample_y, &segment)) {
                success = 0;
                break;
            }
            int quant_level = plane_index == 0 ? segment : 0;
            const IntraQuant8 *block_quant8 = &quant8[quant_level];
            const IntraQuant4 *block_quant4 = &quant4[quant_level];
            IntraBlockCandidate unsplit, split;
            memset(&unsplit, 0, sizeof(unsplit));
            memset(&split, 0, sizeof(split));
            size_t map_index = partition_map->plane_offsets[plane_index] +
                (size_t) block;
            int candidate_ok = rate_model ? make_unsplit_rdo_candidate(
                source, &reconstructed, x0, y0, neutral,
                unsplit_mode_count, -1, block_quant8,
                effort, trellis_policy, rate_model, coefficients, modes,
                block, blocks_x,
                plane_index == 0 ? 0 : 1, previous_dc_category,
                partition_map, map_index, &unsplit) :
                make_unsplit_candidate(source, &reconstructed, x0, y0,
                                       neutral, unsplit_mode_count,
                                       block_quant8,
                                       effort, plane_index == 0,
                                       &unsplit);
            if (!candidate_ok) {
                success = 0;
                break;
            }
            int use_split = 0;
            if (can_split) {
                candidate_ok = rate_model ? make_split_rdo_candidate(
                    source, &reconstructed, x0, y0, neutral,
                    split_mode_count, -1, block_quant4, effort,
                    trellis_policy,
                    rate_model, coefficients, modes, block,
                    blocks_x, plane_index == 0 ? 0 : 1,
                    previous_dc_category, partition_map, map_index,
                    DBL_MAX, 0.0, &split) :
                    make_split_candidate(source, &reconstructed, x0, y0,
                                         neutral, split_mode_count,
                                         block_quant4,
                                         plane_index == 0,
                                         &split);
                if (!candidate_ok) {
                    success = 0;
                    break;
                }
                if (rate_model) {
                    double lambda = fidelity_rdo_lambda(
                        block_quant8->dc_step, plane_index == 0);
                    use_split = (double) split.distortion +
                        lambda * split.proxy_bits <
                        (double) unsplit.distortion +
                        lambda * unsplit.proxy_bits;
                } else {
                    use_split = choose_split(&unsplit, &split, quality);
                }
            }
            const IntraBlockCandidate *chosen = use_split ? &split : &unsplit;
            if (plane_index == 0 && rate_model && luma_stats_sink &&
                !collect_luma_block_statistics(
                    source, &reconstructed, x0, y0, use_split, chosen,
                    block_quant4)) {
                success = 0;
                break;
            }
            memcpy(coefficients->coefficients + block * 64,
                   chosen->coefficients, sizeof(chosen->coefficients));
            memcpy(modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                   chosen->modes, sizeof(chosen->modes));
            coefficients->nonzero_masks[block] =
                coefficient_mask(chosen->coefficients);
            restore_valid_block(&reconstructed, x0, y0, chosen->pixels);
            partition_map->split[map_index] = (uint8_t) use_split;
            previous_dc_category =
                coefficient_category(chosen->coefficients[
                    use_split ? 3 * 16 : 0]);
            if (!filter_reconstructed_block(
                    &reconstructed, x0, y0, quality, plane_index,
                    loop_filter, segment, segmentation_map)) {
                success = 0;
                break;
            }
        }
    }
    if (success && capture) {
        *capture = reconstructed;
        reconstructed = (Plane){0};
    }
    free_plane(&reconstructed);
    if (!success) n148_free_coeff_plane(coefficients);
    return success;
}

static double estimate_transform_bits(
    const IntraRateModel *rate_model, const N148TransformMap *transform_map,
    size_t map_index, uint8_t strategy) {
    uint8_t prediction = n148_transform_map_predict(
        transform_map, map_index);
    int symbol = (strategy + N148_TRANSFORM_STRATEGY_COUNT - prediction) %
        N148_TRANSFORM_STRATEGY_COUNT;
    return rate_model->bits[INTRA_RATE_TRANSFORM_MODEL][symbol];
}

static void clear_macro_syntax(
    int first_block_x, int first_block_y, int blocks_x, int blocks_y,
    N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map) {
    for (int cell_y = 0; cell_y < 2; cell_y++) {
        int block_y = first_block_y + cell_y;
        if (block_y >= blocks_y) continue;
        for (int cell_x = 0; cell_x < 2; cell_x++) {
            int block_x = first_block_x + cell_x;
            if (block_x >= blocks_x) continue;
            long block = (long) block_y * blocks_x + block_x;
            memset(coefficients->coefficients + block * 64, 0,
                   64 * sizeof(*coefficients->coefficients));
            coefficients->nonzero_masks[block] = 0;
            memset(modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK, 0,
                   N148_INTRA_MODE_SLOTS_PER_BLOCK);
            partition_map->split[block] = 0;
        }
    }
}

static void install_macro_candidate(
    const IntraMacroCandidate *candidate,
    int first_block_x, int first_block_y, int blocks_x, int blocks_y,
    N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map, Plane *reconstructed) {
    for (int cell_y = 0; cell_y < 2; cell_y++) {
        int block_y = first_block_y + cell_y;
        if (block_y >= blocks_y) continue;
        for (int cell_x = 0; cell_x < 2; cell_x++) {
            int block_x = first_block_x + cell_x;
            if (block_x >= blocks_x) continue;
            int cell = cell_y * 2 + cell_x;
            long block = (long) block_y * blocks_x + block_x;
            memcpy(coefficients->coefficients + block * 64,
                   candidate->coefficients[cell],
                   sizeof(candidate->coefficients[cell]));
            coefficients->nonzero_masks[block] =
                coefficient_mask(candidate->coefficients[cell]);
            memcpy(modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                   candidate->modes[cell], sizeof(candidate->modes[cell]));
            partition_map->split[block] = candidate->splits[cell];
        }
    }
    restore_macro_snapshot(reconstructed, &candidate->reconstruction);
}

static void clear_super_syntax(
    int first_block_x, int first_block_y, int blocks_x, int blocks_y,
    N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map) {
    for (int cell_y = 0; cell_y < 4; cell_y++) {
        int block_y = first_block_y + cell_y;
        if (block_y >= blocks_y) continue;
        for (int cell_x = 0; cell_x < 4; cell_x++) {
            int block_x = first_block_x + cell_x;
            if (block_x >= blocks_x) continue;
            long block = (long) block_y * blocks_x + block_x;
            memset(coefficients->coefficients + block * 64, 0,
                   64 * sizeof(*coefficients->coefficients));
            coefficients->nonzero_masks[block] = 0;
            memset(modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK, 0,
                   N148_INTRA_MODE_SLOTS_PER_BLOCK);
            partition_map->split[block] = 0;
        }
    }
}

static void install_super_candidate(
    const IntraSuperCandidate *candidate, int first_macro_x, int first_macro_y,
    int blocks_x, N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map, N148TransformMap *transform_map,
    Plane *reconstructed) {
    int first_block_x = first_macro_x * 2;
    int first_block_y = first_macro_y * 2;
    for (int cell = 0; cell < 16; cell++) {
        int block_x = first_block_x + (cell & 3);
        int block_y = first_block_y + (cell >> 2);
        long block = (long) block_y * blocks_x + block_x;
        memcpy(coefficients->coefficients + block * 64,
               candidate->coefficients[cell],
               sizeof(candidate->coefficients[cell]));
        coefficients->nonzero_masks[block] =
            coefficient_mask(candidate->coefficients[cell]);
        memcpy(modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK,
               candidate->modes[cell], sizeof(candidate->modes[cell]));
        partition_map->split[block] = candidate->splits[cell];
    }
    for (int macro = 0; macro < 4; macro++) {
        size_t index = (size_t)(first_macro_y + (macro >> 1)) *
            (size_t) transform_map->columns[0] +
            (size_t)(first_macro_x + (macro & 1));
        transform_map->strategies[index] = candidate->strategies[macro];
    }
    restore_super_snapshot(reconstructed, &candidate->reconstruction);
}

static int make_small_macro_candidate(
    const Plane *source, Plane *reconstructed, int quality,
    int extended_modes, int split_4x4, int effort, int trellis_policy,
    int loop_filter, const IntraQuant8 quant8[N148_SEGMENT_COUNT],
    const IntraQuant4 quant4[N148_SEGMENT_COUNT],
    const IntraRateModel *rate_model, int first_block_x, int first_block_y,
    int blocks_x, int blocks_y, int previous_dc_category,
    N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    const N148TransformMap *transform_map, size_t transform_index,
    IntraMacroCandidate *candidate) {
    IntraMacroSnapshot original;
    take_macro_snapshot(reconstructed, first_block_x * 8,
                        first_block_y * 8, &original);
    memset(candidate, 0, sizeof(*candidate));
    int unsplit_mode_count = luma_mode_count(extended_modes, 8, effort);
    int split_mode_count = extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    int local_dc_category = previous_dc_category;
    int success = 1;
    for (int cell_y = 0; cell_y < 2 && success; cell_y++) {
        int block_y = first_block_y + cell_y;
        if (block_y >= blocks_y) continue;
        for (int cell_x = 0; cell_x < 2; cell_x++) {
            int block_x = first_block_x + cell_x;
            if (block_x >= blocks_x) continue;
            int cell = cell_y * 2 + cell_x;
            long block = (long) block_y * blocks_x + block_x;
            int x0 = block_x * 8;
            int y0 = block_y * 8;
            uint8_t segment = 0;
            if (segmentation_map && !segmentation_level_prevalidated(
                    segmentation_map, block_x, block_y, 0, 0, &segment)) {
                success = 0;
                break;
            }
            size_t map_index = (size_t) block;
            IntraBlockCandidate unsplit = {0}, split = {0};
            if (!make_unsplit_rdo_candidate(
                    source, reconstructed, x0, y0, 128, unsplit_mode_count,
                    first_block_x * 8 + 15, &quant8[segment], effort,
                    trellis_policy,
                    rate_model, coefficients, modes, block, blocks_x, 0,
                    local_dc_category, partition_map, map_index, &unsplit)) {
                success = 0;
                break;
            }
            int use_split = 0;
            if (split_4x4) {
                if (!make_split_rdo_candidate(
                        source, reconstructed, x0, y0, 128, split_mode_count,
                        first_block_x * 8 + 15, &quant4[segment], effort,
                        trellis_policy,
                        rate_model, coefficients, modes, block, blocks_x, 0,
                        local_dc_category, partition_map, map_index,
                        (double) unsplit.distortion +
                            fidelity_rdo_lambda(quant8[segment].dc_step, 1) *
                            unsplit.proxy_bits,
                        fidelity_rdo_lambda(quant8[segment].dc_step, 1),
                        &split)) {
                    success = 0;
                    break;
                }
                double lambda = fidelity_rdo_lambda(
                    quant8[segment].dc_step, 1);
                use_split = (double) split.distortion +
                    lambda * split.proxy_bits <
                    (double) unsplit.distortion +
                    lambda * unsplit.proxy_bits;
            }
            const IntraBlockCandidate *chosen = use_split ? &split : &unsplit;
            memcpy(coefficients->coefficients + block * 64,
                   chosen->coefficients, sizeof(chosen->coefficients));
            coefficients->nonzero_masks[block] =
                coefficient_mask(chosen->coefficients);
            memcpy(modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                   chosen->modes, sizeof(chosen->modes));
            partition_map->split[map_index] = (uint8_t) use_split;
            restore_valid_block(reconstructed, x0, y0, chosen->pixels);
            if (!filter_reconstructed_block(
                    reconstructed, x0, y0, quality, 0, loop_filter,
                    segment, segmentation_map)) {
                success = 0;
                break;
            }
            memcpy(candidate->coefficients[cell], chosen->coefficients,
                   sizeof(candidate->coefficients[cell]));
            memcpy(candidate->modes[cell], chosen->modes,
                   sizeof(candidate->modes[cell]));
            candidate->splits[cell] = (uint8_t) use_split;
            candidate->proxy_bits += chosen->proxy_bits;
            local_dc_category = coefficient_category(chosen->coefficients[
                use_split ? 3 * 16 : 0]);
        }
    }
    if (success) {
        candidate->distortion = fidelity_region_error(
            source, reconstructed, first_block_x * 8,
            first_block_y * 8, 16, 1);
        candidate->proxy_bits += estimate_transform_bits(
            rate_model, transform_map, transform_index,
            N148_TRANSFORM_DCT8);
        candidate->ending_dc_category = local_dc_category;
        take_macro_snapshot(reconstructed, first_block_x * 8,
                            first_block_y * 8,
                            &candidate->reconstruction);
    }
    clear_macro_syntax(first_block_x, first_block_y, blocks_x, blocks_y,
                       coefficients, modes, partition_map);
    restore_macro_snapshot(reconstructed, &original);
    return success;
}

static int make_large_macro_candidate(
    const Plane *source, Plane *reconstructed, int quality,
    int extended_modes, int effort, int loop_filter,
    const IntraQuant16 *quant, const IntraRateModel *rate_model,
    int first_block_x, int first_block_y, int blocks_x,
    int previous_dc_category, N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    const N148TransformMap *transform_map, size_t transform_index,
    uint8_t segment, double competing_cost,
    IntraMacroCandidate *candidate) {
    IntraMacroSnapshot original;
    int x0 = first_block_x * 8;
    int y0 = first_block_y * 8;
    take_macro_snapshot(reconstructed, x0, y0, &original);
    memset(candidate, 0, sizeof(*candidate));
    int mode_count = luma_mode_count(extended_modes, 16, effort);
    int shortlist_mode = -1;
    uint8_t shortlist_prediction[16 * 16];
    if (effort < 5)
        shortlist_mode = choose_mode(source, reconstructed, x0, y0, 16,
                                     128, mode_count, -1,
                                     1,
                                     shortlist_prediction, NULL, NULL);
    double best_cost = DBL_MAX;
    uint64_t nearest_reference_distortion = UINT64_MAX;
    int pruned = 0;
    int tried_mode = 0;
    for (int mode = 0; mode < mode_count; mode++) {
        if (shortlist_mode >= 0 && mode != shortlist_mode) continue;
        if (tried_mode) restore_macro_snapshot(reconstructed, &original);
        tried_mode = 1;
        uint8_t prediction[16 * 16];
        if (shortlist_mode == mode)
            memcpy(prediction, shortlist_prediction, sizeof(prediction));
        else
            predict_square(reconstructed, x0, y0, 16, mode, 128,
                           -1, 1, prediction);
        short sequence[16 * 16];
        if (!quantize16(source, x0, y0, prediction, quant, sequence)) {
            restore_macro_snapshot(reconstructed, &original);
            return 0;
        }
        /* The rate estimator reads the local sequence and candidate mode;
           it only consults committed modes outside this macroblock. Keep
           trial syntax local until install_macro_candidate() commits it. */
        double rate = estimate_transform_bits(
            rate_model, transform_map, transform_index,
            N148_TRANSFORM_DCT16);
        rate += estimate_large_coefficient_bits(
            rate_model, 0, previous_dc_category, sequence, 16 * 16);
        long first_block = (long) first_block_y * blocks_x + first_block_x;
        uint8_t large_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK];
        for (int part = 0; part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
            large_modes[part] = (uint8_t) mode;
        rate += estimate_mode_bits(
            rate_model, 0, modes, first_block, large_modes, 1, blocks_x);
        double lambda = fidelity_rdo_lambda(quant->dc_step, 1);
        /* Distortion cannot be negative, so the rate term alone can reject
           a 16x16 candidate that cannot beat the current best. */
        if ((effort == 3 || effort == 4) &&
            lambda * rate >= competing_cost) {
            pruned = 1;
            continue;
        }
        if (!reconstruct16(sequence, prediction, quant, reconstructed,
                           x0, y0, 0, NULL) ||
            !filter_reconstructed_macro(reconstructed, x0, y0, quality,
                                        loop_filter, segment,
                                        segmentation_map)) {
            restore_macro_snapshot(reconstructed, &original);
            return 0;
        }
        int local_dc_category = coefficient_category(sequence[0]);
        uint64_t distortion = fidelity_region_error(
            source, reconstructed, x0, y0, 16, 1);
        if (mode < N148_DIRECTIONAL_INTRA_MODE_COUNT) {
            if (distortion < nearest_reference_distortion)
                nearest_reference_distortion = distortion;
        } else if (distortion > nearest_reference_distortion) {
            continue;
        }
        double cost = (double) distortion + lambda * rate;
        if (cost < best_cost) {
            best_cost = cost;
            for (int cell = 0; cell < 4; cell++) {
                memcpy(candidate->coefficients[cell],
                       sequence + cell * 64,
                       sizeof(candidate->coefficients[cell]));
                for (int part = 0;
                     part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                    candidate->modes[cell][part] = (uint8_t) mode;
            }
            candidate->distortion = distortion;
            candidate->proxy_bits = rate;
            candidate->ending_dc_category = local_dc_category;
            take_macro_snapshot(reconstructed, x0, y0,
                                &candidate->reconstruction);
        }
    }
    restore_macro_snapshot(reconstructed, &original);
    if (best_cost == DBL_MAX && pruned) {
        candidate->proxy_bits = DBL_MAX;
        return 1;
    }
    return best_cost < DBL_MAX;
}

static int make_baseline_super_candidate(
    const Plane *source, Plane *reconstructed, int quality,
    int extended_modes, int split_4x4, int effort, int trellis_policy,
    int loop_filter, const IntraQuant8 quant8[N148_SEGMENT_COUNT],
    const IntraQuant4 quant4[N148_SEGMENT_COUNT],
    const IntraQuant16 quant16[N148_SEGMENT_COUNT],
    const IntraRateModel *rate_model, int first_macro_x, int first_macro_y,
    int blocks_x, int blocks_y, int previous_dc_category,
    N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    N148TransformMap *transform_map, IntraSuperCandidate *candidate) {
    int first_block_x = first_macro_x * 2;
    int first_block_y = first_macro_y * 2;
    IntraSuperSnapshot original;
    uint8_t original_strategies[4];
    take_super_snapshot(reconstructed, first_block_x * 8,
                        first_block_y * 8, &original);
    for (int macro = 0; macro < 4; macro++) {
        size_t index = (size_t)(first_macro_y + (macro >> 1)) *
            (size_t) transform_map->columns[0] +
            (size_t)(first_macro_x + (macro & 1));
        original_strategies[macro] = transform_map->strategies[index];
    }
    memset(candidate, 0, sizeof(*candidate));
    int local_dc_category = previous_dc_category;
    int success = 1;
    for (int macro_y = 0; macro_y < 2 && success; macro_y++) {
        for (int macro_x = 0; macro_x < 2; macro_x++) {
            int current_macro_x = first_macro_x + macro_x;
            int current_macro_y = first_macro_y + macro_y;
            int block_x = current_macro_x * 2;
            int block_y = current_macro_y * 2;
            size_t transform_index = (size_t) current_macro_y *
                (size_t) transform_map->columns[0] +
                (size_t) current_macro_x;
            IntraMacroCandidate small, large;
            if (!make_small_macro_candidate(
                    source, reconstructed, quality, extended_modes,
                    split_4x4, effort, trellis_policy, loop_filter,
                    quant8, quant4, rate_model, block_x, block_y,
                    blocks_x, blocks_y, local_dc_category, coefficients,
                    modes, partition_map, segmentation_map, transform_map,
                    transform_index, &small)) {
                success = 0;
                break;
            }
            uint8_t segment = 0;
            if (segmentation_map && !segmentation_level_prevalidated(
                    segmentation_map, block_x, block_y, 0, 0, &segment)) {
                success = 0;
                break;
            }
            double lambda = fidelity_rdo_lambda(
                quant16[segment].dc_step, 1);
            double small_cost = (double) small.distortion +
                lambda * small.proxy_bits;
            if (!make_large_macro_candidate(
                    source, reconstructed, quality, extended_modes, effort,
                    loop_filter, &quant16[segment], rate_model, block_x,
                    block_y, blocks_x, local_dc_category, coefficients,
                    modes, partition_map, segmentation_map, transform_map,
                    transform_index, segment, small_cost, &large)) {
                success = 0;
                break;
            }
            int use_large = (double) large.distortion +
                lambda * large.proxy_bits < small_cost;
            const IntraMacroCandidate *chosen = use_large ? &large : &small;
            transform_map->strategies[transform_index] = use_large ?
                N148_TRANSFORM_DCT16 : N148_TRANSFORM_DCT8;
            install_macro_candidate(
                chosen, block_x, block_y, blocks_x, blocks_y,
                coefficients, modes, partition_map, reconstructed);
            candidate->proxy_bits += chosen->proxy_bits;
            local_dc_category = chosen->ending_dc_category;
        }
    }
    if (success) {
        for (int cell = 0; cell < 16; cell++) {
            int block_x = first_block_x + (cell & 3);
            int block_y = first_block_y + (cell >> 2);
            long block = (long) block_y * blocks_x + block_x;
            memcpy(candidate->coefficients[cell],
                   coefficients->coefficients + block * 64,
                   sizeof(candidate->coefficients[cell]));
            memcpy(candidate->modes[cell],
                   modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                   sizeof(candidate->modes[cell]));
            candidate->splits[cell] = partition_map->split[block];
        }
        for (int macro = 0; macro < 4; macro++) {
            size_t index = (size_t)(first_macro_y + (macro >> 1)) *
                (size_t) transform_map->columns[0] +
                (size_t)(first_macro_x + (macro & 1));
            candidate->strategies[macro] =
                transform_map->strategies[index];
        }
        candidate->distortion = fidelity_region_error(
            source, reconstructed, first_block_x * 8,
            first_block_y * 8, 32, 1);
        candidate->ending_dc_category = local_dc_category;
        take_super_snapshot(reconstructed, first_block_x * 8,
                            first_block_y * 8,
                            &candidate->reconstruction);
    }
    clear_super_syntax(first_block_x, first_block_y, blocks_x, blocks_y,
                       coefficients, modes, partition_map);
    for (int macro = 0; macro < 4; macro++) {
        size_t index = (size_t)(first_macro_y + (macro >> 1)) *
            (size_t) transform_map->columns[0] +
            (size_t)(first_macro_x + (macro & 1));
        transform_map->strategies[index] = original_strategies[macro];
    }
    restore_super_snapshot(reconstructed, &original);
    return success;
}

static int make_dct32_super_candidate(
    const Plane *source, Plane *reconstructed, int quality,
    int extended_modes, int effort, int loop_filter,
    const IntraQuant32 *quant, const IntraRateModel *rate_model,
    int first_macro_x, int first_macro_y, int blocks_x, int blocks_y,
    int previous_dc_category, N148CoeffPlane *coefficients, uint8_t *modes,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    N148TransformMap *transform_map, uint8_t segment,
    IntraSuperCandidate *candidate) {
    int first_block_x = first_macro_x * 2;
    int first_block_y = first_macro_y * 2;
    int x0 = first_block_x * 8;
    int y0 = first_block_y * 8;
    if (first_block_x + 3 >= blocks_x || first_block_y + 3 >= blocks_y)
        return 0;
    IntraSuperSnapshot original;
    uint8_t original_strategies[4];
    take_super_snapshot(reconstructed, x0, y0, &original);
    for (int macro = 0; macro < 4; macro++) {
        size_t index = (size_t)(first_macro_y + (macro >> 1)) *
            (size_t) transform_map->columns[0] +
            (size_t)(first_macro_x + (macro & 1));
        original_strategies[macro] = transform_map->strategies[index];
        transform_map->strategies[index] = N148_TRANSFORM_DCT4;
    }
    clear_super_syntax(first_block_x, first_block_y, blocks_x, blocks_y,
                       coefficients, modes, partition_map);
    memset(candidate, 0, sizeof(*candidate));
    for (int macro = 0; macro < 4; macro++)
        candidate->strategies[macro] = N148_TRANSFORM_DCT4;
    int mode_count = luma_mode_count(extended_modes, 32, effort);
    int shortlist_mode = -1;
    uint8_t shortlist_prediction[32 * 32];
    if (effort < 5)
        shortlist_mode = choose_mode(source, reconstructed, x0, y0, 32,
                                     128, mode_count, -1, 1,
                                     shortlist_prediction, NULL, NULL);
    double best_cost = DBL_MAX;
    uint64_t nearest_reference_distortion = UINT64_MAX;
    for (int mode = 0; mode < mode_count; mode++) {
        if (shortlist_mode >= 0 && mode != shortlist_mode) continue;
        restore_super_snapshot(reconstructed, &original);
        clear_super_syntax(first_block_x, first_block_y, blocks_x, blocks_y,
                           coefficients, modes, partition_map);
        uint8_t prediction[32 * 32];
        if (shortlist_mode == mode)
            memcpy(prediction, shortlist_prediction, sizeof(prediction));
        else
            predict_square(reconstructed, x0, y0, 32, mode, 128,
                           -1, 1, prediction);
        short sequence[32 * 32];
        if (!quantize32(source, x0, y0, prediction, quant, sequence)) {
            restore_super_snapshot(reconstructed, &original);
            return 0;
        }
        for (int cell = 0; cell < 16; cell++) {
            int block_x = first_block_x + (cell & 3);
            int block_y = first_block_y + (cell >> 2);
            long block = (long) block_y * blocks_x + block_x;
            memcpy(coefficients->coefficients + block * 64,
                   sequence + cell * 64, 64 * sizeof(*sequence));
            coefficients->nonzero_masks[block] =
                coefficient_mask(sequence + cell * 64);
            for (int part = 0;
                 part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part] =
                    (uint8_t) mode;
            partition_map->split[block] = 0;
        }
        if (!reconstruct32(sequence, prediction, quant, reconstructed,
                           x0, y0) ||
            !filter_reconstructed_super(reconstructed, x0, y0, quality,
                                        loop_filter, segment,
                                        segmentation_map)) {
            restore_super_snapshot(reconstructed, &original);
            return 0;
        }
        double rate = estimate_large_coefficient_bits(
            rate_model, 0, previous_dc_category, sequence, 32 * 32);
        for (int macro = 0; macro < 4; macro++) {
            size_t index = (size_t)(first_macro_y + (macro >> 1)) *
                (size_t) transform_map->columns[0] +
                (size_t)(first_macro_x + (macro & 1));
            rate += estimate_transform_bits(
                rate_model, transform_map, index, N148_TRANSFORM_DCT4);
        }
        long first_block = (long) first_block_y * blocks_x + first_block_x;
        uint8_t large_modes[N148_INTRA_MODE_SLOTS_PER_BLOCK];
        for (int part = 0; part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
            large_modes[part] = (uint8_t) mode;
        rate += estimate_mode_bits(rate_model, 0, modes, first_block,
                                   large_modes, 1, blocks_x);
        uint64_t distortion = fidelity_region_error(
            source, reconstructed, x0, y0, 32, 1);
        if (mode < N148_DIRECTIONAL_INTRA_MODE_COUNT) {
            if (distortion < nearest_reference_distortion)
                nearest_reference_distortion = distortion;
        } else if (distortion > nearest_reference_distortion) {
            continue;
        }
        double lambda = fidelity_rdo_lambda(quant->dc_step, 1);
        double cost = (double) distortion + lambda * rate;
        if (cost < best_cost) {
            best_cost = cost;
            for (int cell = 0; cell < 16; cell++) {
                memcpy(candidate->coefficients[cell],
                       sequence + cell * 64,
                       sizeof(candidate->coefficients[cell]));
                for (int part = 0;
                     part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                    candidate->modes[cell][part] = (uint8_t) mode;
            }
            candidate->distortion = distortion;
            candidate->proxy_bits = rate;
            candidate->ending_dc_category =
                coefficient_category(sequence[0]);
            take_super_snapshot(reconstructed, x0, y0,
                                &candidate->reconstruction);
        }
    }
    clear_super_syntax(first_block_x, first_block_y, blocks_x, blocks_y,
                       coefficients, modes, partition_map);
    for (int macro = 0; macro < 4; macro++) {
        size_t index = (size_t)(first_macro_y + (macro >> 1)) *
            (size_t) transform_map->columns[0] +
            (size_t)(first_macro_x + (macro & 1));
        transform_map->strategies[index] = original_strategies[macro];
    }
    restore_super_snapshot(reconstructed, &original);
    return best_cost < DBL_MAX;
}

/* Variable-luma selection visits 16x16 regions causally. The small alternative retains the
   mature per-cell 4x4/8x8 RDO; the large alternative uses its own mapped
   quantizer and is selected with reconstructed SSE plus contextual bits. */
static int quantize_variable_luma_plane(
    const Plane *source, int quality, int extended_modes, int split_4x4,
    int effort, int trellis_policy, int loop_filter,
    int calibrated_quant, const IntraRateModel *rate_model,
    N148PartitionMap *partition_map, N148TransformMap *transform_map,
    N148CoeffPlane *coefficients, uint8_t *modes,
    const N148SegmentationMap *segmentation_map) {
    size_t count;
    if (!source || !source->data || !rate_model || !partition_map ||
        !transform_map || !modes || quality < 1 || quality > 100 ||
        !split_4x4 || !fidelity_luma_transform_map_validate(transform_map) ||
        !checked_block_count(source->width, source->height, &count) ||
        !allocate_coefficients(count, coefficients)) return 0;
    int blocks_x = source->width / 8 + (source->width % 8 != 0);
    int blocks_y = source->height / 8 + (source->height % 8 != 0);
    if (partition_map->blocks_x[0] != blocks_x ||
        partition_map->blocks_y[0] != blocks_y ||
        transform_map->blocks_x[0] != blocks_x ||
        transform_map->blocks_y[0] != blocks_y) {
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    Plane reconstructed = create_plane(source->width, source->height);
    if (!reconstructed.data) {
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    /* Macroblock order can request a down-left reference from the not-yet
       visited top half of a later macroblock. Neutral initialization makes
       that legal causal padding explicit and decoder-independent. */
    memset(reconstructed.data, 128,
           (size_t) source->width * (size_t) source->height);
    IntraQuant8 quant8[N148_SEGMENT_COUNT];
    IntraQuant4 quant4[N148_SEGMENT_COUNT];
    IntraQuant16 quant16[N148_SEGMENT_COUNT];
    IntraQuant32 quant32[N148_SEGMENT_COUNT];
    const int (*base)[8] = luma_quant_base(calibrated_quant);
    int search_dct32 =
        (fidelity_tools & N148_FIDELITY_DCT32) != 0 && effort >= 5;
    if (!build_segment_quantizers(base, quality, 0, segmentation_map,
                                  quant8, quant4) ||
        !build_segment_quantizers16(base, quality, segmentation_map,
                                    quant16) ||
        (search_dct32 && !build_segment_quantizers32(
            base, quality, segmentation_map, quant32))) {
        free_plane(&reconstructed);
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    int previous_dc_category = 0;
    int success = 1;
    for (int macro_y = 0;
         macro_y < transform_map->rows[0] && success; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            int first_block_x = macro_x * 2;
            int first_block_y = macro_y * 2;
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            if (search_dct32 && n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) continue;
            int complete_super = search_dct32 && !(macro_x & 1) &&
                !(macro_y & 1) &&
                macro_x + 1 < transform_map->columns[0] &&
                macro_y + 1 < transform_map->rows[0] &&
                first_block_x + 3 < blocks_x &&
                first_block_y + 3 < blocks_y;
            if (complete_super) {
                uint8_t segment = 0;
                IntraSuperCandidate baseline, dct32;
                if (segmentation_map && !segmentation_level_prevalidated(
                        segmentation_map, first_block_x, first_block_y,
                        0, 0, &segment)) {
                    success = 0;
                    break;
                }
                if (!make_baseline_super_candidate(
                        source, &reconstructed, quality, extended_modes,
                        split_4x4, effort, trellis_policy, loop_filter,
                        quant8, quant4, quant16, rate_model, macro_x,
                        macro_y, blocks_x, blocks_y, previous_dc_category,
                        coefficients, modes, partition_map, segmentation_map,
                        transform_map, &baseline) ||
                    !make_dct32_super_candidate(
                        source, &reconstructed, quality, extended_modes,
                        effort, loop_filter, &quant32[segment], rate_model,
                        macro_x, macro_y, blocks_x, blocks_y,
                        previous_dc_category, coefficients, modes,
                        partition_map, segmentation_map, transform_map,
                        segment, &dct32)) {
                    success = 0;
                    break;
                }
                double lambda = fidelity_rdo_lambda(
                    quant32[segment].dc_step, 1);
                int use_dct32 = (double) dct32.distortion +
                    lambda * dct32.proxy_bits <
                    (double) baseline.distortion +
                    lambda * baseline.proxy_bits;
                if (use_dct32) {
                    install_super_candidate(
                        &dct32, macro_x, macro_y, blocks_x, coefficients,
                        modes, partition_map, transform_map, &reconstructed);
                    previous_dc_category = dct32.ending_dc_category;
                    continue;
                }
            }
            IntraMacroCandidate small, large;
            if (!make_small_macro_candidate(
                    source, &reconstructed, quality, extended_modes,
                    split_4x4, effort, trellis_policy, loop_filter,
                    quant8, quant4, rate_model, first_block_x,
                    first_block_y, blocks_x, blocks_y,
                    previous_dc_category, coefficients, modes,
                    partition_map, segmentation_map, transform_map,
                    transform_index, &small)) {
                success = 0;
                break;
            }
            int complete = first_block_x + 1 < blocks_x &&
                first_block_y + 1 < blocks_y;
            int use_large = 0;
            if (complete) {
                uint8_t segment = 0;
                if (segmentation_map && !segmentation_level_prevalidated(
                        segmentation_map, first_block_x, first_block_y,
                        0, 0, &segment)) {
                    success = 0;
                    break;
                }
                double lambda = fidelity_rdo_lambda(
                    quant16[segment].dc_step, 1);
                double small_cost = (double) small.distortion +
                    lambda * small.proxy_bits;
                if (!make_large_macro_candidate(
                        source, &reconstructed, quality, extended_modes,
                        effort, loop_filter, &quant16[segment], rate_model,
                        first_block_x, first_block_y, blocks_x,
                        previous_dc_category, coefficients, modes,
                        partition_map, segmentation_map, transform_map,
                        transform_index, segment, small_cost, &large)) {
                    success = 0;
                    break;
                }
                use_large = (double) large.distortion +
                    lambda * large.proxy_bits < small_cost;
            }
            const IntraMacroCandidate *chosen = use_large ? &large : &small;
            transform_map->strategies[transform_index] = use_large ?
                N148_TRANSFORM_DCT16 : N148_TRANSFORM_DCT8;
            install_macro_candidate(
                chosen, first_block_x, first_block_y, blocks_x, blocks_y,
                coefficients, modes, partition_map, &reconstructed);
            previous_dc_category = chosen->ending_dc_category;
        }
    }
    free_plane(&reconstructed);
    if (!success) n148_free_coeff_plane(coefficients);
    return success;
}

/* Joint chroma prediction selects one predictor for Cb and Cr. The two coefficient
   streams and reconstructed references remain independent; only the decision
   and its entropy symbol are shared. The caller controls whether the four
   baseline or all ten directional modes participate in the search. */
static int quantize_chroma_pair(
    const Plane *cb, const Plane *cr, int quality, int extended_modes,
    int effort, int calibrated_chroma,
    int trellis_policy, int loop_filter,
    const IntraRateModel *rate_model, N148PartitionMap *partition_map,
    N148CoeffPlane coefficients[2], uint8_t *cb_modes, uint8_t *cr_modes,
    int subsample_x, int subsample_y,
    const N148SegmentationMap *segmentation_map) {
    size_t count;
    if (!cb || !cr || !cb->data || !cr->data || !partition_map ||
        !coefficients || !cb_modes || !cr_modes ||
        cb->width != cr->width || cb->height != cr->height ||
        quality < 1 || quality > 100 || effort < 0 || effort > 9 ||
        (extended_modes != 0 && extended_modes != 1) ||
        (calibrated_chroma != 0 && calibrated_chroma != 1) ||
        (trellis_policy != N148_TRELLIS_OFF &&
         trellis_policy != N148_TRELLIS_LEGACY) ||
        loop_filter < N148_LOOP_FILTER_OFF ||
        loop_filter > N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA ||
        (subsample_x != 0 && subsample_x != 1) ||
        (subsample_y != 0 && subsample_y != 1) ||
        (segmentation_map &&
         !n148_segmentation_map_validate(segmentation_map)) ||
        !checked_block_count(cb->width, cb->height, &count)) return 0;
    memset(coefficients, 0, 2 * sizeof(*coefficients));
    if (!allocate_coefficients(count, &coefficients[0]) ||
        !allocate_coefficients(count, &coefficients[1])) {
        n148_free_coeff_plane(&coefficients[0]);
        n148_free_coeff_plane(&coefficients[1]);
        return 0;
    }
    int blocks_x = cb->width / 8 + (cb->width % 8 != 0);
    int blocks_y = cb->height / 8 + (cb->height % 8 != 0);
    if (partition_map->blocks_x[1] != blocks_x ||
        partition_map->blocks_y[1] != blocks_y ||
        partition_map->blocks_x[2] != blocks_x ||
        partition_map->blocks_y[2] != blocks_y) {
        n148_free_coeff_plane(&coefficients[0]);
        n148_free_coeff_plane(&coefficients[1]);
        return 0;
    }
    Plane reconstructed[2] = {
        create_plane(cb->width, cb->height),
        create_plane(cr->width, cr->height),
    };
    if (!reconstructed[0].data || !reconstructed[1].data) {
        free_plane(&reconstructed[0]);
        free_plane(&reconstructed[1]);
        n148_free_coeff_plane(&coefficients[0]);
        n148_free_coeff_plane(&coefficients[1]);
        return 0;
    }
    const Plane *source[2] = {cb, cr};
    IntraQuant8 quant[N148_SEGMENT_COUNT];
    IntraQuant4 unused_quant4[N148_SEGMENT_COUNT];
    if (!build_segment_quantizers(
            calibrated_chroma ? FORMAT_4_CHROMA_QUANT_BASE : Q_CHROMA_BASE,
            quality, 1, segmentation_map, quant, unused_quant4)) {
        free_plane(&reconstructed[0]);
        free_plane(&reconstructed[1]);
        n148_free_coeff_plane(&coefficients[0]);
        n148_free_coeff_plane(&coefficients[1]);
        return 0;
    }
    int mode_count = extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    int previous_dc_category[2] = {0, 0};
    int success = 1;
    for (int block_y = 0; block_y < blocks_y && success; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            long block = (long) block_y * blocks_x + block_x;
            int x0 = block_x * 8;
            int y0 = block_y * 8;
            uint8_t segment = 0;
            if (segmentation_map && !segmentation_level_prevalidated(
                    segmentation_map, block_x, block_y,
                    subsample_x, subsample_y, &segment)) {
                success = 0;
                break;
            }
            const IntraQuant8 *block_quant = &quant[0];
            IntraBlockCandidate best[2];
            memset(best, 0, sizeof(best));
            double best_cost = DBL_MAX;
            size_t cb_map = partition_map->plane_offsets[1] + (size_t) block;
            size_t cr_map = partition_map->plane_offsets[2] + (size_t) block;
            int shortlist = effort <= 4 && mode_count == 10 &&
                (fidelity_tools & N148_FIDELITY_FAST_MODE_SEARCH);
            uint8_t predictions[2][N148_DIRECTIONAL_INTRA_MODE_COUNT][64];
            uint8_t selected[N148_DIRECTIONAL_INTRA_MODE_COUNT] = {0};
            if (shortlist) {
                uint64_t scores[N148_DIRECTIONAL_INTRA_MODE_COUNT];
                for (int mode = 0; mode < mode_count; mode++) {
                    for (int plane = 0; plane < 2; plane++) {
                        predict_square(&reconstructed[plane], x0, y0, 8,
                                       mode, 128, -1, 0,
                                       predictions[plane][mode]);
                    }
                    scores[mode] = prediction_sse(
                        cb, x0, y0, 8, predictions[0][mode], UINT64_MAX) +
                        prediction_sse(
                        cr, x0, y0, 8, predictions[1][mode], UINT64_MAX);
                }
                selected[0] = 1;
                for (int rank = 0; rank < 2; rank++) {
                    int chosen = -1;
                    for (int mode = 0; mode < mode_count; mode++) {
                        if (scores[mode] == UINT64_MAX) continue;
                        if (chosen < 0 || scores[mode] < scores[chosen])
                            chosen = mode;
                    }
                    if (chosen < 0) break;
                    selected[chosen] = 1;
                    scores[chosen] = UINT64_MAX;
                }
            }
            for (int mode = 0; mode < mode_count; mode++) {
                if (shortlist && !selected[mode]) continue;
                IntraBlockCandidate trial[2];
                memset(trial, 0, sizeof(trial));
                if (!make_fixed_unsplit_candidate(
                        source[0], &reconstructed[0], x0, y0, 128, mode,
                        block_quant, effort, trellis_policy, rate_model,
                        &coefficients[0], block, blocks_x,
                        previous_dc_category[0],
                        shortlist ? predictions[0][mode] : NULL,
                        &trial[0]) ||
                    !make_fixed_unsplit_candidate(
                        source[1], &reconstructed[1], x0, y0, 128, mode,
                        block_quant, effort, trellis_policy, rate_model,
                        &coefficients[1], block, blocks_x,
                        previous_dc_category[1],
                        shortlist ? predictions[1][mode] : NULL,
                        &trial[1])) {
                    success = 0;
                    break;
                }
                double rate = trial[0].proxy_bits + trial[1].proxy_bits;
                if (rate_model) {
                    rate += estimate_mode_bits(
                        rate_model, 1, cb_modes, block, trial[0].modes,
                        1, blocks_x);
                    rate += estimate_partition_bits(
                        rate_model, partition_map, cb_map, 0);
                    rate += estimate_partition_bits(
                        rate_model, partition_map, cr_map, 0);
                } else {
                    rate += 2.0;
                }
                double lambda = INTRA_RDO_LAMBDA_SCALE *
                    block_quant->dc_step * block_quant->dc_step;
                double cost = joint_chroma_distortion(
                    cb, cr, trial, x0, y0) + lambda * rate;
                if (cost < best_cost) {
                    best_cost = cost;
                    best[0] = trial[0];
                    best[1] = trial[1];
                }
            }
            if (!success || best_cost == DBL_MAX) {
                success = 0;
                break;
            }
            for (int plane = 0; plane < 2; plane++) {
                memcpy(coefficients[plane].coefficients + block * 64,
                       best[plane].coefficients,
                       sizeof(best[plane].coefficients));
                coefficients[plane].nonzero_masks[block] =
                    coefficient_mask(best[plane].coefficients);
                restore_valid_block(&reconstructed[plane], x0, y0,
                                    best[plane].pixels);
                previous_dc_category[plane] =
                    coefficient_category(best[plane].coefficients[0]);
                if (!filter_reconstructed_block(
                        &reconstructed[plane], x0, y0, quality,
                        plane + 1, loop_filter, segment,
                        segmentation_map)) {
                    success = 0;
                    break;
                }
            }
            memcpy(cb_modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                   best[0].modes, sizeof(best[0].modes));
            memcpy(cr_modes + block * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                   best[0].modes, sizeof(best[0].modes));
            partition_map->split[cb_map] = 0;
            partition_map->split[cr_map] = 0;
            if (!success) break;
        }
    }
    free_plane(&reconstructed[0]);
    free_plane(&reconstructed[1]);
    if (!success) {
        n148_free_coeff_plane(&coefficients[0]);
        n148_free_coeff_plane(&coefficients[1]);
    }
    return success;
}

int n148_directional_intra_quantize_planes(
    const Plane *y, const Plane *cb, const Plane *cr, int quality,
    int chroma_quality, int calibrated_chroma, int calibrated_luma,
    int extended_modes,
    int split_4x4, int contextual_rdo, int effort, int trellis_policy,
    int loop_filter,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    N148TransformMap *transform_map,
    N148CoeffPlane coefficients[3], uint8_t **modes, size_t *mode_count) {
    if (!y || !cb || !cr || !partition_map || !coefficients || !modes ||
        !mode_count || cb->width != cr->width || cb->height != cr->height ||
        quality < 1 || quality > 100 || chroma_quality < 1 ||
        chroma_quality > 100 ||
        (calibrated_chroma != 0 && calibrated_chroma != 1) ||
        (calibrated_luma != 0 && calibrated_luma != 1) ||
        (segmentation_map &&
         !n148_segmentation_map_validate(segmentation_map)) ||
        trellis_policy < N148_TRELLIS_OFF ||
        trellis_policy > N148_TRELLIS_PERCEPTUAL_LUMA ||
        loop_filter < N148_LOOP_FILTER_OFF ||
        loop_filter > N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA ||
        (trellis_policy != N148_TRELLIS_OFF && !contextual_rdo) ||
        (transform_map &&
         (!contextual_rdo || !split_4x4 ||
          !fidelity_luma_transform_map_validate(transform_map))))
        return 0;
    memset(coefficients, 0, 3 * sizeof(*coefficients));
    *modes = NULL;
    *mode_count = 0;
    int chroma = CHROMA_444;
    if (cb->width < y->width)
        chroma = cb->height < y->height ? CHROMA_420 : CHROMA_422;
    int chroma_subsample_x = chroma != CHROMA_444;
    int chroma_subsample_y = chroma == CHROMA_420;
    int chroma_trellis_policy = trellis_policy == N148_TRELLIS_LEGACY ?
        N148_TRELLIS_LEGACY : N148_TRELLIS_OFF;
    size_t total_modes;
    if (!n148_directional_intra_expected_modes(y->width, y->height, chroma,
                                      &total_modes) ||
        !n148_partition_map_validate(partition_map)) return 0;
    uint8_t *all_modes = (uint8_t *) malloc(total_modes);
    if (!all_modes) return 0;
    size_t y_blocks, chroma_blocks;
    if (!checked_block_count(y->width, y->height, &y_blocks) ||
        !checked_block_count(cb->width, cb->height, &chroma_blocks)) {
        free(all_modes);
        return 0;
    }
    Plane corrected_y = {0};
    const Plane *final_y = y;
    init_dct_tables();
    int analysis_split = effort == 3 &&
        (fidelity_tools & N148_FIDELITY_STRUCTURAL_LUMA_QUANT) ? 0 : split_4x4;
    int success = quantize_plane(
        y, quality, 0, extended_modes, analysis_split, effort,
        0, loop_filter, calibrated_luma, 0, 0, NULL, partition_map,
        &coefficients[0],
        all_modes, segmentation_map, NULL) &&
        quantize_plane(
            cb, chroma_quality, 1, extended_modes, split_4x4,
            effort, 0, loop_filter, calibrated_chroma,
            chroma_subsample_x, chroma_subsample_y, NULL, partition_map,
            &coefficients[1],
            all_modes + y_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK,
            segmentation_map, NULL) &&
        quantize_plane(
            cr, chroma_quality, 2, extended_modes, split_4x4,
            effort, 0, loop_filter, calibrated_chroma,
            chroma_subsample_x, chroma_subsample_y, NULL, partition_map,
            &coefficients[2],
            all_modes + (y_blocks + chroma_blocks) *
                N148_INTRA_MODE_SLOTS_PER_BLOCK,
            segmentation_map, NULL);
    if (success && contextual_rdo) {
        IntraRateModel rate_model;
        int allowed_modes = luma_mode_count(extended_modes, 8, effort);
        success = build_rate_model(coefficients, all_modes, partition_map,
                                   allowed_modes, 0, NULL, &rate_model);
        for (int plane = 0; plane < 3; plane++)
            n148_free_coeff_plane(&coefficients[plane]);
        memset(all_modes, 0, total_modes);
        memset(partition_map->split, 0, partition_map->count);
        if (success && n148_fidelity_structural_quant() &&
            fidelity_rgb_source.pixels) {
            Plane reconstructed_chroma[2] = {{0},{0}};
            success = quantize_plane(
                cb, chroma_quality, 1, extended_modes, split_4x4, effort,
                chroma_trellis_policy, loop_filter, calibrated_chroma,
                chroma_subsample_x, chroma_subsample_y, &rate_model,
                partition_map, &coefficients[1],
                all_modes + y_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                segmentation_map, &reconstructed_chroma[0]) &&
                quantize_plane(
                cr, chroma_quality, 2, extended_modes, split_4x4, effort,
                chroma_trellis_policy, loop_filter, calibrated_chroma,
                chroma_subsample_x, chroma_subsample_y, &rate_model,
                partition_map, &coefficients[2],
                all_modes + (y_blocks + chroma_blocks) *
                    N148_INTRA_MODE_SLOTS_PER_BLOCK, segmentation_map, &reconstructed_chroma[1]);
            if (success) success = n148_project_luma_from_chroma(
                &fidelity_rgb_source, y, &reconstructed_chroma[0],
                &reconstructed_chroma[1], &corrected_y);
            if (success) final_y = &corrected_y;
            if (success) success = transform_map ? quantize_variable_luma_plane(
                final_y, quality, extended_modes, split_4x4, effort,
                trellis_policy, loop_filter, calibrated_luma,
                &rate_model, partition_map, transform_map,
                &coefficients[0], all_modes, segmentation_map) :
                quantize_plane(
                    final_y, quality, 0, extended_modes, split_4x4, effort,
                    trellis_policy, loop_filter, calibrated_luma, 0, 0,
                    &rate_model, partition_map, &coefficients[0], all_modes,
                    segmentation_map, NULL);
            free_plane(&reconstructed_chroma[0]);
            free_plane(&reconstructed_chroma[1]);
        }
        else if (success) {
            success = (transform_map ? quantize_variable_luma_plane(
                y, quality, extended_modes, split_4x4, effort,
                trellis_policy, loop_filter, calibrated_luma,
                &rate_model, partition_map, transform_map,
                &coefficients[0], all_modes, segmentation_map) :
                quantize_plane(
                    y, quality, 0, extended_modes, split_4x4, effort,
                    trellis_policy, loop_filter, calibrated_luma, 0, 0,
                    &rate_model, partition_map, &coefficients[0], all_modes,
                    segmentation_map, NULL)) &&
                quantize_plane(
                    cb, chroma_quality, 1, extended_modes, split_4x4, effort,
                    chroma_trellis_policy, loop_filter, calibrated_chroma,
                    chroma_subsample_x, chroma_subsample_y,
                    &rate_model, partition_map,
                    &coefficients[1],
                    all_modes + y_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK,
                    segmentation_map, NULL) &&
                quantize_plane(
                    cr, chroma_quality, 2, extended_modes, split_4x4, effort,
                    chroma_trellis_policy, loop_filter, calibrated_chroma,
                    chroma_subsample_x, chroma_subsample_y,
                    &rate_model, partition_map,
                    &coefficients[2],
                    all_modes + (y_blocks + chroma_blocks) *
                        N148_INTRA_MODE_SLOTS_PER_BLOCK,
                    segmentation_map, NULL);
        }
        /* The fast search keeps the first causal RDO result. The
           refined model requires a second complete luma reconstruction and
           measured only a small mixed quality change on the release corpus. */
        if (success && transform_map &&
            !(effort <= 4 &&
              (fidelity_tools & N148_FIDELITY_FAST_MODE_SEARCH))) {
            IntraRateModel refined_rate_model;
            success = build_rate_model(
                coefficients, all_modes, partition_map, allowed_modes, 0,
                transform_map, &refined_rate_model);
            n148_free_coeff_plane(&coefficients[0]);
            memset(all_modes, 0, y_blocks *
                   N148_INTRA_MODE_SLOTS_PER_BLOCK);
            memset(partition_map->split, 0,
                   partition_map->plane_offsets[1]);
            memset(transform_map->strategies, N148_TRANSFORM_DCT8,
                   transform_map->count);
            if (success) {
                success = quantize_variable_luma_plane(
                    final_y, quality, extended_modes, split_4x4, effort,
                    trellis_policy, loop_filter, calibrated_luma,
                    &refined_rate_model, partition_map, transform_map,
                    &coefficients[0], all_modes, segmentation_map);
            }
        }
    }
    free_plane(&corrected_y);
    if (!success) {
        for (int plane = 0; plane < 3; plane++)
            n148_free_coeff_plane(&coefficients[plane]);
        free(all_modes);
        return 0;
    }
    *modes = all_modes;
    *mode_count = total_modes;
    return 1;
}

int n148_joint_chroma_intra_quantize_planes(
    const Plane *y, const Plane *cb, const Plane *cr, int quality,
    int chroma_quality, int calibrated_chroma, int calibrated_luma,
    int extended_modes,
    int split_4x4, int contextual_rdo, int effort, int trellis_policy,
    int loop_filter,
    N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    N148TransformMap *transform_map,
    N148CoeffPlane coefficients[3], uint8_t **modes, size_t *mode_count) {
    if (!y || !cb || !cr || !partition_map || !coefficients || !modes ||
        !mode_count || cb->width != cr->width || cb->height != cr->height ||
        quality < 1 || quality > 100 || chroma_quality < 1 ||
        chroma_quality > 100 ||
        (calibrated_chroma != 0 && calibrated_chroma != 1) ||
        (calibrated_luma != 0 && calibrated_luma != 1) ||
        (segmentation_map &&
         !n148_segmentation_map_validate(segmentation_map)) ||
        trellis_policy < N148_TRELLIS_OFF ||
        trellis_policy > N148_TRELLIS_PERCEPTUAL_LUMA ||
        loop_filter < N148_LOOP_FILTER_OFF ||
        loop_filter > N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA ||
        !contextual_rdo ||
        (trellis_policy != N148_TRELLIS_OFF && !contextual_rdo) ||
        (transform_map &&
         (!split_4x4 ||
          !fidelity_luma_transform_map_validate(transform_map)))) return 0;
    memset(coefficients, 0, 3 * sizeof(*coefficients));
    *modes = NULL;
    *mode_count = 0;
    int chroma = CHROMA_444;
    if (cb->width < y->width)
        chroma = cb->height < y->height ? CHROMA_420 : CHROMA_422;
    int chroma_subsample_x = chroma != CHROMA_444;
    int chroma_subsample_y = chroma == CHROMA_420;
    int chroma_trellis_policy = trellis_policy == N148_TRELLIS_LEGACY ?
        N148_TRELLIS_LEGACY : N148_TRELLIS_OFF;
    size_t total_modes;
    if (!n148_directional_intra_expected_modes(y->width, y->height, chroma,
                                      &total_modes) ||
        !n148_partition_map_validate(partition_map)) return 0;
    uint8_t *all_modes = (uint8_t *) malloc(total_modes);
    if (!all_modes) return 0;
    size_t y_blocks, chroma_blocks;
    if (!checked_block_count(y->width, y->height, &y_blocks) ||
        !checked_block_count(cb->width, cb->height, &chroma_blocks)) {
        free(all_modes);
        return 0;
    }
    size_t y_mode_count = y_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK;
    size_t chroma_mode_count = chroma_blocks *
        N148_INTRA_MODE_SLOTS_PER_BLOCK;
    uint8_t *cb_modes = all_modes + y_mode_count;
    uint8_t *cr_modes = cb_modes + chroma_mode_count;
    init_dct_tables();
    int success = quantize_plane(
        y, quality, 0, extended_modes, split_4x4, effort,
        0, loop_filter, calibrated_luma, 0, 0, NULL, partition_map,
        &coefficients[0],
        all_modes, segmentation_map, NULL) &&
        quantize_chroma_pair(
            cb, cr, chroma_quality, extended_modes, effort,
            calibrated_chroma, 0, loop_filter, NULL, partition_map,
            &coefficients[1], cb_modes, cr_modes,
            chroma_subsample_x, chroma_subsample_y, segmentation_map);
    if (success) {
        IntraRateModel rate_model;
        int allowed_modes = luma_mode_count(extended_modes, 8, effort);
        success = build_rate_model(coefficients, all_modes, partition_map,
                                   allowed_modes, 1, NULL, &rate_model);
        for (int plane = 0; plane < 3; plane++)
            n148_free_coeff_plane(&coefficients[plane]);
        memset(all_modes, 0, total_modes);
        memset(partition_map->split, 0, partition_map->count);
        if (success) {
            success = (transform_map ? quantize_variable_luma_plane(
                y, quality, extended_modes, split_4x4, effort,
                trellis_policy, loop_filter, calibrated_luma,
                &rate_model, partition_map, transform_map,
                &coefficients[0], all_modes, segmentation_map) :
                quantize_plane(
                    y, quality, 0, extended_modes, split_4x4, effort,
                    trellis_policy, loop_filter, calibrated_luma, 0, 0,
                    &rate_model, partition_map, &coefficients[0], all_modes,
                    segmentation_map, NULL)) &&
                quantize_chroma_pair(
                    cb, cr, chroma_quality, extended_modes, effort,
                    calibrated_chroma, chroma_trellis_policy, loop_filter,
                    &rate_model, partition_map, &coefficients[1],
                    cb_modes, cr_modes, chroma_subsample_x,
                    chroma_subsample_y, segmentation_map);
        }
        if (success && transform_map &&
            !(effort <= 4 &&
              (fidelity_tools & N148_FIDELITY_FAST_MODE_SEARCH))) {
            IntraRateModel refined_rate_model;
            success = build_rate_model(
                coefficients, all_modes, partition_map, allowed_modes, 1,
                transform_map, &refined_rate_model);
            n148_free_coeff_plane(&coefficients[0]);
            memset(all_modes, 0, y_mode_count);
            memset(partition_map->split, 0,
                   partition_map->plane_offsets[1]);
            memset(transform_map->strategies, N148_TRANSFORM_DCT8,
                   transform_map->count);
            if (success) {
                success = quantize_variable_luma_plane(
                    y, quality, extended_modes, split_4x4, effort,
                    trellis_policy, loop_filter, calibrated_luma,
                    &refined_rate_model, partition_map, transform_map,
                    &coefficients[0], all_modes, segmentation_map);
            }
        }
    }
    if (!success) {
        for (int plane = 0; plane < 3; plane++)
            n148_free_coeff_plane(&coefficients[plane]);
        free(all_modes);
        return 0;
    }
    *modes = all_modes;
    *mode_count = total_modes;
    return 1;
}

static int reconstruct_plane(
    const N148CoeffPlane *coefficients, const uint8_t *modes,
    size_t mode_count, int width, int height, int quality, int plane_index,
    int extended_modes, int split_4x4, int loop_filter,
    int calibrated_quant, int subsample_x, int subsample_y,
    const N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map, Plane *output) {
    size_t blocks;
    if (!coefficients || !coefficients->coefficients || !modes || !output ||
        !partition_map || !checked_block_count(width, height, &blocks) ||
        coefficients->count != (long) blocks ||
        mode_count != blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK ||
        (subsample_x != 0 && subsample_x != 1) ||
        (subsample_y != 0 && subsample_y != 1) ||
        (segmentation_map &&
         !n148_segmentation_map_validate(segmentation_map))) return 0;
    int blocks_x = width / 8 + (width % 8 != 0);
    int blocks_y = height / 8 + (height % 8 != 0);
    if (partition_map->blocks_x[plane_index] != blocks_x ||
        partition_map->blocks_y[plane_index] != blocks_y) return 0;
    *output = create_plane(width, height);
    if (!output->data) return 0;
    IntraQuant8 quant8[N148_SEGMENT_COUNT];
    IntraQuant4 quant4[N148_SEGMENT_COUNT];
    N148LoopFilterPrepared filters[N148_SEGMENT_COUNT];
    const int (*base)[8] = plane_index ?
        (calibrated_quant ? FORMAT_4_CHROMA_QUANT_BASE : Q_CHROMA_BASE) :
        luma_quant_base(calibrated_quant);
    if (!build_segment_decode_quantizers(base, quality, plane_index,
                                         segmentation_map, quant8, quant4) ||
        !prepare_reconstruction_filters(
            quality, plane_index, loop_filter, segmentation_map, filters)) {
        free_plane(output);
        return 0;
    }
    int allowed_modes = plane_index == 0 ? luma_allowed_modes(extended_modes) :
        (extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4);
    int split_allowed_modes = extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    for (int block_y = 0; block_y < blocks_y; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            long block = (long) block_y * blocks_x + block_x;
            const short *values = coefficients->coefficients + block * 64;
            const uint8_t *block_modes = modes +
                block * N148_INTRA_MODE_SLOTS_PER_BLOCK;
            size_t map_index = partition_map->plane_offsets[plane_index] +
                (size_t) block;
            int use_split = partition_map->split[map_index];
            if ((!split_4x4 && use_split) ||
                (plane_index != 0 && use_split) || use_split > 1) {
                free_plane(output);
                return 0;
            }
            int x0 = block_x * 8;
            int y0 = block_y * 8;
            uint8_t segment = 0;
            if (segmentation_map && !segmentation_level_prevalidated(
                    segmentation_map, block_x, block_y,
                    subsample_x, subsample_y, &segment)) {
                free_plane(output);
                return 0;
            }
            if (use_split) {
                for (int part = 0; part < 4; part++) {
                    int mode = block_modes[part];
                    if (mode < 0 || mode >= split_allowed_modes) {
                        free_plane(output);
                        return 0;
                    }
                    int part_x = x0 + (part & 1) * 4;
                    int part_y = y0 + (part >> 1) * 4;
                    uint8_t prediction[16];
                    predict_square(output, part_x, part_y, 4, mode, 128,
                                   -1, plane_index == 0, prediction);
                    int quant_level = plane_index == 0 ? segment : 0;
                    unsigned int mask = coefficients->nonzero_masks ?
                        (unsigned int)((coefficients->nonzero_masks[block] >>
                            (part * 16)) & 0xffffu) : UINT_MAX;
                    if (!reconstruct4_masked(
                            values + part * 16, prediction,
                            &quant4[quant_level], output,
                            part_x, part_y, mask)) {
                        free_plane(output);
                        return 0;
                    }
                }
            } else {
                int mode = block_modes[0];
                if (mode < 0 || mode >= allowed_modes ||
                    block_modes[1] != mode || block_modes[2] != mode ||
                    block_modes[3] != mode) {
                    free_plane(output);
                    return 0;
                }
                uint8_t prediction[64];
                predict_square(output, x0, y0, 8, mode, 128, -1,
                               plane_index == 0,
                               prediction);
                int quant_level = plane_index == 0 ? segment : 0;
                if (!reconstruct8_masked(
                        values, prediction, &quant8[quant_level], output,
                        x0, y0, coefficients->nonzero_masks ?
                            coefficients->nonzero_masks[block] : ~0ull)) {
                    free_plane(output);
                    return 0;
                }
            }
            if (!filter_reconstructed_prepared(
                    output, x0, y0, 8, loop_filter, segment, filters)) {
                free_plane(output);
                return 0;
            }
        }
    }
    return 1;
}

static int reconstruct_variable_luma_plane(
    const N148CoeffPlane *coefficients, const uint8_t *modes,
    size_t mode_count, int width, int height, int quality,
    int extended_modes, int split_4x4, int loop_filter,
    int calibrated_quant, const N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    const N148TransformMap *transform_map, Plane *output) {
    size_t blocks;
    if (!coefficients || !coefficients->coefficients || !modes || !output ||
        !partition_map || !transform_map ||
        !checked_block_count(width, height, &blocks) ||
        coefficients->count != (long) blocks ||
        mode_count != blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK ||
        !split_4x4 || !fidelity_luma_transform_map_validate(transform_map) ||
        (segmentation_map &&
         !n148_segmentation_map_validate(segmentation_map))) return 0;
    int blocks_x = width / 8 + (width % 8 != 0);
    int blocks_y = height / 8 + (height % 8 != 0);
    if (partition_map->blocks_x[0] != blocks_x ||
        partition_map->blocks_y[0] != blocks_y ||
        transform_map->blocks_x[0] != blocks_x ||
        transform_map->blocks_y[0] != blocks_y) return 0;
    *output = create_plane(width, height);
    if (!output->data) return 0;
    memset(output->data, 128, (size_t) width * (size_t) height);
    IntraQuant8 quant8[N148_SEGMENT_COUNT];
    IntraQuant4 quant4[N148_SEGMENT_COUNT];
    IntraQuant16 quant16[N148_SEGMENT_COUNT];
    IntraQuant32 quant32[N148_SEGMENT_COUNT];
    N148LoopFilterPrepared filters[N148_SEGMENT_COUNT];
    const int (*base)[8] = luma_quant_base(calibrated_quant);
    if (!build_segment_decode_quantizers(base, quality, 0, segmentation_map,
                                         quant8, quant4) ||
        !build_segment_quantizers16(base, quality, segmentation_map,
                                    quant16) ||
        ((fidelity_tools & N148_FIDELITY_DCT32) &&
         !build_segment_quantizers32(base, quality, segmentation_map,
                                     quant32)) ||
        !prepare_reconstruction_filters(
            quality, 0, loop_filter, segmentation_map, filters)) {
        free_plane(output);
        return 0;
    }
    int allowed_modes = luma_allowed_modes(extended_modes);
    int split_allowed_modes = extended_modes ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            uint8_t strategy = transform_map->strategies[transform_index];
            int first_block_x = macro_x * 2;
            int first_block_y = macro_y * 2;
            int x0 = first_block_x * 8;
            int y0 = first_block_y * 8;
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                short sequence[32 * 32];
                int mode = -1;
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_block_x + (cell & 3);
                    int block_y = first_block_y + (cell >> 2);
                    long block = (long) block_y * blocks_x + block_x;
                    if (partition_map->split[block]) {
                        free_plane(output);
                        return 0;
                    }
                    const uint8_t *block_modes = modes +
                        block * N148_INTRA_MODE_SLOTS_PER_BLOCK;
                    if (block_modes[0] >= allowed_modes ||
                        block_modes[1] != block_modes[0] ||
                        block_modes[2] != block_modes[0] ||
                        block_modes[3] != block_modes[0] ||
                        (mode >= 0 && mode != block_modes[0])) {
                        free_plane(output);
                        return 0;
                    }
                    mode = block_modes[0];
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                uint8_t segment = 0;
                if (segmentation_map && !segmentation_level_prevalidated(
                        segmentation_map, first_block_x, first_block_y,
                        0, 0, &segment)) {
                    free_plane(output);
                    return 0;
                }
                uint8_t prediction[32 * 32];
                predict_square(output, x0, y0, 32, mode, 128, -1, 1,
                               prediction);
                if (!reconstruct32(sequence, prediction, &quant32[segment],
                                   output, x0, y0) ||
                    !filter_reconstructed_prepared(
                        output, x0, y0, 32, loop_filter, segment,
                        filters)) {
                    free_plane(output);
                    return 0;
                }
                continue;
            }
            if (strategy == N148_TRANSFORM_DCT16) {
                if (first_block_x + 1 >= blocks_x ||
                    first_block_y + 1 >= blocks_y) {
                    free_plane(output);
                    return 0;
                }
                short sequence[16 * 16];
                int mode = -1;
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_block_x + (cell & 1);
                    int block_y = first_block_y + (cell >> 1);
                    long block = (long) block_y * blocks_x + block_x;
                    if (partition_map->split[block]) {
                        free_plane(output);
                        return 0;
                    }
                    const uint8_t *block_modes = modes +
                        block * N148_INTRA_MODE_SLOTS_PER_BLOCK;
                    if (block_modes[0] >= allowed_modes ||
                        block_modes[1] != block_modes[0] ||
                        block_modes[2] != block_modes[0] ||
                        block_modes[3] != block_modes[0] ||
                        (mode >= 0 && mode != block_modes[0])) {
                        free_plane(output);
                        return 0;
                    }
                    mode = block_modes[0];
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                uint8_t segment = 0;
                if (segmentation_map && !segmentation_level_prevalidated(
                        segmentation_map, first_block_x, first_block_y,
                        0, 0, &segment)) {
                    free_plane(output);
                    return 0;
                }
                uint8_t prediction[16 * 16];
                predict_square(output, x0, y0, 16, mode, 128, -1, 1,
                               prediction);
                unsigned long long masks[4];
                const unsigned long long *block_masks = NULL;
                if (coefficients->nonzero_masks) {
                    for (int cell = 0; cell < 4; cell++) {
                        long index = (long)(first_block_y + (cell >> 1)) *
                            blocks_x + first_block_x + (cell & 1);
                        masks[cell] = coefficients->nonzero_masks[index];
                    }
                    block_masks = masks;
                }
                if (!reconstruct16(sequence, prediction, &quant16[segment],
                                   output, x0, y0, 1, block_masks) ||
                    !filter_reconstructed_prepared(
                        output, x0, y0, 16, loop_filter, segment,
                        filters)) {
                    free_plane(output);
                    return 0;
                }
                continue;
            }
            if (strategy != N148_TRANSFORM_DCT8) {
                free_plane(output);
                return 0;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_block_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_block_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    const short *values =
                        coefficients->coefficients + block * 64;
                    const uint8_t *block_modes = modes +
                        block * N148_INTRA_MODE_SLOTS_PER_BLOCK;
                    int use_split = partition_map->split[block];
                    if (use_split > 1) {
                        free_plane(output);
                        return 0;
                    }
                    int block_x0 = block_x * 8;
                    int block_y0 = block_y * 8;
                    uint8_t segment = 0;
                    if (segmentation_map &&
                        !segmentation_level_prevalidated(
                            segmentation_map, block_x, block_y, 0, 0,
                            &segment)) {
                        free_plane(output);
                        return 0;
                    }
                    if (use_split) {
                        for (int part = 0; part < 4; part++) {
                            int part_mode = block_modes[part];
                            if (part_mode < 0 ||
                                part_mode >= split_allowed_modes) {
                                free_plane(output);
                                return 0;
                            }
                            int part_x = block_x0 + (part & 1) * 4;
                            int part_y = block_y0 + (part >> 1) * 4;
                            uint8_t prediction[16];
                            predict_square(output, part_x, part_y, 4,
                                           part_mode, 128,
                                           first_block_x * 8 + 15,
                                           1,
                                           prediction);
                            unsigned int mask = coefficients->nonzero_masks ?
                                (unsigned int)(
                                    (coefficients->nonzero_masks[block] >>
                                        (part * 16)) & 0xffffu) : UINT_MAX;
                            if (!reconstruct4_masked(
                                    values + part * 16, prediction,
                                    &quant4[segment], output, part_x,
                                    part_y, mask)) {
                                free_plane(output);
                                return 0;
                            }
                        }
                    } else {
                        int block_mode = block_modes[0];
                        if (block_mode < 0 || block_mode >= allowed_modes ||
                            block_modes[1] != block_mode ||
                            block_modes[2] != block_mode ||
                            block_modes[3] != block_mode) {
                            free_plane(output);
                            return 0;
                        }
                        uint8_t prediction[64];
                        predict_square(output, block_x0, block_y0, 8,
                                       block_mode, 128,
                                       first_block_x * 8 + 15,
                                       1,
                                       prediction);
                        if (!reconstruct8_masked(
                                values, prediction, &quant8[segment], output,
                                block_x0, block_y0,
                                coefficients->nonzero_masks ?
                                    coefficients->nonzero_masks[block] :
                                    ~0ull)) {
                            free_plane(output);
                            return 0;
                        }
                    }
                    if (!filter_reconstructed_prepared(
                            output, block_x0, block_y0, 8, loop_filter,
                            segment, filters)) {
                        free_plane(output);
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

int n148_directional_intra_reconstruct_planes(
    const N148CoeffPlane coefficients[3], const uint8_t *modes,
    size_t mode_count, int width, int height, int quality,
    int chroma_quality, int calibrated_chroma, int calibrated_luma, int chroma,
    int extended_modes, int split_4x4, int loop_filter,
    const N148PartitionMap *partition_map,
    const N148SegmentationMap *segmentation_map,
    const N148TransformMap *transform_map,
    Plane *y, Plane *cb, Plane *cr) {
    if (!coefficients || !modes || !partition_map || !y || !cb || !cr ||
        width <= 0 || height <= 0 || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 ||
        (calibrated_chroma != 0 && calibrated_chroma != 1) ||
        (calibrated_luma != 0 && calibrated_luma != 1) ||
        loop_filter < N148_LOOP_FILTER_OFF ||
        loop_filter > N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA ||
        chroma < CHROMA_444 || chroma > CHROMA_420 ||
        !n148_partition_map_validate(partition_map) ||
        (segmentation_map &&
         !n148_segmentation_map_validate(segmentation_map)) ||
        (transform_map &&
         (!split_4x4 ||
          !fidelity_luma_transform_map_validate(transform_map)))) return 0;
    *y = (Plane){0};
    *cb = (Plane){0};
    *cr = (Plane){0};
    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    int chroma_subsample_x = chroma != CHROMA_444;
    int chroma_subsample_y = chroma == CHROMA_420;
    size_t y_blocks, chroma_blocks, expected_modes;
    if (!checked_block_count(width, height, &y_blocks) ||
        !checked_block_count(chroma_width, chroma_height, &chroma_blocks) ||
        !n148_directional_intra_expected_modes(width, height, chroma,
                                      &expected_modes) ||
        mode_count != expected_modes) return 0;
    size_t y_modes = y_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK;
    size_t chroma_modes = chroma_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK;
    int success = (transform_map ? reconstruct_variable_luma_plane(
        &coefficients[0], modes, y_modes, width, height, quality,
        extended_modes, split_4x4, loop_filter, calibrated_luma,
        partition_map, segmentation_map, transform_map, y) :
        reconstruct_plane(
            &coefficients[0], modes, y_modes, width, height, quality, 0,
            extended_modes, split_4x4, loop_filter, calibrated_luma, 0, 0,
            partition_map, segmentation_map, y)) &&
        reconstruct_plane(
            &coefficients[1], modes + y_modes, chroma_modes,
            chroma_width, chroma_height, chroma_quality, 1, extended_modes,
            split_4x4, loop_filter, calibrated_chroma,
            chroma_subsample_x, chroma_subsample_y, partition_map,
            segmentation_map, cb) &&
        reconstruct_plane(
            &coefficients[2], modes + y_modes + chroma_modes, chroma_modes,
            chroma_width, chroma_height, chroma_quality, 2, extended_modes,
            split_4x4, loop_filter, calibrated_chroma,
            chroma_subsample_x, chroma_subsample_y, partition_map,
            segmentation_map, cr);
    if (!success) {
        free_plane(y);
        free_plane(cb);
        free_plane(cr);
    }
    return success;
}

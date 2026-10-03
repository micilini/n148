/*
 * N.148i reconstructed-neighbor intra prediction.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Prediction is deliberately kept separate from the format-1 transform path. The
 * encoder reconstructs each chosen residual before visiting the next block,
 * exactly mirroring the decoder and preventing prediction drift.
 */

#include "intra.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "adaptive_quant.h"
#include "dct.h"
#include "header.h"
#include "loop_filter.h"
#include "rdo.h"
#include "tables.h"
#include "variable_transform.h"

typedef struct {
    float reciprocal[64];
    float multiplier[64];
    double unit_energy[64];
} IntraQuant;

typedef struct {
    int step[16 * 16];
    int size;
} VariableQuant;

#define N148_RDO_TRELLIS_LAMBDA_SCALE 0.04
#define N148_RDO_MODE_LAMBDA_SCALE 0.25

static int checked_block_count(int width, int height, size_t *count) {
    if (width <= 0 || height <= 0 || !count) return 0;
    size_t blocks_x = (size_t)(width / 8 + (width % 8 != 0));
    size_t blocks_y = (size_t)(height / 8 + (height % 8 != 0));
    if (blocks_y != 0 && blocks_x > SIZE_MAX / blocks_y) return 0;
    *count = blocks_x * blocks_y;
    return *count != 0 && *count <= LONG_MAX;
}

int n148_intra_expected_modes(int width, int height, int chroma,
                              size_t *mode_count) {
    if (!mode_count || chroma < 0 || chroma > 2) return 0;
    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    size_t luma, chroma_blocks;
    if (!checked_block_count(width, height, &luma) ||
        !checked_block_count(chroma_width, chroma_height, &chroma_blocks) ||
        chroma_blocks > (SIZE_MAX - luma) / 2) return 0;
    *mode_count = luma + chroma_blocks * 2;
    return 1;
}

static void build_quant_from_table(const int table[8][8], IntraQuant *quant) {
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
}

static void build_quant(const int base[8][8], int quality,
                        IntraQuant *quant) {
    int table[8][8];
    scale_table(base, quality, table);
    build_quant_from_table((const int (*)[8]) table, quant);
}

static void prepare_rdo_energy(IntraQuant *quant) {
    for (int position = 0; position < 64; position++) {
        float basis[64] = {0.0f};
        int natural = ZIGZAG[position];
        basis[natural] = quant->multiplier[natural];
        n148_idct_block_scalar(basis, basis);
        double energy = 0.0;
        for (int sample = 0; sample < 64; sample++) {
            double response = basis[sample] - 128.0;
            energy += response * response;
        }
        quant->unit_energy[position] = energy;
    }
}

static int build_adaptive_quants(const int base[8][8], int quality,
                                 IntraQuant quants[N148_AQ_LEVEL_COUNT]) {
    int scaled[8][8], adjusted[8][8];
    scale_table(base, quality, scaled);
    for (int level = 0; level < N148_AQ_LEVEL_COUNT; level++) {
        if (!n148_adaptive_adjust_table(
                (const int (*)[8]) scaled, (uint8_t) level, adjusted))
            return 0;
        build_quant_from_table((const int (*)[8]) adjusted, &quants[level]);
    }
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

static uint8_t prediction_neutral(int plane_index, int perceptual_color) {
    if (!perceptual_color) return 128;
    if (plane_index == 0) return 161;
    if (plane_index == 1) return 94;
    return 115;
}

static void prediction_block(const Plane *reconstructed, int block_x,
                             int block_y, int mode, uint8_t neutral,
                             uint8_t prediction[64]) {
    int x0 = block_x * 8;
    int y0 = block_y * 8;
    int has_top = y0 > 0;
    int has_left = x0 > 0;
    uint8_t top[8], left[8];
    int top_sum = 0;
    int left_sum = 0;

    for (int index = 0; index < 8; index++) {
        top[index] = has_top ? (uint8_t) sample_clamped(
            reconstructed, x0 + index, y0 - 1) : neutral;
        left[index] = has_left ? (uint8_t) sample_clamped(
            reconstructed, x0 - 1, y0 + index) : neutral;
        top_sum += top[index];
        left_sum += left[index];
    }
    int top_left = has_top && has_left ?
        sample_clamped(reconstructed, x0 - 1, y0 - 1) : neutral;
    int dc = neutral;
    if (has_top && has_left) dc = (top_sum + left_sum + 8) / 16;
    else if (has_top) dc = (top_sum + 4) / 8;
    else if (has_left) dc = (left_sum + 4) / 8;

    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int value;
            switch (mode) {
                case N148_INTRA_DC:
                    value = dc;
                    break;
                case N148_INTRA_VERTICAL:
                    value = top[column];
                    break;
                case N148_INTRA_HORIZONTAL:
                    value = left[row];
                    break;
                case N148_INTRA_TRUE_MOTION:
                    value = left[row] + top[column] - top_left;
                    break;
                default:
                    value = neutral;
                    break;
            }
            prediction[row * 8 + column] = clamp_byte(value);
        }
    }
}

static void hadamard8(int values[8]) {
    for (int span = 1; span < 8; span *= 2) {
        for (int first = 0; first < 8; first += span * 2) {
            for (int offset = 0; offset < span; offset++) {
                int a = values[first + offset];
                int b = values[first + offset + span];
                values[first + offset] = a + b;
                values[first + offset + span] = a - b;
            }
        }
    }
}

/* SATD uses an integer Hadamard transform as a cheap proxy for the number of
   transform bits. Unlike pixel SAD, it penalizes prediction residuals whose
   energy spreads across many frequencies. */
static uint64_t prediction_satd(const Plane *source, int block_x,
                                int block_y,
                                const uint8_t prediction[64]) {
    int transformed[64];
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int index = row * 8 + column;
            int original = sample_clamped(source, block_x * 8 + column,
                                          block_y * 8 + row);
            transformed[index] = original - prediction[index];
        }
        hadamard8(transformed + row * 8);
    }
    for (int column = 0; column < 8; column++) {
        int vertical[8];
        for (int row = 0; row < 8; row++)
            vertical[row] = transformed[row * 8 + column];
        hadamard8(vertical);
        for (int row = 0; row < 8; row++)
            transformed[row * 8 + column] = vertical[row];
    }
    uint64_t score = 0;
    for (int index = 0; index < 64; index++) {
        int value = transformed[index];
        score += (uint64_t)(value < 0 ? -value : value);
    }
    return score;
}

static int quantize_residual_details(const Plane *source, int block_x,
                                     int block_y,
                                     const uint8_t prediction[64],
                                     const IntraQuant *quant,
                                     short output[64],
                                     float ideal_levels[64]) {
    float block[64], transformed[64];
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int index = row * 8 + column;
            int original = sample_clamped(source, block_x * 8 + column,
                                          block_y * 8 + row);
            block[index] = (float)(original - prediction[index] + 128);
        }
    }
    dct_block_fast(block, transformed);
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

static int quantize_residual(const Plane *source, int block_x, int block_y,
                             const uint8_t prediction[64],
                             const IntraQuant *quant, short output[64]) {
    return quantize_residual_details(source, block_x, block_y, prediction,
                                     quant, output, NULL);
}

static int choose_mode(const Plane *source, const Plane *reconstructed,
                       int block_x, int block_y, uint8_t neutral,
                       uint8_t prediction[64]) {
    uint64_t best_satd = UINT64_MAX;
    int best_mode = N148_INTRA_DC;
    uint8_t candidate_prediction[64];
    for (int mode = 0; mode < N148_INTRA_MODE_COUNT; mode++) {
        prediction_block(reconstructed, block_x, block_y, mode, neutral,
                         candidate_prediction);
        uint64_t satd = prediction_satd(source, block_x, block_y,
                                        candidate_prediction);
        if (satd < best_satd) {
            best_satd = satd;
            best_mode = mode;
            memcpy(prediction, candidate_prediction,
                   sizeof(candidate_prediction));
        }
    }
    return best_mode;
}

static void reconstruct_block(const short coefficients[64],
                              const uint8_t prediction[64],
                              const IntraQuant *quant, Plane *output,
                              int block_x, int block_y) {
    float residual[64] = {0};
    for (int position = 0; position < 64; position++) {
        int natural = ZIGZAG[position];
        residual[natural] = coefficients[position] * quant->multiplier[natural];
    }
    n148_idct_block_scalar(residual, residual);
    int max_x = block_x * 8 + 8 <= output->width ?
        8 : output->width - block_x * 8;
    int max_y = block_y * 8 + 8 <= output->height ?
        8 : output->height - block_y * 8;
    for (int row = 0; row < max_y; row++) {
        uint8_t *destination = output->data +
            (long)(block_y * 8 + row) * output->width + block_x * 8;
        for (int column = 0; column < max_x; column++) {
            int index = row * 8 + column;
            float value = prediction[index] + residual[index] - 128.0f;
            if (value < 0.0f) value = 0.0f;
            if (value > 255.0f) value = 255.0f;
            destination[column] = (uint8_t)(value + 0.5f);
        }
    }
}

static uint64_t reconstructed_block_error(
    const Plane *source, int block_x, int block_y,
    const short coefficients[64], const uint8_t prediction[64],
    const IntraQuant *quant) {
    float residual[64] = {0.0f};
    for (int position = 0; position < 64; position++) {
        int natural = ZIGZAG[position];
        residual[natural] = coefficients[position] * quant->multiplier[natural];
    }
    n148_idct_block_scalar(residual, residual);
    int max_x = source->width - block_x * 8;
    int max_y = source->height - block_y * 8;
    if (max_x > 8) max_x = 8;
    if (max_y > 8) max_y = 8;
    uint64_t error = 0;
    for (int row = 0; row < max_y; row++) {
        for (int column = 0; column < max_x; column++) {
            int index = row * 8 + column;
            float value = prediction[index] + residual[index] - 128.0f;
            if (value < 0.0f) value = 0.0f;
            if (value > 255.0f) value = 255.0f;
            int reconstructed = (int)(value + 0.5f);
            int original = source->data[
                (long)(block_y * 8 + row) * source->width + block_x * 8 +
                column];
            int difference = original - reconstructed;
            error += (uint64_t)(difference * difference);
        }
    }
    return error;
}

static double prediction_mode_bits(const uint8_t *modes, long block,
                                   int blocks_x, int candidate) {
    int has_left = block % blocks_x != 0;
    int has_top = block >= blocks_x;
    if (!has_left && !has_top) return 2.0;
    int left = has_left ? modes[block - 1] : -1;
    int top = has_top ? modes[block - blocks_x] : -1;
    if (has_left && has_top && left != top)
        return candidate == left || candidate == top ? 1.25 : 3.0;
    int prediction = has_left ? left : top;
    return candidate == prediction ? 0.5 : 2.75;
}

static int choose_mode_rdo(
    const Plane *source, const Plane *reconstructed, int block_x, int block_y,
    uint8_t neutral, const IntraQuant *quant, const uint8_t *modes,
    long block, int blocks_x, int effort, double lambda,
    uint8_t prediction[64], short coefficients[64]) {
    double best_cost = DBL_MAX;
    int best_mode = -1;
    int first_mode = 0;
    int last_mode = N148_INTRA_MODE_COUNT;
    uint8_t candidate_prediction[64];
    short candidate_coefficients[64];
    float ideal_levels[64];
    float best_ideal_levels[64];
    if (effort >= 3 && effort < 8) {
        first_mode = choose_mode(source, reconstructed, block_x, block_y,
                                 neutral, candidate_prediction);
        last_mode = first_mode + 1;
    }
    for (int mode = first_mode; mode < last_mode; mode++) {
        prediction_block(reconstructed, block_x, block_y, mode, neutral,
                         candidate_prediction);
        if (!quantize_residual_details(
                source, block_x, block_y, candidate_prediction, quant,
                candidate_coefficients, ideal_levels)) return -1;
        if (effort >= 8 && !n148_rdo_refine_coefficients_trusted(
                ideal_levels, quant->unit_energy,
                lambda * N148_RDO_TRELLIS_LAMBDA_SCALE, effort,
                candidate_coefficients, NULL)) return -1;
        uint64_t distortion = reconstructed_block_error(
            source, block_x, block_y, candidate_coefficients,
            candidate_prediction, quant);
        double rate = n148_rdo_estimate_block_bits(candidate_coefficients) +
            prediction_mode_bits(modes, block, blocks_x, mode);
        double cost = distortion +
            lambda * N148_RDO_MODE_LAMBDA_SCALE * rate;
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = mode;
            memcpy(prediction, candidate_prediction,
                   sizeof(candidate_prediction));
            memcpy(coefficients, candidate_coefficients,
                   sizeof(candidate_coefficients));
            memcpy(best_ideal_levels, ideal_levels, sizeof(ideal_levels));
        }
    }
    if (best_mode < 0) return -1;
    if (effort >= 3 && effort < 8 &&
        !n148_rdo_refine_coefficients_trusted(
            best_ideal_levels, quant->unit_energy,
            lambda * N148_RDO_TRELLIS_LAMBDA_SCALE, effort,
            coefficients, NULL)) return -1;
    return best_mode;
}

static unsigned long long coefficient_mask(const short coefficients[64]) {
    unsigned long long mask = 0;
    for (int index = 0; index < 64; index++)
        if (coefficients[index] != 0) mask |= 1ull << index;
    return mask;
}

static int signed_round_division(int64_t value, int divisor, int *result) {
    if (!result || divisor <= 0) return 0;
    int64_t rounded = value >= 0 ?
        (value + divisor / 2) / divisor :
        -((-value + divisor / 2) / divisor);
    if (rounded < INT_MIN || rounded > INT_MAX) return 0;
    *result = (int) rounded;
    return 1;
}

static int build_variable_quant_from_table(const int table[8][8], int size,
                                           VariableQuant *quant) {
    if (!table || !quant || (size != 4 && size != 16)) return 0;
    quant->size = size;
    int denominator = size - 1;
    for (int row = 0; row < size; row++) {
        int source_row = (row * 7 + denominator / 2) / denominator;
        for (int column = 0; column < size; column++) {
            int source_column =
                (column * 7 + denominator / 2) / denominator;
            int value = table[source_row][source_column];
            if (value < 1 || value > 255) return 0;
            quant->step[row * size + column] = value;
        }
    }
    return 1;
}

static int build_variable_quant(const int base[8][8], int quality, int size,
                                VariableQuant *quant) {
    int table[8][8];
    scale_table(base, quality, table);
    return build_variable_quant_from_table((const int (*)[8]) table,
                                           size, quant);
}

static int build_adaptive_variable_quants(
    const int base[8][8], int quality, int size,
    VariableQuant quants[N148_AQ_LEVEL_COUNT]) {
    int scaled[8][8], adjusted[8][8];
    scale_table(base, quality, scaled);
    for (int level = 0; level < N148_AQ_LEVEL_COUNT; level++) {
        if (!n148_adaptive_adjust_table(
                (const int (*)[8]) scaled, (uint8_t) level, adjusted) ||
            !build_variable_quant_from_table(
                (const int (*)[8]) adjusted, size, &quants[level])) return 0;
    }
    return 1;
}

static void prediction_square(const Plane *reconstructed, int x0, int y0,
                              int size, int mode, uint8_t neutral,
                              uint8_t prediction[16 * 16]) {
    int has_top = y0 > 0;
    int has_left = x0 > 0;
    uint8_t top[16], left[16];
    int top_sum = 0;
    int left_sum = 0;
    for (int index = 0; index < size; index++) {
        top[index] = has_top ? (uint8_t) sample_clamped(
            reconstructed, x0 + index, y0 - 1) : neutral;
        left[index] = has_left ? (uint8_t) sample_clamped(
            reconstructed, x0 - 1, y0 + index) : neutral;
        top_sum += top[index];
        left_sum += left[index];
    }
    int top_left = has_top && has_left ?
        sample_clamped(reconstructed, x0 - 1, y0 - 1) : neutral;
    int dc = neutral;
    if (has_top && has_left)
        dc = (top_sum + left_sum + size) / (size * 2);
    else if (has_top) dc = (top_sum + size / 2) / size;
    else if (has_left) dc = (left_sum + size / 2) / size;
    for (int row = 0; row < size; row++) {
        for (int column = 0; column < size; column++) {
            int value;
            switch (mode) {
                case N148_INTRA_DC: value = dc; break;
                case N148_INTRA_VERTICAL: value = top[column]; break;
                case N148_INTRA_HORIZONTAL: value = left[row]; break;
                case N148_INTRA_TRUE_MOTION:
                    value = left[row] + top[column] - top_left;
                    break;
                default: value = neutral; break;
            }
            prediction[row * size + column] = clamp_byte(value);
        }
    }
}

static void hadamard_power_of_two(int *values, int size) {
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

static uint64_t prediction_square_satd(const Plane *source, int x0, int y0,
                                       int size,
                                       const uint8_t *prediction) {
    int transformed[16 * 16];
    for (int row = 0; row < size; row++) {
        for (int column = 0; column < size; column++) {
            transformed[row * size + column] =
                sample_clamped(source, x0 + column, y0 + row) -
                prediction[row * size + column];
        }
        hadamard_power_of_two(transformed + row * size, size);
    }
    for (int column = 0; column < size; column++) {
        int vertical[16];
        for (int row = 0; row < size; row++)
            vertical[row] = transformed[row * size + column];
        hadamard_power_of_two(vertical, size);
        for (int row = 0; row < size; row++)
            transformed[row * size + column] = vertical[row];
    }
    uint64_t score = 0;
    for (int index = 0; index < size * size; index++) {
        int value = transformed[index];
        score += (uint64_t)(value < 0 ? -value : value);
    }
    return score;
}

static int choose_square_mode(const Plane *source,
                              const Plane *reconstructed,
                              int x0, int y0, int size, uint8_t neutral,
                              uint8_t prediction[16 * 16]) {
    uint64_t best_score = UINT64_MAX;
    int best_mode = N148_INTRA_DC;
    uint8_t candidate[16 * 16];
    for (int mode = 0; mode < N148_INTRA_MODE_COUNT; mode++) {
        prediction_square(reconstructed, x0, y0, size, mode, neutral,
                          candidate);
        uint64_t score = prediction_square_satd(
            source, x0, y0, size, candidate);
        if (score < best_score) {
            best_score = score;
            best_mode = mode;
            memcpy(prediction, candidate, (size_t) size * (size_t) size);
        }
    }
    return best_mode;
}

static int quantize_integer_region(const Plane *source, int x0, int y0,
                                   const uint8_t *prediction,
                                   int prediction_stride,
                                   int prediction_x, int prediction_y,
                                   const VariableQuant *quant,
                                   short *output) {
    int size = quant ? quant->size : 0;
    if (!source || !source->data || !prediction || !quant || !output ||
        (size != 4 && size != 16) || prediction_stride < size) return 0;
    int32_t residual[16 * 16], transformed[16 * 16];
    for (int row = 0; row < size; row++) {
        for (int column = 0; column < size; column++) {
            residual[row * size + column] =
                sample_clamped(source, x0 + column, y0 + row) -
                prediction[(prediction_y + row) * prediction_stride +
                           prediction_x + column];
        }
    }
    if (!n148_variable_forward(residual, size, transformed)) return 0;
    for (int position = 0; position < size * size; position++) {
        int natural = n148_variable_zigzag(size, position);
        int value;
        if (natural < 0 || !signed_round_division(
                transformed[natural], quant->step[natural], &value) ||
            value < SHRT_MIN || value > SHRT_MAX) return 0;
        output[position] = (short) value;
    }
    return 1;
}

static int reconstruct_integer_region(const short *coefficients,
                                       const uint8_t *prediction,
                                       int prediction_stride,
                                       int prediction_x, int prediction_y,
                                       const VariableQuant *quant,
                                       Plane *output, int x0, int y0) {
    int size = quant ? quant->size : 0;
    if (!coefficients || !prediction || !quant || !output || !output->data ||
        (size != 4 && size != 16) || prediction_stride < size) return 0;
    int32_t transformed[16 * 16] = {0}, residual[16 * 16];
    for (int position = 0; position < size * size; position++) {
        int natural = n148_variable_zigzag(size, position);
        if (natural < 0) return 0;
        transformed[natural] =
            (int32_t) coefficients[position] * quant->step[natural];
    }
    if (!n148_variable_inverse(transformed, size, residual)) return 0;
    int max_x = output->width - x0;
    int max_y = output->height - y0;
    if (max_x > size) max_x = size;
    if (max_y > size) max_y = size;
    if (max_x <= 0 || max_y <= 0) return 1;
    for (int row = 0; row < max_y; row++) {
        uint8_t *destination = output->data +
            (long)(y0 + row) * output->width + x0;
        for (int column = 0; column < max_x; column++) {
            int value = prediction[(prediction_y + row) * prediction_stride +
                                   prediction_x + column] +
                residual[row * size + column];
            destination[column] = clamp_byte(value);
        }
    }
    return 1;
}

static int quantize_plane(const Plane *source, int quality, int plane_index,
                          int perceptual_color,
                          const N148AdaptiveMap *adaptive_map,
                          int subsample_x, int subsample_y,
                          int rdo_effort, int loop_filter,
                          N148CoeffPlane *coefficients, uint8_t *modes) {
    size_t count;
    if (!source || !source->data || !modes || rdo_effort < 0 ||
        rdo_effort > 9 || (loop_filter != 0 && loop_filter != 1) ||
        !checked_block_count(
            source->width, source->height, &count) ||
        !allocate_coefficients(count, coefficients)) return 0;
    Plane reconstructed = create_plane(source->width, source->height);
    if (!reconstructed.data) {
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    IntraQuant quant;
    const int (*base)[8];
    if (perceptual_color) {
        base = plane_index ? Q_PERCEPTUAL_OPPONENT_BASE :
                              Q_PERCEPTUAL_INTENSITY_BASE;
    } else {
        base = plane_index ? Q_CHROMA_BASE : Q_LUMA_BASE;
    }
    build_quant(base, quality, &quant);
    IntraQuant adaptive_quants[N148_AQ_LEVEL_COUNT];
    if (adaptive_map &&
        !build_adaptive_quants(base, quality, adaptive_quants)) {
        free_plane(&reconstructed);
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    if (rdo_effort >= 3) {
        prepare_rdo_energy(&quant);
        if (adaptive_map) {
            for (int level = 0; level < N148_AQ_LEVEL_COUNT; level++)
                prepare_rdo_energy(&adaptive_quants[level]);
        }
    }
    int blocks_x = source->width / 8 + (source->width % 8 != 0);
    int blocks_y = source->height / 8 + (source->height % 8 != 0);
    int success = 1;
    for (int block_y = 0; block_y < blocks_y && success; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            long block = (long) block_y * blocks_x + block_x;
            uint8_t prediction[64];
            short *values = coefficients->coefficients + block * 64;
            const IntraQuant *block_quant = &quant;
            uint8_t level = N148_AQ_INITIAL_LEVEL;
            if (adaptive_map) {
                if (!n148_adaptive_level_for_block(
                        adaptive_map, block_x, block_y,
                        subsample_x, subsample_y, &level)) {
                    success = 0;
                    break;
                }
                block_quant = &adaptive_quants[level];
            }
            int mode;
            if (rdo_effort >= 3) {
                double lambda = 0.85 * block_quant->unit_energy[0];
                mode = choose_mode_rdo(
                    source, &reconstructed, block_x, block_y,
                    prediction_neutral(plane_index, perceptual_color),
                    block_quant, modes, block, blocks_x, rdo_effort, lambda,
                    prediction, values);
                if (mode < 0) {
                    success = 0;
                    break;
                }
            } else {
                mode = choose_mode(
                    source, &reconstructed, block_x, block_y,
                    prediction_neutral(plane_index, perceptual_color),
                    prediction);
                if (!quantize_residual(source, block_x, block_y, prediction,
                                       block_quant, values)) {
                    success = 0;
                    break;
                }
            }
            modes[block] = (uint8_t) mode;
            coefficients->nonzero_masks[block] = coefficient_mask(values);
            reconstruct_block(values, prediction, block_quant, &reconstructed,
                              block_x, block_y);
            if (loop_filter && !n148_loop_filter_block(
                    &reconstructed, block_x * 8, block_y * 8, 8,
                    quality, plane_index, level, NULL)) {
                success = 0;
                break;
            }
        }
    }
    free_plane(&reconstructed);
    if (!success) n148_free_coeff_plane(coefficients);
    return success;
}

static int quantize_variable_plane(
    const Plane *source, int quality, int plane_index, int perceptual_color,
    const N148AdaptiveMap *adaptive_map, int subsample_x, int subsample_y,
    const N148TransformMap *transform_map,
    int rdo_effort, int loop_filter,
    N148CoeffPlane *coefficients, uint8_t *modes) {
    size_t count;
    if (!source || !source->data || !transform_map || !modes ||
        plane_index < 0 || plane_index > 2 || rdo_effort < 0 ||
        rdo_effort > 9 || (loop_filter != 0 && loop_filter != 1) ||
        !checked_block_count(source->width, source->height, &count) ||
        !allocate_coefficients(count, coefficients)) return 0;
    int blocks_x = source->width / 8 + (source->width % 8 != 0);
    int blocks_y = source->height / 8 + (source->height % 8 != 0);
    if (transform_map->blocks_x[plane_index] != blocks_x ||
        transform_map->blocks_y[plane_index] != blocks_y) {
        n148_free_coeff_plane(coefficients);
        return 0;
    }

    Plane reconstructed = create_plane(source->width, source->height);
    if (!reconstructed.data) {
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    const int (*base)[8];
    if (perceptual_color) {
        base = plane_index ? Q_PERCEPTUAL_OPPONENT_BASE :
                             Q_PERCEPTUAL_INTENSITY_BASE;
    } else {
        base = plane_index ? Q_CHROMA_BASE : Q_LUMA_BASE;
    }
    IntraQuant quant8;
    VariableQuant quant4, quant16;
    build_quant(base, quality, &quant8);
    if (!build_variable_quant(base, quality, 4, &quant4) ||
        !build_variable_quant(base, quality, 16, &quant16)) {
        free_plane(&reconstructed);
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    IntraQuant adaptive8[N148_AQ_LEVEL_COUNT];
    VariableQuant adaptive4[N148_AQ_LEVEL_COUNT];
    VariableQuant adaptive16[N148_AQ_LEVEL_COUNT];
    if (adaptive_map &&
        (!build_adaptive_quants(base, quality, adaptive8) ||
         !build_adaptive_variable_quants(base, quality, 4, adaptive4) ||
         !build_adaptive_variable_quants(base, quality, 16, adaptive16))) {
        free_plane(&reconstructed);
        n148_free_coeff_plane(coefficients);
        return 0;
    }
    if (rdo_effort >= 3) {
        prepare_rdo_energy(&quant8);
        if (adaptive_map) {
            for (int level = 0; level < N148_AQ_LEVEL_COUNT; level++)
                prepare_rdo_energy(&adaptive8[level]);
        }
    }

    uint8_t neutral = prediction_neutral(plane_index, perceptual_color);
    int success = 1;
    for (int macro_y = 0;
         macro_y < transform_map->rows[plane_index] && success; macro_y++) {
        for (int macro_x = 0;
             macro_x < transform_map->columns[plane_index]; macro_x++) {
            uint8_t strategy;
            if (!n148_transform_strategy_at(transform_map, plane_index,
                                            macro_x, macro_y, &strategy)) {
                success = 0;
                break;
            }
            int first_block_x = macro_x * 2;
            int first_block_y = macro_y * 2;
            if (strategy == N148_TRANSFORM_DCT16) {
                if (first_block_x + 1 >= blocks_x ||
                    first_block_y + 1 >= blocks_y) {
                    success = 0;
                    break;
                }
                uint8_t prediction[16 * 16];
                int mode = choose_square_mode(
                    source, &reconstructed, first_block_x * 8,
                    first_block_y * 8, 16, neutral, prediction);
                const VariableQuant *block_quant = &quant16;
                uint8_t level = N148_AQ_INITIAL_LEVEL;
                if (adaptive_map) {
                    if (!n148_adaptive_level_for_block(
                            adaptive_map, first_block_x, first_block_y,
                            subsample_x, subsample_y, &level)) {
                        success = 0;
                        break;
                    }
                    block_quant = &adaptive16[level];
                }
                short sequence[16 * 16];
                if (!quantize_integer_region(
                        source, first_block_x * 8, first_block_y * 8,
                        prediction, 16, 0, 0, block_quant, sequence)) {
                    success = 0;
                    break;
                }
                for (int chunk = 0; chunk < 4; chunk++) {
                    int block_x = first_block_x + (chunk & 1);
                    int block_y = first_block_y + (chunk >> 1);
                    long block = (long) block_y * blocks_x + block_x;
                    short *values = coefficients->coefficients + block * 64;
                    memcpy(values, sequence + chunk * 64,
                           64 * sizeof(*values));
                    modes[block] = (uint8_t) mode;
                    coefficients->nonzero_masks[block] =
                        coefficient_mask(values);
                }
                if (!reconstruct_integer_region(
                        sequence, prediction, 16, 0, 0, block_quant,
                        &reconstructed, first_block_x * 8,
                        first_block_y * 8) ||
                    (loop_filter && !n148_loop_filter_block(
                        &reconstructed, first_block_x * 8,
                        first_block_y * 8, 16, quality, plane_index,
                        level, NULL))) {
                    success = 0;
                    break;
                }
                continue;
            }

            for (int cell_y = 0; cell_y < 2 && success; cell_y++) {
                int block_y = first_block_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_block_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    short *values = coefficients->coefficients + block * 64;
                    uint8_t prediction[64];
                    uint8_t level = N148_AQ_INITIAL_LEVEL;
                    if (adaptive_map && !n148_adaptive_level_for_block(
                            adaptive_map, block_x, block_y, subsample_x,
                            subsample_y, &level)) {
                        success = 0;
                        break;
                    }
                    if (strategy == N148_TRANSFORM_DCT8) {
                        const IntraQuant *block_quant = adaptive_map ?
                            &adaptive8[level] : &quant8;
                        int mode;
                        if (rdo_effort >= 3) {
                            double lambda =
                                0.85 * block_quant->unit_energy[0];
                            mode = choose_mode_rdo(
                                source, &reconstructed, block_x, block_y,
                                neutral, block_quant, modes, block, blocks_x,
                                rdo_effort, lambda, prediction, values);
                            if (mode < 0) {
                                success = 0;
                                break;
                            }
                        } else {
                            mode = choose_mode(
                                source, &reconstructed, block_x, block_y,
                                neutral, prediction);
                            if (!quantize_residual(
                                    source, block_x, block_y, prediction,
                                    block_quant, values)) {
                                success = 0;
                                break;
                            }
                        }
                        modes[block] = (uint8_t) mode;
                        reconstruct_block(values, prediction, block_quant,
                                          &reconstructed, block_x, block_y);
                    } else if (strategy == N148_TRANSFORM_DCT4) {
                        int mode = choose_mode(
                            source, &reconstructed, block_x, block_y, neutral,
                            prediction);
                        modes[block] = (uint8_t) mode;
                        const VariableQuant *block_quant = adaptive_map ?
                            &adaptive4[level] : &quant4;
                        for (int part = 0; part < 4; part++) {
                            int offset_x = (part & 1) * 4;
                            int offset_y = (part >> 1) * 4;
                            if (!quantize_integer_region(
                                    source, block_x * 8 + offset_x,
                                    block_y * 8 + offset_y, prediction, 8,
                                    offset_x, offset_y, block_quant,
                                    values + part * 16) ||
                                !reconstruct_integer_region(
                                    values + part * 16, prediction, 8,
                                    offset_x, offset_y, block_quant,
                                    &reconstructed, block_x * 8 + offset_x,
                                    block_y * 8 + offset_y)) {
                                success = 0;
                                break;
                            }
                        }
                    } else {
                        success = 0;
                        break;
                    }
                    coefficients->nonzero_masks[block] =
                        coefficient_mask(values);
                    if (success && loop_filter && !n148_loop_filter_block(
                            &reconstructed, block_x * 8, block_y * 8, 8,
                            quality, plane_index, level, NULL)) success = 0;
                    if (!success) break;
                }
            }
        }
    }
    free_plane(&reconstructed);
    if (!success) n148_free_coeff_plane(coefficients);
    return success;
}

int n148_intra_quantize_planes(const Plane *y, const Plane *cb,
                               const Plane *cr, int quality,
                               int perceptual_color,
                               const N148AdaptiveMap *adaptive_map,
                               const N148TransformMap *transform_map,
                               int rdo_effort, int loop_filter,
                               N148CoeffPlane coefficients[3],
                               uint8_t **modes, size_t *mode_count) {
    if (!y || !cb || !cr || !coefficients || !modes || !mode_count ||
        quality < 1 || quality > 100 || rdo_effort < 0 || rdo_effort > 9 ||
        (loop_filter != 0 && loop_filter != 1))
        return 0;
    memset(coefficients, 0, 3 * sizeof(*coefficients));
    *modes = NULL;
    *mode_count = 0;
    size_t y_count, cb_count, cr_count;
    if (!checked_block_count(y->width, y->height, &y_count) ||
        !checked_block_count(cb->width, cb->height, &cb_count) ||
        !checked_block_count(cr->width, cr->height, &cr_count) ||
        cb_count > SIZE_MAX - y_count ||
        cr_count > SIZE_MAX - y_count - cb_count) return 0;
    size_t total = y_count + cb_count + cr_count;
    uint8_t *all_modes = (uint8_t *) malloc(total);
    if (!all_modes) return 0;
    init_dct_tables();
    int subsample_x = cb->width < y->width;
    int subsample_y = cb->height < y->height;
    if (cr->width != cb->width || cr->height != cb->height) {
        free(all_modes);
        return 0;
    }
    if (transform_map && !n148_transform_map_validate(transform_map)) {
        free(all_modes);
        return 0;
    }
    int success;
    if (transform_map) {
        success = quantize_variable_plane(
            y, quality, 0, perceptual_color, adaptive_map, 0, 0,
            transform_map, rdo_effort, loop_filter,
            &coefficients[0], all_modes) &&
            quantize_variable_plane(
                cb, quality, 1, perceptual_color, adaptive_map,
                subsample_x, subsample_y, transform_map, rdo_effort,
                loop_filter,
                &coefficients[1], all_modes + y_count) &&
            quantize_variable_plane(
                cr, quality, 2, perceptual_color, adaptive_map,
                subsample_x, subsample_y, transform_map, rdo_effort,
                loop_filter,
                &coefficients[2], all_modes + y_count + cb_count);
    } else {
        success = quantize_plane(y, quality, 0, perceptual_color,
                                 adaptive_map, 0, 0, rdo_effort, loop_filter,
                                 &coefficients[0], all_modes) &&
            quantize_plane(cb, quality, 1, perceptual_color,
                           adaptive_map, subsample_x, subsample_y, rdo_effort,
                           loop_filter,
                           &coefficients[1], all_modes + y_count) &&
            quantize_plane(cr, quality, 2, perceptual_color,
                           adaptive_map, subsample_x, subsample_y, rdo_effort,
                           loop_filter,
                           &coefficients[2], all_modes + y_count + cb_count);
    }
    if (!success) {
        for (int plane = 0; plane < 3; plane++)
            n148_free_coeff_plane(&coefficients[plane]);
        free(all_modes);
        return 0;
    }
    *modes = all_modes;
    *mode_count = total;
    return 1;
}

static int reconstruct_plane(const N148CoeffPlane *coefficients,
                             const uint8_t *modes, size_t mode_count,
                             int width, int height, int quality,
                             int plane_index, int perceptual_color,
                             const N148AdaptiveMap *adaptive_map,
                             int subsample_x, int subsample_y,
                             int loop_filter,
                             Plane *output) {
    size_t expected;
    if (!coefficients || !coefficients->coefficients || !modes || !output ||
        !checked_block_count(width, height, &expected) ||
        mode_count != expected || coefficients->count != (long) expected ||
        (loop_filter != 0 && loop_filter != 1))
        return 0;
    *output = create_plane(width, height);
    if (!output->data) return 0;
    IntraQuant quant;
    const int (*base)[8];
    if (perceptual_color) {
        base = plane_index ? Q_PERCEPTUAL_OPPONENT_BASE :
                              Q_PERCEPTUAL_INTENSITY_BASE;
    } else {
        base = plane_index ? Q_CHROMA_BASE : Q_LUMA_BASE;
    }
    build_quant(base, quality, &quant);
    IntraQuant adaptive_quants[N148_AQ_LEVEL_COUNT];
    if (adaptive_map &&
        !build_adaptive_quants(base, quality, adaptive_quants)) {
        free_plane(output);
        return 0;
    }
    int blocks_x = width / 8 + (width % 8 != 0);
    int blocks_y = height / 8 + (height % 8 != 0);
    for (int block_y = 0; block_y < blocks_y; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            long block = (long) block_y * blocks_x + block_x;
            int mode = modes[block];
            if (mode < 0 || mode >= N148_INTRA_MODE_COUNT) {
                free_plane(output);
                return 0;
            }
            uint8_t prediction[64];
            const IntraQuant *block_quant = &quant;
            uint8_t level = N148_AQ_INITIAL_LEVEL;
            if (adaptive_map) {
                if (!n148_adaptive_level_for_block(
                        adaptive_map, block_x, block_y,
                        subsample_x, subsample_y, &level)) {
                    free_plane(output);
                    return 0;
                }
                block_quant = &adaptive_quants[level];
            }
            prediction_block(
                output, block_x, block_y, mode,
                prediction_neutral(plane_index, perceptual_color), prediction);
            reconstruct_block(coefficients->coefficients + block * 64,
                              prediction, block_quant, output,
                              block_x, block_y);
            if (loop_filter && !n148_loop_filter_block(
                    output, block_x * 8, block_y * 8, 8, quality,
                    plane_index, level, NULL)) {
                free_plane(output);
                return 0;
            }
        }
    }
    return 1;
}

static int reconstruct_variable_plane(
    const N148CoeffPlane *coefficients, const uint8_t *modes,
    size_t mode_count, int width, int height, int quality, int plane_index,
    int perceptual_color, const N148AdaptiveMap *adaptive_map,
    int subsample_x, int subsample_y,
    const N148TransformMap *transform_map, int loop_filter, Plane *output) {
    size_t expected;
    if (!coefficients || !coefficients->coefficients || !modes || !output ||
        !transform_map || plane_index < 0 || plane_index > 2 ||
        !checked_block_count(width, height, &expected) ||
        mode_count != expected || coefficients->count != (long) expected ||
        (loop_filter != 0 && loop_filter != 1))
        return 0;
    int blocks_x = width / 8 + (width % 8 != 0);
    int blocks_y = height / 8 + (height % 8 != 0);
    if (transform_map->blocks_x[plane_index] != blocks_x ||
        transform_map->blocks_y[plane_index] != blocks_y) return 0;
    *output = create_plane(width, height);
    if (!output->data) return 0;

    const int (*base)[8];
    if (perceptual_color) {
        base = plane_index ? Q_PERCEPTUAL_OPPONENT_BASE :
                             Q_PERCEPTUAL_INTENSITY_BASE;
    } else {
        base = plane_index ? Q_CHROMA_BASE : Q_LUMA_BASE;
    }
    IntraQuant quant8;
    VariableQuant quant4, quant16;
    build_quant(base, quality, &quant8);
    if (!build_variable_quant(base, quality, 4, &quant4) ||
        !build_variable_quant(base, quality, 16, &quant16)) {
        free_plane(output);
        return 0;
    }
    IntraQuant adaptive8[N148_AQ_LEVEL_COUNT];
    VariableQuant adaptive4[N148_AQ_LEVEL_COUNT];
    VariableQuant adaptive16[N148_AQ_LEVEL_COUNT];
    if (adaptive_map &&
        (!build_adaptive_quants(base, quality, adaptive8) ||
         !build_adaptive_variable_quants(base, quality, 4, adaptive4) ||
         !build_adaptive_variable_quants(base, quality, 16, adaptive16))) {
        free_plane(output);
        return 0;
    }

    uint8_t neutral = prediction_neutral(plane_index, perceptual_color);
    for (int macro_y = 0; macro_y < transform_map->rows[plane_index];
         macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[plane_index];
             macro_x++) {
            uint8_t strategy;
            if (!n148_transform_strategy_at(transform_map, plane_index,
                                            macro_x, macro_y, &strategy)) {
                free_plane(output);
                return 0;
            }
            int first_block_x = macro_x * 2;
            int first_block_y = macro_y * 2;
            if (strategy == N148_TRANSFORM_DCT16) {
                if (first_block_x + 1 >= blocks_x ||
                    first_block_y + 1 >= blocks_y) {
                    free_plane(output);
                    return 0;
                }
                long first = (long) first_block_y * blocks_x + first_block_x;
                int mode = modes[first];
                if (mode < 0 || mode >= N148_INTRA_MODE_COUNT ||
                    modes[first + 1] != mode ||
                    modes[first + blocks_x] != mode ||
                    modes[first + blocks_x + 1] != mode) {
                    free_plane(output);
                    return 0;
                }
                uint8_t prediction[16 * 16];
                prediction_square(output, first_block_x * 8,
                                  first_block_y * 8, 16, mode, neutral,
                                  prediction);
                const VariableQuant *block_quant = &quant16;
                uint8_t level = N148_AQ_INITIAL_LEVEL;
                if (adaptive_map) {
                    if (!n148_adaptive_level_for_block(
                            adaptive_map, first_block_x, first_block_y,
                            subsample_x, subsample_y, &level)) {
                        free_plane(output);
                        return 0;
                    }
                    block_quant = &adaptive16[level];
                }
                short sequence[16 * 16];
                for (int chunk = 0; chunk < 4; chunk++) {
                    int block_x = first_block_x + (chunk & 1);
                    int block_y = first_block_y + (chunk >> 1);
                    long block = (long) block_y * blocks_x + block_x;
                    memcpy(sequence + chunk * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                if (!reconstruct_integer_region(
                        sequence, prediction, 16, 0, 0, block_quant, output,
                        first_block_x * 8, first_block_y * 8) ||
                    (loop_filter && !n148_loop_filter_block(
                        output, first_block_x * 8, first_block_y * 8, 16,
                        quality, plane_index, level, NULL))) {
                    free_plane(output);
                    return 0;
                }
                continue;
            }

            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_block_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_block_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int mode = modes[block];
                    if (mode < 0 || mode >= N148_INTRA_MODE_COUNT) {
                        free_plane(output);
                        return 0;
                    }
                    uint8_t prediction[64];
                    prediction_block(output, block_x, block_y, mode, neutral,
                                     prediction);
                    uint8_t level = N148_AQ_INITIAL_LEVEL;
                    if (adaptive_map && !n148_adaptive_level_for_block(
                            adaptive_map, block_x, block_y, subsample_x,
                            subsample_y, &level)) {
                        free_plane(output);
                        return 0;
                    }
                    const short *values =
                        coefficients->coefficients + block * 64;
                    if (strategy == N148_TRANSFORM_DCT8) {
                        const IntraQuant *block_quant = adaptive_map ?
                            &adaptive8[level] : &quant8;
                        reconstruct_block(values, prediction, block_quant,
                                          output, block_x, block_y);
                    } else if (strategy == N148_TRANSFORM_DCT4) {
                        const VariableQuant *block_quant = adaptive_map ?
                            &adaptive4[level] : &quant4;
                        for (int part = 0; part < 4; part++) {
                            int offset_x = (part & 1) * 4;
                            int offset_y = (part >> 1) * 4;
                            if (!reconstruct_integer_region(
                                    values + part * 16, prediction, 8,
                                    offset_x, offset_y, block_quant, output,
                                    block_x * 8 + offset_x,
                                    block_y * 8 + offset_y)) {
                                free_plane(output);
                                return 0;
                            }
                        }
                    } else {
                        free_plane(output);
                        return 0;
                    }
                    if (loop_filter && !n148_loop_filter_block(
                            output, block_x * 8, block_y * 8, 8, quality,
                            plane_index, level, NULL)) {
                        free_plane(output);
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

int n148_intra_reconstruct_planes(const N148CoeffPlane coefficients[3],
                                  const uint8_t *modes, size_t mode_count,
                                  int width, int height, int quality,
                                  int chroma, int perceptual_color,
                                  const N148AdaptiveMap *adaptive_map,
                                  const N148TransformMap *transform_map,
                                  int loop_filter,
                                  Plane *y, Plane *cb, Plane *cr) {
    if (!coefficients || !modes || !y || !cb || !cr ||
        width <= 0 || height <= 0 || quality < 1 || quality > 100 ||
        chroma < 0 || chroma > 2 ||
        (loop_filter != 0 && loop_filter != 1)) return 0;
    *y = (Plane){0};
    *cb = (Plane){0};
    *cr = (Plane){0};
    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    size_t y_count, chroma_count, expected;
    if (!checked_block_count(width, height, &y_count) ||
        !checked_block_count(chroma_width, chroma_height, &chroma_count) ||
        !n148_intra_expected_modes(width, height, chroma, &expected) ||
        mode_count != expected) return 0;
    int subsample_x = chroma != CHROMA_444;
    int subsample_y = chroma == CHROMA_420;
    if (transform_map && !n148_transform_map_validate(transform_map)) return 0;
    int success;
    if (transform_map) {
        success = reconstruct_variable_plane(
            &coefficients[0], modes, y_count, width, height, quality, 0,
            perceptual_color, adaptive_map, 0, 0, transform_map,
            loop_filter, y) &&
            reconstruct_variable_plane(
                &coefficients[1], modes + y_count, chroma_count,
                chroma_width, chroma_height, quality, 1, perceptual_color,
                adaptive_map, subsample_x, subsample_y, transform_map,
                loop_filter, cb) &&
            reconstruct_variable_plane(
                &coefficients[2], modes + y_count + chroma_count,
                chroma_count, chroma_width, chroma_height, quality, 2,
                perceptual_color, adaptive_map, subsample_x, subsample_y,
                transform_map, loop_filter, cr);
    } else {
        success = reconstruct_plane(&coefficients[0], modes, y_count,
                                    width, height, quality, 0,
                                    perceptual_color, adaptive_map, 0, 0,
                                    loop_filter, y) &&
            reconstruct_plane(&coefficients[1], modes + y_count, chroma_count,
                              chroma_width, chroma_height, quality, 1,
                              perceptual_color, adaptive_map,
                              subsample_x, subsample_y, loop_filter, cb) &&
            reconstruct_plane(
                &coefficients[2], modes + y_count + chroma_count,
                chroma_count, chroma_width, chroma_height, quality, 2,
                perceptual_color, adaptive_map, subsample_x, subsample_y,
                loop_filter, cr);
    }
    if (!success) {
        free_plane(y);
        free_plane(cb);
        free_plane(cr);
    }
    return success;
}

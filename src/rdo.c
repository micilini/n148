/*
 * N.148i rate-distortion trellis.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * This is an original, deliberately small dynamic program over the existing
 * run/category syntax. It is not copied from a reference codec.
 */

#include "rdo.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define N148_RDO_EOB_BITS 0.75
#define N148_RDO_SYMBOL_BITS 2.25
#define N148_RDO_ZRL_BITS 1.25
#define N148_RDO_RUN_FRACTION 0.08

/* Category one is used for every predecessor comparison. These rounded
   constants match ac_event_bits_category() for each run exactly, including
   its floating-point evaluation order. */
static const double predecessor_run_bits[64] = {
    3.25, 3.33, 3.41, 3.49, 3.57, 3.65, 3.73, 3.81,
    3.89, 3.9699999999999998, 4.05, 4.13, 4.21, 4.29, 4.37, 4.45,
    4.5, 4.58, 4.66, 4.74, 4.82, 4.9, 4.98, 5.0600000000000005,
    5.14, 5.22, 5.3, 5.38, 5.46, 5.54, 5.62, 5.7,
    5.75, 5.83, 5.91, 5.99, 6.07, 6.15, 6.23, 6.3100000000000005,
    6.39, 6.47, 6.55, 6.63, 6.71, 6.79, 6.87, 6.95,
    7, 7.08, 7.16, 7.24, 7.32, 7.4, 7.48, 7.5600000000000005,
    7.64, 7.72, 7.8, 7.88, 7.96, 8.04, 8.120000000000001, 8.2,
};

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

static double ac_event_bits_category(int zero_run, int category) {
    if (zero_run < 0 || category <= 0) return DBL_MAX;
    int zrl_count = zero_run / 16;
    int residual_run = zero_run & 15;
    return zrl_count * N148_RDO_ZRL_BITS + N148_RDO_SYMBOL_BITS + category +
        residual_run * N148_RDO_RUN_FRACTION;
}

static double ac_event_bits(int zero_run, short value) {
    return ac_event_bits_category(zero_run, coefficient_category(value));
}

static double estimate_ac_bits(const short coefficients[64]) {
    int next_position = 1;
    double bits = 0.0;
    for (int position = 1; position < 64; position++) {
        if (coefficients[position] == 0) continue;
        bits += ac_event_bits(position - next_position,
                              coefficients[position]);
        next_position = position + 1;
    }
    if (next_position < 64) bits += N148_RDO_EOB_BITS;
    return bits;
}

double n148_rdo_estimate_block_bits(const short coefficients[64]) {
    if (!coefficients) return DBL_MAX;
    return N148_RDO_SYMBOL_BITS + coefficient_category(coefficients[0]) +
        estimate_ac_bits(coefficients);
}

static double coefficient_distortion(float ideal, short quantized,
                                     double energy) {
    double difference = (double) ideal - quantized;
    return difference * difference * energy;
}

static double proxy_cost_validated(const float ideal_levels[64],
                                   const double unit_energy[64],
                                   const short coefficients[64],
                                   double lambda) {
    double distortion = 0.0;
    double bits = 0.0;
    int next_position = 1;
    for (int position = 1; position < 64; position++) {
        distortion += coefficient_distortion(
            ideal_levels[position], coefficients[position],
            unit_energy[position]);
        if (coefficients[position] != 0) {
            bits += ac_event_bits(position - next_position,
                                  coefficients[position]);
            next_position = position + 1;
        }
    }
    if (next_position < 64) bits += N148_RDO_EOB_BITS;
    return distortion + lambda * bits;
}

double n148_rdo_proxy_cost(const float ideal_levels[64],
                           const double unit_energy[64],
                           const short coefficients[64], double lambda) {
    if (!ideal_levels || !unit_energy || !coefficients ||
        !isfinite(lambda) || lambda < 0.0) return DBL_MAX;
    for (int position = 1; position < 64; position++) {
        if (!isfinite(ideal_levels[position]) ||
            !isfinite(unit_energy[position]) || unit_energy[position] < 0.0)
            return DBL_MAX;
    }
    return proxy_cost_validated(
        ideal_levels, unit_energy, coefficients, lambda);
}

static int add_candidate(short candidates[3], int count, int value) {
    if (value == 0 || value < SHRT_MIN || value > SHRT_MAX) return count;
    for (int index = 0; index < count; index++)
        if (candidates[index] == value) return count;
    if (count < 3) candidates[count++] = (short) value;
    return count;
}

static int candidates_for_position(float ideal, short rounded, int effort,
                                   short candidates[3]) {
    int count = 0;
    count = add_candidate(candidates, count, rounded);
    if (rounded > 1) count = add_candidate(candidates, count, rounded - 1);
    if (rounded < -1) count = add_candidate(candidates, count, rounded + 1);
    if (effort >= 8) {
        if (rounded > 0)
            count = add_candidate(candidates, count, (int) rounded + 1);
        else if (rounded < 0)
            count = add_candidate(candidates, count, (int) rounded - 1);
        else if (fabsf(ideal) >= 0.35f)
            count = add_candidate(candidates, count, ideal < 0.0f ? -1 : 1);
    }
    return count;
}

static uint32_t nonzero_count(const short coefficients[64]) {
    uint32_t count = 0;
    for (int position = 1; position < 64; position++)
        count += coefficients[position] != 0;
    return count;
}

static int refine_coefficients_core(const float ideal_levels[64],
                                    const double unit_energy[64],
                                    double lambda, int effort,
                                    short coefficients[64],
                                    N148RdoResult *result,
                                    int verify_result) {
    /* At efforts 3 and 4, zero AC coefficients have no nonzero trellis
       candidate. An entirely empty AC block must therefore remain empty;
       avoid building its cost table and predecessor graph. The public path
       still runs its full validation and result accounting. */
    if ((effort == 3 || effort == 4) && !verify_result && !result) {
        int any_ac = 0;
        for (int position = 1; position < 64; position++)
            any_ac |= coefficients[position];
        if (!any_ac) return 1;
    }
    short input[64];
    const short *initial = coefficients;
    if (verify_result || result) {
        memcpy(input, coefficients, sizeof(input));
        initial = input;
    }
    double zero_prefix[65] = {0.0};
    for (int position = 1; position < 64; position++) {
        zero_prefix[position + 1] = zero_prefix[position] +
            coefficient_distortion(ideal_levels[position], 0,
                                   unit_energy[position]);
    }

    int previous_position[64];
    short ending_value[64];
    int active_positions[64];
    double active_best[64];
    double active_zero_after[64];
    double prefix_minimum[64], prefix_magnitude[64];
    int active_count = 0;

    for (int position = 1; position < 64; position++) {
        /* Below effort 8 a zero rounded AC level has no nonzero candidate.
           Keep the predecessor graph restricted to positions that can be
           selected; zero_prefix still accounts for every skipped level. */
        if (effort < 8 && initial[position] == 0) continue;
        short candidates[3];
        int candidate_count = candidates_for_position(
            ideal_levels[position], initial[position], effort, candidates);
        if (candidate_count == 0) continue;
        double best_ending = DBL_MAX;
        previous_position[position] = -1;
        ending_value[position] = 0;
        /* Every candidate at this position adds its own distortion and
           coefficient category to the same predecessor path. Select that
           path once, then evaluate each candidate with its exact rate. */
        int best_predecessor = -1;
        double best_predecessor_cost = DBL_MAX;
        for (int active = active_count-1; active >= 0; active--) {
            double bound = prefix_minimum[active] + zero_prefix[position] + lambda * 3.25;
            double slack = 64.0 * DBL_EPSILON * (prefix_magnitude[active] +
                zero_prefix[position] + fabs(best_predecessor_cost) + lambda * 3.25 + 1.0);
            if (bound > best_predecessor_cost + slack) break;
            int previous = active_positions[active];
            double cost = active_best[active] +
                zero_prefix[position] - active_zero_after[active] +
                lambda * predecessor_run_bits[
                    position - previous - 1];
            if (cost < best_predecessor_cost || (cost == best_predecessor_cost &&
                best_predecessor >= 0 && active < best_predecessor)) {
                best_predecessor_cost = cost;
                best_predecessor = active;
            }
        }
        for (int candidate_index = 0; candidate_index < candidate_count;
             candidate_index++) {
            short value = candidates[candidate_index];
            int category = coefficient_category(value);
            double distortion = coefficient_distortion(
                ideal_levels[position], value, unit_energy[position]);
            double cost = zero_prefix[position] - zero_prefix[1] + distortion +
                lambda * ac_event_bits_category(position - 1, category);
            int predecessor = -1;
            if (best_predecessor >= 0) {
                int previous = active_positions[best_predecessor];
                double candidate_cost = active_best[best_predecessor] +
                    zero_prefix[position] -
                    active_zero_after[best_predecessor] +
                    distortion + lambda * ac_event_bits_category(
                        position - previous - 1, category);
                if (candidate_cost < cost) {
                    cost = candidate_cost;
                    predecessor = previous;
                }
            }
            if (cost < best_ending) {
                best_ending = cost;
                previous_position[position] = predecessor;
                ending_value[position] = value;
            }
        }
        if (best_ending != DBL_MAX) {
            active_positions[active_count] = position;
            active_best[active_count] = best_ending;
            active_zero_after[active_count] = zero_prefix[position + 1];
            double base = best_ending - zero_prefix[position + 1];
            double magnitude = fabs(best_ending) + zero_prefix[position + 1];
            if (active_count) {
                if (prefix_minimum[active_count-1] < base) base = prefix_minimum[active_count-1];
                if (prefix_magnitude[active_count-1] > magnitude) magnitude = prefix_magnitude[active_count-1];
            }
            prefix_minimum[active_count] = base;
            prefix_magnitude[active_count] = magnitude;
            active_count++;
        }
    }

    double best_cost = zero_prefix[64] - zero_prefix[1] +
        lambda * N148_RDO_EOB_BITS;
    int last_position = -1;
    for (int active = 0; active < active_count; active++) {
        int position = active_positions[active];
        double cost = active_best[active] +
            zero_prefix[64] - zero_prefix[position + 1];
        if (position < 63) cost += lambda * N148_RDO_EOB_BITS;
        if (cost < best_cost) {
            best_cost = cost;
            last_position = position;
        }
    }

    memset(coefficients + 1, 0, 63 * sizeof(*coefficients));
    while (last_position >= 1) {
        coefficients[last_position] = ending_value[last_position];
        last_position = previous_position[last_position];
    }

    if (verify_result || result) {
        double input_cost = proxy_cost_validated(
            ideal_levels, unit_energy, input, lambda);
        double output_cost = proxy_cost_validated(
            ideal_levels, unit_energy, coefficients, lambda);
        double tolerance = 1e-9 * (fabs(input_cost) + 1.0);
        if (!isfinite(input_cost) || !isfinite(output_cost) ||
            output_cost > input_cost + tolerance) {
            memcpy(coefficients, input, sizeof(input));
            return 0;
        }
        if (!result) return 1;
        memset(result, 0, sizeof(*result));
        result->input_nonzeros = nonzero_count(input);
        result->output_nonzeros = nonzero_count(coefficients);
        for (int position = 1; position < 64; position++)
            result->changed_coefficients +=
                input[position] != coefficients[position];
        result->input_cost = input_cost;
        result->output_cost = output_cost;
    }
    return 1;
}

int n148_rdo_refine_coefficients_trusted(const float ideal_levels[64],
                                          const double unit_energy[64],
                                          double lambda, int effort,
                                          short coefficients[64],
                                          N148RdoResult *result) {
    return refine_coefficients_core(
        ideal_levels, unit_energy, lambda, effort, coefficients, result,
        result != NULL);
}

int n148_rdo_refine_coefficients(const float ideal_levels[64],
                                  const double unit_energy[64],
                                  double lambda, int effort,
                                  short coefficients[64],
                                  N148RdoResult *result) {
    if (!ideal_levels || !unit_energy || !coefficients || effort < 0 ||
        effort > 9 || !isfinite(lambda) || lambda < 0.0) return 0;
    for (int position = 0; position < 64; position++) {
        if (!isfinite(ideal_levels[position]) ||
            !isfinite(unit_energy[position]) || unit_energy[position] < 0.0)
            return 0;
    }
    return refine_coefficients_core(
        ideal_levels, unit_energy, lambda, effort, coefficients, result, 1);
}

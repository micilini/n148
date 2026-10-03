/*
 * N.148i deterministic cone-opponent colour transform.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * This is an original, deliberately compact perceptual transform. It applies
 * a cube-root response after deterministic gamma expansion, then stores one
 * perceptual intensity plus two opponent axes. Integer arithmetic defines the
 * bitstream-facing forward transform so the same RGB input cannot acquire
 * platform-dependent coefficients through libm.
 */

#include "perceptual.h"

#include <stddef.h>
#include <stdint.h>

#define PERCEPTUAL_MAX 65535
#define RESPONSE_TABLE_BITS 4
#define RESPONSE_TABLE_SIZE ((PERCEPTUAL_MAX >> RESPONSE_TABLE_BITS) + 2)
#define XYB_X_SCALE 59
#define XYB_B_SCALE 253
#define XYB_X_NEUTRAL 94
#define XYB_B_NEUTRAL 115
#define CHROMA_FRACTION_BITS 4
#define OPSIN_BIAS 0

/* Rounded, intentionally compact cone mixing. Rows sum to 4096, so neutral
   grey remains neutral. The coefficients follow broad cone sensitivities but
   are represented as small integers rather than importing another codec's
   floating-point implementation. */
static const int cone_matrix[3][3] = {
    {1700, 2160,  236},
    { 900, 2900,  296},
    { 120,  500, 3476},
};

/* Inverse of the integer matrix above in Q20 fixed point. */
static const int cone_inverse_q20[3][3] = {
    { 4170622, -3103133,  -18912},
    {-1298702,  2469386, -122107},
    {   42830,  -248077, 1253823},
};

static uint16_t response_table[RESPONSE_TABLE_SIZE];
static uint16_t response_to_linear[PERCEPTUAL_MAX + 1];
static uint16_t srgb_to_linear[256];
static uint8_t linear_to_srgb[PERCEPTUAL_MAX + 1];
static int tables_ready;

static uint16_t perceptual_response(uint16_t linear);

static uint64_t cube_u16(uint32_t value) {
    return (uint64_t) value * value * value;
}

static uint32_t nearest_cube_root_response(uint32_t linear) {
    uint64_t target = (uint64_t) linear * PERCEPTUAL_MAX * PERCEPTUAL_MAX;
    uint32_t low = 0;
    uint32_t high = PERCEPTUAL_MAX + 256;
    while (low < high) {
        uint32_t middle = low + (high - low + 1) / 2;
        if (cube_u16(middle) <= target) low = middle;
        else high = middle - 1;
    }
    if (low < PERCEPTUAL_MAX + 256) {
        uint64_t lower_error = target - cube_u16(low);
        uint64_t upper_error = cube_u16(low + 1) - target;
        if (upper_error < lower_error) low++;
    }
    return low;
}

static int rounded_signed_division(int64_t value, int divisor) {
    if (value >= 0) return (int)((value + divisor / 2) / divisor);
    return -(int)((-value + divisor / 2) / divisor);
}

static int clamp_u16(int value) {
    if (value < 0) return 0;
    if (value > PERCEPTUAL_MAX) return PERCEPTUAL_MAX;
    return value;
}

static uint8_t clamp_byte(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t) value;
}

void n148_perceptual_init(void) {
    if (tables_ready) return;

    for (int value = 0; value < 256; value++) {
        uint32_t squared = (uint32_t) value * value;
        srgb_to_linear[value] =
            (uint16_t)((squared * 257u + 127u) / 255u);
    }
    uint32_t bias_root = nearest_cube_root_response(OPSIN_BIAS);
    uint32_t white_root = nearest_cube_root_response(
        PERCEPTUAL_MAX + OPSIN_BIAS);
    uint32_t root_range = white_root - bias_root;
    for (int index = 0; index < RESPONSE_TABLE_SIZE; index++) {
        uint32_t linear = (uint32_t) index << RESPONSE_TABLE_BITS;
        if (linear > PERCEPTUAL_MAX) linear = PERCEPTUAL_MAX;
        uint32_t biased_root = nearest_cube_root_response(linear + OPSIN_BIAS);
        response_table[index] = (uint16_t)(
            ((biased_root - bias_root) * PERCEPTUAL_MAX + root_range / 2) /
            root_range);
    }

    int linear = 0;
    for (int response = 0; response <= PERCEPTUAL_MAX; response++) {
        while (linear < PERCEPTUAL_MAX) {
            int lower_error = response - perceptual_response((uint16_t) linear);
            int upper_error = perceptual_response((uint16_t)(linear + 1)) -
                response;
            if (upper_error >= lower_error) break;
            linear++;
        }
        response_to_linear[response] = (uint16_t) linear;
    }

    int srgb = 0;
    for (int linear = 0; linear <= PERCEPTUAL_MAX; linear++) {
        while (srgb < 255) {
            int lower_error = linear - srgb_to_linear[srgb];
            int upper_error = srgb_to_linear[srgb + 1] - linear;
            if (upper_error >= lower_error) break;
            srgb++;
        }
        linear_to_srgb[linear] = (uint8_t) srgb;
    }
    tables_ready = 1;
}

static uint16_t perceptual_response(uint16_t linear) {
    if (linear == PERCEPTUAL_MAX) return PERCEPTUAL_MAX;
    unsigned index = linear >> RESPONSE_TABLE_BITS;
    unsigned fraction = linear & ((1u << RESPONSE_TABLE_BITS) - 1u);
    unsigned first = response_table[index];
    unsigned second = response_table[index + 1];
    return (uint16_t)((first * ((1u << RESPONSE_TABLE_BITS) - fraction) +
                       second * fraction +
                       (1u << (RESPONSE_TABLE_BITS - 1))) >>
                      RESPONSE_TABLE_BITS);
}

static uint16_t inverse_perceptual_response(int response) {
    return response_to_linear[clamp_u16(response)];
}

void n148_rgb_to_perceptual(const uint8_t *rgb, long count,
                            uint8_t *intensity, uint8_t *red_green,
                            uint8_t *blue_yellow) {
    for (long pixel = 0; pixel < count; pixel++) {
        uint16_t linear[3] = {
            srgb_to_linear[rgb[pixel * 3]],
            srgb_to_linear[rgb[pixel * 3 + 1]],
            srgb_to_linear[rgb[pixel * 3 + 2]],
        };
        uint16_t response[3];
        for (int cone = 0; cone < 3; cone++) {
            uint32_t sum = 0;
            for (int channel = 0; channel < 3; channel++)
                sum += (uint32_t) cone_matrix[cone][channel] * linear[channel];
            response[cone] = perceptual_response(
                (uint16_t)((sum + 2048) >> 12));
        }
        int average = (response[0] + response[1] + 1) / 2;
        int x = (int) response[0] - response[1];
        int blue_difference = (int) response[2] - average;
        intensity[pixel] = clamp_byte((average + 128) / 257);
        red_green[pixel] = clamp_byte(
            XYB_X_NEUTRAL +
            rounded_signed_division(x, XYB_X_SCALE));
        blue_yellow[pixel] = clamp_byte(
            XYB_B_NEUTRAL +
            rounded_signed_division(blue_difference, XYB_B_SCALE));
    }
}

void n148_perceptual_to_rgb(uint8_t intensity,
                            uint16_t red_green_scaled,
                            uint16_t blue_yellow_scaled,
                            uint8_t rgb[3]) {
    int average = intensity * 257;
    int x = rounded_signed_division(
        ((int) red_green_scaled -
         (XYB_X_NEUTRAL << CHROMA_FRACTION_BITS)) * XYB_X_SCALE,
        1 << CHROMA_FRACTION_BITS);
    int blue_difference = rounded_signed_division(
        ((int) blue_yellow_scaled -
         (XYB_B_NEUTRAL << CHROMA_FRACTION_BITS)) * XYB_B_SCALE,
        1 << CHROMA_FRACTION_BITS);
    int responses[3] = {
        average + rounded_signed_division(x, 2),
        average - rounded_signed_division(x, 2),
        average + blue_difference,
    };
    uint16_t cones[3] = {
        inverse_perceptual_response(responses[0]),
        inverse_perceptual_response(responses[1]),
        inverse_perceptual_response(responses[2]),
    };
    for (int channel = 0; channel < 3; channel++) {
        int64_t sum = 0;
        for (int cone = 0; cone < 3; cone++)
            sum += (int64_t) cone_inverse_q20[channel][cone] * cones[cone];
        int linear = rounded_signed_division(sum, 1 << 20);
        rgb[channel] = linear_to_srgb[clamp_u16(linear)];
    }
}

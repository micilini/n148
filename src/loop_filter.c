/*
 * N.148i causal deblocking and deringing filter.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * This original filter is deliberately small and integer-only. It operates
 * as part of reconstructed-neighbor prediction, so its exact order and
 * rounding are normative for streams carrying the LOOP_FILTER flag.
 */

#include "loop_filter.h"

#include <stdlib.h>
#include <string.h>

#include "adaptive_quant.h"
#include "cpu.h"
#include "tables.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

static int absolute(int value) {
    return value < 0 ? -value : value;
}

static int clamp_integer(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static uint8_t clamp_byte(int value) {
    return (uint8_t) clamp_integer(value, 0, 255);
}

static int rounded_signed_division(int value, int divisor) {
    return value >= 0 ? (value + divisor / 2) / divisor :
        -((-value + divisor / 2) / divisor);
}

int n148_loop_filter_strength(int quality, int plane_index,
                              uint8_t adaptive_level) {
    if (quality < 1 || quality > 100 || plane_index < 0 || plane_index > 2 ||
        adaptive_level >= N148_AQ_LEVEL_COUNT) return -1;
    if (quality >= 96) return 0;
    int loss = 100 - quality;
    int strength = (loss + 19) / 20;
    if (adaptive_level == 0 && strength > 1) strength--;
    if (adaptive_level == 3) strength++;
    if (plane_index != 0 && strength > 1)
        strength = (strength * 3 + 2) / 4;
    if (strength > 5) strength = 5;
    return strength;
}

int n148_loop_filter_quant_luma_strength(int quality) {
    if (quality < 1 || quality > 100) return -1;
    int table[8][8];
    scale_table(FORMAT_5_LUMA_QUANT_BASE, quality, table);
    int quant_step = table[0][0];
    if (quant_step <= 2) return 0;
    int strength = (quant_step + 8) / 9;
    return strength > 5 ? 5 : strength;
}

static int edge_threshold(int quality, uint8_t adaptive_level) {
    int threshold = 3 + (100 - quality + 3) / 4;
    static const int scale[N148_AQ_LEVEL_COUNT] = {90, 97, 108, 118};
    threshold = (threshold * scale[adaptive_level] + 50) / 100;
    return threshold < 3 ? 3 : threshold;
}

static int filter_edge_sample(uint8_t *p1, uint8_t *p0,
                              uint8_t *q0, uint8_t *q1,
                              int strength, int threshold) {
    int p1_value = *p1;
    int p0_value = *p0;
    int q0_value = *q0;
    int q1_value = *q1;
    int gap = absolute(q0_value - p0_value);
    int side_activity = absolute(p0_value - p1_value) +
        absolute(q1_value - q0_value);
    if (gap < 2 || gap > threshold ||
        side_activity > threshold + strength) return 0;

    int delta = rounded_signed_division(
        3 * (q0_value - p0_value) + p1_value - q1_value, 8);
    delta = clamp_integer(delta, -strength, strength);
    if (delta == 0) return 0;
    *p0 = clamp_byte(p0_value + delta);
    *q0 = clamp_byte(q0_value - delta);
    if (strength >= 3 && side_activity <= threshold / 2) {
        int secondary = rounded_signed_division(delta, 2);
        *p1 = clamp_byte(p1_value + secondary);
        *q1 = clamp_byte(q1_value - secondary);
    }
    return 1;
}

#if defined(__x86_64__) || defined(__i386__)
/* A vertical edge has four contiguous source bytes per row. Gather eight
   rows, filter them together, then scatter the four bytes back per row. */
__attribute__((target("avx2")))
static uint64_t filter_vertical_simd(Plane *plane, int x0, int y0,
                                     int height, int strength, int threshold,
                                     int count_changes) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i two = _mm_set1_epi16(2);
    const __m128i three = _mm_set1_epi16(3);
    const __m128i four = _mm_set1_epi16(4);
    const __m128i one = _mm_set1_epi16(1);
    const __m128i limit = _mm_set1_epi16((short)strength);
    const __m128i minus_limit = _mm_sub_epi16(zero, limit);
    const __m128i edge_limit = _mm_set1_epi16((short)threshold);
    const __m128i activity_limit = _mm_set1_epi16(
        (short)(threshold + strength));
    const __m128i secondary_limit = _mm_set1_epi16((short)(threshold / 2));
    const __m128i masks[4] = {
        _mm_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1,
                      -1, -1, -1, -1, -1, -1, -1, -1),
        _mm_setr_epi8(1, 5, 9, 13, -1, -1, -1, -1,
                      -1, -1, -1, -1, -1, -1, -1, -1),
        _mm_setr_epi8(2, 6, 10, 14, -1, -1, -1, -1,
                      -1, -1, -1, -1, -1, -1, -1, -1),
        _mm_setr_epi8(3, 7, 11, 15, -1, -1, -1, -1,
                      -1, -1, -1, -1, -1, -1, -1, -1),
    };
    uint64_t changed = 0;
    int row = 0;
    for (; row + 8 <= height; row += 8) {
        uint8_t *edge[8];
        __m128i pixels[8];
        for (int i = 0; i < 8; i++) {
            edge[i] = plane->data +
                (long)(y0 + row + i) * plane->width + x0;
            pixels[i] = _mm_loadu_si32(edge[i] - 2);
        }
        __m128i first = _mm_unpacklo_epi64(
            _mm_unpacklo_epi32(pixels[0], pixels[1]),
            _mm_unpacklo_epi32(pixels[2], pixels[3]));
        __m128i second = _mm_unpacklo_epi64(
            _mm_unpacklo_epi32(pixels[4], pixels[5]),
            _mm_unpacklo_epi32(pixels[6], pixels[7]));
        __m128i values[4];
        for (int i = 0; i < 4; i++) {
            __m128i lo = _mm_shuffle_epi8(first, masks[i]);
            __m128i hi = _mm_shuffle_epi8(second, masks[i]);
            values[i] = _mm_unpacklo_epi8(
                _mm_or_si128(lo, _mm_slli_si128(hi, 4)), zero);
        }
        __m128i p1 = values[0], p0 = values[1];
        __m128i q0 = values[2], q1 = values[3];
        __m128i gap = _mm_abs_epi16(_mm_sub_epi16(q0, p0));
        __m128i side = _mm_add_epi16(
            _mm_abs_epi16(_mm_sub_epi16(p0, p1)),
            _mm_abs_epi16(_mm_sub_epi16(q1, q0)));
        __m128i blocked = _mm_or_si128(
            _mm_or_si128(_mm_cmpgt_epi16(two, gap),
                         _mm_cmpgt_epi16(gap, edge_limit)),
            _mm_cmpgt_epi16(side, activity_limit));
        __m128i numerator = _mm_add_epi16(
            _mm_mullo_epi16(_mm_sub_epi16(q0, p0), three),
            _mm_sub_epi16(p1, q1));
        __m128i magnitude = _mm_srli_epi16(
            _mm_add_epi16(_mm_abs_epi16(numerator), four), 3);
        __m128i delta = _mm_blendv_epi8(
            magnitude, _mm_sub_epi16(zero, magnitude),
            _mm_cmpgt_epi16(zero, numerator));
        delta = _mm_min_epi16(_mm_max_epi16(delta, minus_limit), limit);
        __m128i active = _mm_andnot_si128(
            _mm_or_si128(blocked, _mm_cmpeq_epi16(delta, zero)),
            _mm_set1_epi16(-1));
        p0 = _mm_blendv_epi8(p0, _mm_add_epi16(p0, delta), active);
        q0 = _mm_blendv_epi8(q0, _mm_sub_epi16(q0, delta), active);
        if (strength >= 3) {
            __m128i secondary_mask = _mm_andnot_si128(
                _mm_cmpgt_epi16(side, secondary_limit), active);
            __m128i half_magnitude = _mm_srli_epi16(
                _mm_add_epi16(_mm_abs_epi16(delta), one), 1);
            __m128i secondary = _mm_blendv_epi8(
                half_magnitude, _mm_sub_epi16(zero, half_magnitude),
                _mm_cmpgt_epi16(zero, delta));
            p1 = _mm_blendv_epi8(p1, _mm_add_epi16(p1, secondary),
                                 secondary_mask);
            q1 = _mm_blendv_epi8(q1, _mm_sub_epi16(q1, secondary),
                                 secondary_mask);
        }
        __m128i left = _mm_unpacklo_epi8(
            _mm_packus_epi16(p1, p1), _mm_packus_epi16(p0, p0));
        __m128i right = _mm_unpacklo_epi8(
            _mm_packus_epi16(q0, q0), _mm_packus_epi16(q1, q1));
        __m128i out0 = _mm_unpacklo_epi16(left, right);
        __m128i out1 = _mm_unpackhi_epi16(left, right);
        uint32_t packed[8];
        _mm_storeu_si128((__m128i *)packed, out0);
        _mm_storeu_si128((__m128i *)(packed + 4), out1);
        for (int i = 0; i < 8; i++)
            memcpy(edge[i] - 2, packed + i, sizeof(packed[i]));
        if (count_changes)
            changed += (uint64_t)__builtin_popcount(
                (unsigned)_mm_movemask_epi8(active)) / 2u;
    }
    for (; row < height; row++) {
        uint8_t *edge = plane->data +
            (long)(y0 + row) * plane->width + x0;
        int sample_changed = filter_edge_sample(
            edge - 2, edge - 1, edge, edge + 1, strength, threshold);
        if (count_changes) changed += (uint64_t)sample_changed;
    }
    return changed;
}

__attribute__((target("avx2")))
static uint64_t filter_horizontal_simd(Plane *plane, int x0, int y0,
                                       int width, int strength, int threshold,
                                       int count_changes) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i two = _mm_set1_epi16(2);
    const __m128i three = _mm_set1_epi16(3);
    const __m128i four = _mm_set1_epi16(4);
    const __m128i one = _mm_set1_epi16(1);
    const __m128i limit = _mm_set1_epi16((short) strength);
    const __m128i minus_limit = _mm_sub_epi16(zero, limit);
    const __m128i edge_limit = _mm_set1_epi16((short) threshold);
    const __m128i activity_limit = _mm_set1_epi16(
        (short)(threshold + strength));
    const __m128i secondary_limit = _mm_set1_epi16((short)(threshold / 2));
    uint64_t changed = 0;
    for (int column = 0; column < width; column += 8) {
        uint8_t *q0_ptr = plane->data + (long)y0 * plane->width + x0 + column;
        uint8_t *p0_ptr = q0_ptr - plane->width;
        uint8_t *p1_ptr = p0_ptr - plane->width;
        uint8_t *q1_ptr = q0_ptr + plane->width;
        __m128i p1 = _mm_unpacklo_epi8(
            _mm_loadl_epi64((const __m128i *)p1_ptr), zero);
        __m128i p0 = _mm_unpacklo_epi8(
            _mm_loadl_epi64((const __m128i *)p0_ptr), zero);
        __m128i q0 = _mm_unpacklo_epi8(
            _mm_loadl_epi64((const __m128i *)q0_ptr), zero);
        __m128i q1 = _mm_unpacklo_epi8(
            _mm_loadl_epi64((const __m128i *)q1_ptr), zero);
        __m128i gap = _mm_abs_epi16(_mm_sub_epi16(q0, p0));
        __m128i side = _mm_add_epi16(
            _mm_abs_epi16(_mm_sub_epi16(p0, p1)),
            _mm_abs_epi16(_mm_sub_epi16(q1, q0)));
        __m128i blocked = _mm_or_si128(
            _mm_or_si128(_mm_cmpgt_epi16(two, gap),
                         _mm_cmpgt_epi16(gap, edge_limit)),
            _mm_cmpgt_epi16(side, activity_limit));
        __m128i numerator = _mm_add_epi16(
            _mm_mullo_epi16(_mm_sub_epi16(q0, p0), three),
            _mm_sub_epi16(p1, q1));
        __m128i magnitude = _mm_srli_epi16(
            _mm_add_epi16(_mm_abs_epi16(numerator), four), 3);
        __m128i negative = _mm_cmpgt_epi16(zero, numerator);
        __m128i delta = _mm_blendv_epi8(
            magnitude, _mm_sub_epi16(zero, magnitude), negative);
        delta = _mm_min_epi16(_mm_max_epi16(delta, minus_limit), limit);
        __m128i active = _mm_andnot_si128(
            _mm_or_si128(blocked, _mm_cmpeq_epi16(delta, zero)),
            _mm_set1_epi16(-1));
        __m128i new_p0 = _mm_blendv_epi8(
            p0, _mm_add_epi16(p0, delta), active);
        __m128i new_q0 = _mm_blendv_epi8(
            q0, _mm_sub_epi16(q0, delta), active);
        _mm_storel_epi64((__m128i *)p0_ptr,
                         _mm_packus_epi16(new_p0, new_p0));
        _mm_storel_epi64((__m128i *)q0_ptr,
                         _mm_packus_epi16(new_q0, new_q0));
        if (strength >= 3) {
            __m128i secondary_mask = _mm_andnot_si128(
                _mm_cmpgt_epi16(side, secondary_limit), active);
            __m128i half_magnitude = _mm_srli_epi16(
                _mm_add_epi16(_mm_abs_epi16(delta), one), 1);
            __m128i secondary = _mm_blendv_epi8(
                half_magnitude, _mm_sub_epi16(zero, half_magnitude),
                _mm_cmpgt_epi16(zero, delta));
            __m128i new_p1 = _mm_blendv_epi8(
                p1, _mm_add_epi16(p1, secondary), secondary_mask);
            __m128i new_q1 = _mm_blendv_epi8(
                q1, _mm_sub_epi16(q1, secondary), secondary_mask);
            _mm_storel_epi64((__m128i *)p1_ptr,
                             _mm_packus_epi16(new_p1, new_p1));
            _mm_storel_epi64((__m128i *)q1_ptr,
                             _mm_packus_epi16(new_q1, new_q1));
        }
        if (count_changes)
            changed += (uint64_t)__builtin_popcount(
                (unsigned)_mm_movemask_epi8(active)) / 2u;
    }
    return changed;
}

__attribute__((target("avx2")))
static uint64_t dering_square_avx2(Plane *plane, int x0, int y0, int size,
                                   int strength, int threshold,
                                   const uint8_t *source,
                                   int count_changes) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i two = _mm256_set1_epi16(2);
    const __m256i limit = _mm256_set1_epi16((short) strength);
    const __m256i negative_limit = _mm256_sub_epi16(zero, limit);
    const __m256i edge_threshold = _mm256_set1_epi16((short) threshold);
    const __m256i first_mask = _mm256_setr_epi16(
        0, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i last_mask = _mm256_setr_epi16(
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, 0);
    const __m256i paired_8_mask = _mm256_setr_epi16(
        0, -1, -1, -1, -1, -1, -1, 0,
        0, -1, -1, -1, -1, -1, -1, 0);
    uint64_t changed = 0;
    /* Pair adjacent interior rows for 8x8 blocks. Larger blocks process
       sixteen horizontal samples at a time. */
    int paired_rows = size == 8;
    for (int row = 1; row + 1 < size; row += paired_rows ? 2 : 1) {
        for (int column = 0; column < size; column += 16) {
            const uint8_t *p = source + row * size + column;
            __m256i interior = paired_rows ? paired_8_mask :
                _mm256_set1_epi16(-1);
            if (!paired_rows && column == 0)
                interior = _mm256_and_si256(interior, first_mask);
            if (!paired_rows && column + 16 == size)
                interior = _mm256_and_si256(interior, last_mask);
            __m256i center = _mm256_cvtepu8_epi16(
                _mm_loadu_si128((const __m128i *) p));
            __m256i left = _mm256_cvtepu8_epi16(
                _mm_loadu_si128((const __m128i *)(p - 1)));
            __m256i right = _mm256_cvtepu8_epi16(
                _mm_loadu_si128((const __m128i *)(p + 1)));
            __m256i top = _mm256_cvtepu8_epi16(
                _mm_loadu_si128((const __m128i *)(p - size)));
            __m256i bottom = _mm256_cvtepu8_epi16(
                _mm_loadu_si128((const __m128i *)(p + size)));
            __m256i horizontal = _mm256_abs_epi16(
                _mm256_sub_epi16(left, right));
            __m256i vertical = _mm256_abs_epi16(
                _mm256_sub_epi16(top, bottom));
            __m256i use_vertical = _mm256_cmpgt_epi16(horizontal, vertical);
            __m256i first = _mm256_blendv_epi8(left, top, use_vertical);
            __m256i second = _mm256_blendv_epi8(right, bottom, use_vertical);
            __m256i gap = _mm256_abs_epi16(_mm256_sub_epi16(first, second));
            __m256i within_threshold = _mm256_andnot_si256(
                _mm256_cmpgt_epi16(gap, edge_threshold), interior);
            __m256i minimum = _mm256_min_epi16(first, second);
            __m256i maximum = _mm256_max_epi16(first, second);
            __m256i outside = _mm256_or_si256(
                _mm256_cmpgt_epi16(_mm256_sub_epi16(minimum, limit), center),
                _mm256_cmpgt_epi16(center, _mm256_add_epi16(maximum, limit)));
            __m256i average = _mm256_srli_epi16(_mm256_add_epi16(
                _mm256_add_epi16(first, second),
                _mm256_add_epi16(_mm256_slli_epi16(center, 1), two)), 2);
            __m256i delta = _mm256_sub_epi16(average, center);
            delta = _mm256_min_epi16(
                _mm256_max_epi16(delta, negative_limit), limit);
            __m256i nonzero = _mm256_andnot_si256(
                _mm256_cmpeq_epi16(delta, zero), interior);
            __m256i change_mask = _mm256_and_si256(
                _mm256_and_si256(within_threshold, outside), nonzero);
            __m256i result = _mm256_blendv_epi8(
                center, _mm256_add_epi16(center, delta), change_mask);
            __m128i packed = _mm_packus_epi16(
                _mm256_castsi256_si128(result),
                _mm256_extracti128_si256(result, 1));
            uint8_t *destination = plane->data +
                (long)(y0 + row) * plane->width + x0 + column;
            if (paired_rows) {
                _mm_storel_epi64((__m128i *)destination, packed);
                _mm_storel_epi64((__m128i *)(destination + plane->width),
                                 _mm_srli_si128(packed, 8));
            } else {
                _mm_storeu_si128((__m128i *)destination, packed);
            }
            if (count_changes)
                changed += (uint64_t) __builtin_popcount(
                    (unsigned) _mm256_movemask_epi8(change_mask)) / 2u;
        }
    }
    return changed;
}
/* Original exact byte-domain implementation of N.148's existing deringer.
 * All samples and bounded results remain in [0,255]. */
__attribute__((target("avx2"), always_inline))
static inline __m256i dering_bytes_vector(
    __m256i center, __m256i left, __m256i right, __m256i top,
    __m256i bottom, __m256i interior, int strength, int threshold,
    __m256i *changed) {
    const __m256i one = _mm256_set1_epi8(1);
    const __m256i limit = _mm256_set1_epi8((char)strength);
    const __m256i edge = _mm256_set1_epi8((char)threshold);
    __m256i horizontal = _mm256_or_si256(
        _mm256_subs_epu8(left,right), _mm256_subs_epu8(right,left));
    __m256i vertical = _mm256_or_si256(
        _mm256_subs_epu8(top,bottom), _mm256_subs_epu8(bottom,top));
    __m256i gap = _mm256_min_epu8(horizontal,vertical);
    __m256i use_horizontal = _mm256_cmpeq_epi8(gap,horizontal);
    __m256i first = _mm256_blendv_epi8(top,left,use_horizontal);
    __m256i second = _mm256_blendv_epi8(bottom,right,use_horizontal);
    __m256i within = _mm256_cmpeq_epi8(_mm256_min_epu8(gap,edge),gap);
    __m256i minimum = _mm256_subs_epu8(_mm256_min_epu8(first,second),limit);
    __m256i maximum = _mm256_adds_epu8(_mm256_max_epu8(first,second),limit);
    __m256i clipped = _mm256_min_epu8(_mm256_max_epu8(center,minimum),maximum);
    __m256i outside = _mm256_andnot_si256(
        _mm256_cmpeq_epi8(clipped,center),interior);
    /* avg_epu8 rounds upward. Its two-stage result differs from the
       required floor((first+second+2*center+2)/4) only when that numerator
       without +2 is 1 modulo 4. The parity correction removes exactly 1. */
    __m256i neighbor_average = _mm256_avg_epu8(first,second);
    __m256i correction = _mm256_and_si256(one,_mm256_and_si256(
        _mm256_xor_si256(first,second),
        _mm256_xor_si256(center,neighbor_average)));
    __m256i average = _mm256_sub_epi8(
        _mm256_avg_epu8(center,neighbor_average),correction);
    __m256i result = _mm256_min_epu8(
        _mm256_max_epu8(average,_mm256_subs_epu8(center,limit)),
        _mm256_adds_epu8(center,limit));
    __m256i active = _mm256_and_si256(within,outside);
    *changed = _mm256_andnot_si256(_mm256_cmpeq_epi8(result,center),active);
    return _mm256_blendv_epi8(center,result,active);
}

__attribute__((target("avx2"), always_inline))
static inline __m256i load_dering_bytes(const uint8_t *p, int half) {
    return half ? _mm256_inserti128_si256(_mm256_setzero_si256(),
        _mm_loadu_si128((const __m128i *)p),0) :
        _mm256_loadu_si256((const __m256i *)p);
}

__attribute__((target("avx2")))
static uint64_t dering_square_bytes(Plane *plane, int x0, int y0,
    int size, int strength, int threshold, const uint8_t *source,
    int count_changes) {
    __m256i interior;
    if (size == 8) interior = _mm256_set1_epi64x(0x00ffffffffffff00ll);
    else if (size == 16) interior = _mm256_setr_epi64x(
        -256ll,0x00ffffffffffffffll,-256ll,0x00ffffffffffffffll);
    else interior = _mm256_setr_epi64x(-256ll,-1ll,-1ll,0x00ffffffffffffffll);
    uint64_t changed_count = 0;
    int step = 32 / size;
    for (int row = 1; row + 1 < size; row += step) {
        int half = size == 8 && row == 5;
        int rows = half ? 2 : step;
        const uint8_t *p = source + row*size;
        __m256i changed;
        __m256i result = dering_bytes_vector(
            load_dering_bytes(p,half), load_dering_bytes(p-1,half),
            load_dering_bytes(p+1,half), load_dering_bytes(p-size,half),
            load_dering_bytes(p+size,half),interior,strength,threshold,&changed);
        uint8_t *destination = plane->data+(long)(y0+row)*plane->width+x0;
        __m128i lo = _mm256_castsi256_si128(result);
        __m128i hi = _mm256_extracti128_si256(result,1);
        if (size == 8) {
            _mm_storel_epi64((__m128i *)destination,lo);
            _mm_storel_epi64((__m128i *)(destination+plane->width),_mm_srli_si128(lo,8));
            if (rows == 4) {
                _mm_storel_epi64((__m128i *)(destination+2*plane->width),hi);
                _mm_storel_epi64((__m128i *)(destination+3*plane->width),_mm_srli_si128(hi,8));
            }
        } else if (size == 16) {
            _mm_storeu_si128((__m128i *)destination,lo);
            _mm_storeu_si128((__m128i *)(destination+plane->width),hi);
        } else _mm256_storeu_si256((__m256i *)destination,result);
        if (count_changes) {
            unsigned mask = (unsigned)_mm256_movemask_epi8(changed);
            if (half) mask &= 0xffffu;
            changed_count += (uint64_t)__builtin_popcount(mask);
        }
    }
    return changed_count;
}

#endif

static uint64_t dering_square(Plane *plane, int x0, int y0, int width,
                              int height, int strength, int threshold,
                              int count_changes, int use_avx2) {
    if (width < 3 || height < 3 || strength <= 0) return 0;
    uint8_t source[32 * 32];
    for (int row = 0; row < height; row++) {
        memcpy(source + row * width,
               plane->data + (long)(y0 + row) * plane->width + x0,
               (size_t) width);
    }
#if defined(__x86_64__) || defined(__i386__)
    if (width == height &&
        (width == 8 || width == 16 || width == 32) &&
        use_avx2) {
        if (strength <= 255 && threshold >= 0 && threshold <= 255)
            return dering_square_bytes(plane, x0, y0, width, strength,
                                       threshold, source, count_changes);
        return dering_square_avx2(plane, x0, y0, width, strength,
                                  threshold, source, count_changes);
    }
#endif
    (void) use_avx2;
    uint64_t changed = 0;
    for (int row = 1; row + 1 < height; row++) {
        for (int column = 1; column + 1 < width; column++) {
            int center = source[row * width + column];
            int left = source[row * width + column - 1];
            int right = source[row * width + column + 1];
            int top = source[(row - 1) * width + column];
            int bottom = source[(row + 1) * width + column];
            int first, second;
            if (absolute(left - right) <= absolute(top - bottom)) {
                first = left;
                second = right;
            } else {
                first = top;
                second = bottom;
            }
            if (absolute(first - second) > threshold) continue;
            int minimum = first < second ? first : second;
            int maximum = first > second ? first : second;
            int limited = clamp_integer(center, minimum - strength,
                                        maximum + strength);
            if (limited == center) continue;
            int average = (first + 2 * center + second + 2) / 4;
            int delta = clamp_integer(average - center,
                                      -strength, strength);
            if (delta == 0) continue;
            plane->data[(long)(y0 + row) * plane->width + x0 + column] =
                clamp_byte(center + delta);
            if (count_changes) changed++;
        }
    }
    return changed;
}

int n148_loop_filter_prepare(int quality, int plane_index,
                             uint8_t adaptive_level, int quant_adaptive_luma,
                             N148LoopFilterPrepared *prepared) {
    if (!prepared || quality < 1 || quality > 100 ||
        plane_index < 0 || plane_index > 2 ||
        adaptive_level >= N148_AQ_LEVEL_COUNT ||
        (quant_adaptive_luma != 0 && quant_adaptive_luma != 1)) return 0;
    prepared->strength = quant_adaptive_luma && plane_index == 0 ?
        n148_loop_filter_quant_luma_strength(quality) :
        n148_loop_filter_strength(quality, plane_index, adaptive_level);
    if (prepared->strength < 0) return 0;
    prepared->threshold = edge_threshold(quality, adaptive_level);
    prepared->use_avx2 = 0;
#if defined(__x86_64__) || defined(__i386__)
    if (prepared->strength > 0)
        prepared->use_avx2 = n148_cpu_level() >= N148_CPU_AVX2;
#endif
    return 1;
}

int n148_loop_filter_block_prepared(Plane *plane, int x0, int y0, int size,
                                    const N148LoopFilterPrepared *prepared,
                                    N148LoopFilterStats *stats) {
    if (!plane || !plane->data || plane->width <= 0 || plane->height <= 0 ||
        x0 < 0 || y0 < 0 || x0 >= plane->width || y0 >= plane->height ||
        (size != 8 && size != 16 && size != 32) || !prepared ||
        prepared->strength < 0) return 0;
    int strength = prepared->strength;
    int width = plane->width - x0;
    int height = plane->height - y0;
    if (width > size) width = size;
    if (height > size) height = size;
    if (width <= 0 || height <= 0 || strength == 0) return 1;
    int threshold = prepared->threshold;
    int use_avx2 = prepared->use_avx2;

    uint64_t deringed = dering_square(
        plane, x0, y0, width, height, strength, threshold,
        stats != NULL, use_avx2);
    uint64_t deblocked = 0;
    if (x0 >= 2 && x0 + 1 < plane->width) {
#if defined(__x86_64__) || defined(__i386__)
        if (height >= 8 && use_avx2) {
            deblocked += filter_vertical_simd(
                plane, x0, y0, height, strength, threshold, stats != NULL);
        } else
#endif
        for (int row = 0; row < height; row++) {
            uint8_t *edge = plane->data +
                (long)(y0 + row) * plane->width + x0;
            int changed = filter_edge_sample(
                edge - 2, edge - 1, edge, edge + 1,
                strength, threshold);
            if (stats) deblocked += (uint64_t) changed;
        }
    }
    if (y0 >= 2 && y0 + 1 < plane->height) {
#if defined(__x86_64__) || defined(__i386__)
        if (width >= 8 && width % 8 == 0 && use_avx2) {
            deblocked += filter_horizontal_simd(
                plane, x0, y0, width, strength, threshold, stats != NULL);
        } else
#endif
        for (int column = 0; column < width; column++) {
            uint8_t *edge = plane->data + (long) y0 * plane->width +
                x0 + column;
            int changed = filter_edge_sample(
                edge - 2 * plane->width, edge - plane->width,
                edge, edge + plane->width, strength, threshold);
            if (stats) deblocked += (uint64_t) changed;
        }
    }
    if (stats) {
        stats->deblocked_samples += deblocked;
        stats->deringed_samples += deringed;
    }
    return 1;
}

static int loop_filter_block(Plane *plane, int x0, int y0, int size,
                             int quality, int plane_index,
                             uint8_t adaptive_level, int quant_adaptive_luma,
                             N148LoopFilterStats *stats) {
    N148LoopFilterPrepared prepared;
    return n148_loop_filter_prepare(quality, plane_index, adaptive_level,
                                     quant_adaptive_luma, &prepared) &&
           n148_loop_filter_block_prepared(
               plane, x0, y0, size, &prepared, stats);
}

int n148_loop_filter_block(Plane *plane, int x0, int y0, int size,
                           int quality, int plane_index,
                           uint8_t adaptive_level,
                           N148LoopFilterStats *stats) {
    return loop_filter_block(plane, x0, y0, size, quality, plane_index,
                             adaptive_level, 0, stats);
}

int n148_loop_filter_quant_luma_block(Plane *plane, int x0, int y0, int size,
                              int quality, int plane_index,
                              N148LoopFilterStats *stats) {
    return loop_filter_block(plane, x0, y0, size, quality, plane_index,
                             1, 1, stats);
}

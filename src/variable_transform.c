/*
 * N.148i variable transform map and deterministic integer DCT.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * The implementation is intentionally compact and original. Literal Q14
 * matrices make the transform independent of the platform math library.
 */

#include "variable_transform.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "header.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#define N148_DCT_SCALE_SQUARED 268435456ll
#if defined(__x86_64__) || defined(__i386__)
#include "dct_forward_transposes.inc"
#endif

static const int16_t DCT4_Q14[4][4] = {
    {8192, 8192, 8192, 8192},
    {10703, 4433, -4433, -10703},
    {8192, -8192, -8192, 8192},
    {4433, -10703, 10703, -4433},
};

/* Continuous synthesis energy of our native Q14 basis, before
   rounding to integer samples. This stays positive even at unit steps. */
double n148_variable_dct4_energy(int natural, int step) {
    double row_norm = 0.0, column_norm = 0.0;
    for (int sample = 0; sample < 4; ++sample) {
        double row = DCT4_Q14[natural / 4][sample];
        double column = DCT4_Q14[natural % 4][sample];
        row_norm += row * row;
        column_norm += column * column;
    }
    return (double)step * step *
        (row_norm / 268435456.0) * (column_norm / 268435456.0);
}

static const int16_t DCT16_Q14[16][16] = {
    {4096,4096,4096,4096,4096,4096,4096,4096,
     4096,4096,4096,4096,4096,4096,4096,4096},
    {5765,5543,5109,4478,3675,2731,1682,568,
     -568,-1682,-2731,-3675,-4478,-5109,-5543,-5765},
    {5681,4816,3218,1130,-1130,-3218,-4816,-5681,
     -5681,-4816,-3218,-1130,1130,3218,4816,5681},
    {5543,3675,568,-2731,-5109,-5765,-4478,-1682,
     1682,4478,5765,5109,2731,-568,-3675,-5543},
    {5352,2217,-2217,-5352,-5352,-2217,2217,5352,
     5352,2217,-2217,-5352,-5352,-2217,2217,5352},
    {5109,568,-4478,-5543,-1682,3675,5765,2731,
     -2731,-5765,-3675,1682,5543,4478,-568,-5109},
    {4816,-1130,-5681,-3218,3218,5681,1130,-4816,
     -4816,1130,5681,3218,-3218,-5681,-1130,4816},
    {4478,-2731,-5543,568,5765,1682,-5109,-3675,
     3675,5109,-1682,-5765,-568,5543,2731,-4478},
    {4096,-4096,-4096,4096,4096,-4096,-4096,4096,
     4096,-4096,-4096,4096,4096,-4096,-4096,4096},
    {3675,-5109,-1682,5765,-568,-5543,2731,4478,
     -4478,-2731,5543,568,-5765,1682,5109,-3675},
    {3218,-5681,1130,4816,-4816,-1130,5681,-3218,
     -3218,5681,-1130,-4816,4816,1130,-5681,3218},
    {2731,-5765,3675,1682,-5543,4478,568,-5109,
     5109,-568,-4478,5543,-1682,-3675,5765,-2731},
    {2217,-5352,5352,-2217,-2217,5352,-5352,2217,
     2217,-5352,5352,-2217,-2217,5352,-5352,2217},
    {1682,-4478,5765,-5109,2731,568,-3675,5543,
     -5543,3675,-568,-2731,5109,-5765,4478,-1682},
    {1130,-3218,4816,-5681,5681,-4816,3218,-1130,
     -1130,3218,-4816,5681,-5681,4816,-3218,1130},
    {568,-1682,2731,-3675,4478,-5109,5543,-5765,
     5765,-5543,5109,-4478,3675,-2731,1682,-568},
};

#include "dct32_q14.inc"

static int checked_plane_geometry(int width, int height, int *blocks_x,
                                  int *blocks_y, int *columns, int *rows,
                                  size_t *count) {
    if (width <= 0 || height <= 0 || !blocks_x || !blocks_y || !columns ||
        !rows || !count) return 0;
    *blocks_x = width / 8 + (width % 8 != 0);
    *blocks_y = height / 8 + (height % 8 != 0);
    *columns = (*blocks_x + 1) / 2;
    *rows = (*blocks_y + 1) / 2;
    if (*columns <= 0 || *rows <= 0 ||
        (size_t) *columns > SIZE_MAX / (size_t) *rows) return 0;
    *count = (size_t) *columns * (size_t) *rows;
    return *count <= UINT32_MAX;
}

int n148_transform_map_allocate(int width, int height, int chroma,
                                N148TransformMap *map) {
    if (!map || chroma < CHROMA_444 || chroma > CHROMA_420) return 0;
    memset(map, 0, sizeof(*map));
    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    int widths[3] = {width, chroma_width, chroma_width};
    int heights[3] = {height, chroma_height, chroma_height};
    size_t total = 0;
    for (int plane = 0; plane < 3; plane++) {
        size_t plane_count;
        map->plane_offsets[plane] = total;
        if (!checked_plane_geometry(
                widths[plane], heights[plane], &map->blocks_x[plane],
                &map->blocks_y[plane], &map->columns[plane],
                &map->rows[plane], &plane_count) ||
            plane_count > SIZE_MAX - total) {
            memset(map, 0, sizeof(*map));
            return 0;
        }
        total += plane_count;
    }
    if (total == 0 || total > UINT32_MAX) {
        memset(map, 0, sizeof(*map));
        return 0;
    }
    map->strategies = (uint8_t *) malloc(total);
    if (!map->strategies) {
        memset(map, 0, sizeof(*map));
        return 0;
    }
    memset(map->strategies, N148_TRANSFORM_INITIAL_STRATEGY, total);
    map->count = total;
    return 1;
}

static uint16_t transform_activity(const Plane *plane, int x0, int y0) {
    int x1 = x0 + N148_TRANSFORM_REGION_SIZE;
    int y1 = y0 + N148_TRANSFORM_REGION_SIZE;
    if (x1 > plane->width) x1 = plane->width;
    if (y1 > plane->height) y1 = plane->height;
    uint32_t total = 0;
    uint32_t samples = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t *row = plane->data + (long) y * plane->width;
        for (int x = x0; x < x1; x++) {
            if (x >= x0 + 2) {
                int value = (int) row[x] - 2 * (int) row[x - 1] + row[x - 2];
                if (value < 0) value = -value;
                if (value > 32) value = 32;
                total += (uint32_t) value;
                samples++;
            }
            if (y >= y0 + 2) {
                int value = (int) row[x] -
                    2 * (int) plane->data[(long)(y - 1) * plane->width + x] +
                    plane->data[(long)(y - 2) * plane->width + x];
                if (value < 0) value = -value;
                if (value > 32) value = 32;
                total += (uint32_t) value;
                samples++;
            }
        }
    }
    return samples ? (uint16_t)((total * 256u + samples / 2u) / samples) : 0;
}

static int build_plane_map(const Plane *plane, int plane_index,
                           N148TransformMap *map) {
    if (!plane || !plane->data || !map || plane_index < 0 || plane_index > 2)
        return 0;
    for (int row = 0; row < map->rows[plane_index]; row++) {
        for (int column = 0; column < map->columns[plane_index]; column++) {
            size_t index = map->plane_offsets[plane_index] +
                (size_t) row * (size_t) map->columns[plane_index] +
                (size_t) column;
            uint16_t activity = transform_activity(
                plane, column * N148_TRANSFORM_REGION_SIZE,
                row * N148_TRANSFORM_REGION_SIZE);
            int complete = column * 2 + 1 < map->blocks_x[plane_index] &&
                row * 2 + 1 < map->blocks_y[plane_index];
            map->strategies[index] = plane_index == 0 && complete &&
                activity == 0 ? N148_TRANSFORM_DCT16 :
                activity >= 31u * 256u ? N148_TRANSFORM_DCT4 :
                                         N148_TRANSFORM_DCT8;
        }
    }
    return 1;
}

int n148_transform_map_build(const Plane *y, const Plane *cb, const Plane *cr,
                             N148TransformMap *map) {
    if (!y || !cb || !cr || !map || !y->data || !cb->data || !cr->data ||
        cb->width != cr->width || cb->height != cr->height) return 0;
    int chroma = CHROMA_444;
    if (cb->width < y->width) chroma = cb->height < y->height ?
        CHROMA_420 : CHROMA_422;
    if (!n148_transform_map_allocate(y->width, y->height, chroma, map) ||
        map->blocks_x[1] != cb->width / 8 + (cb->width % 8 != 0) ||
        map->blocks_y[1] != cb->height / 8 + (cb->height % 8 != 0) ||
        !build_plane_map(y, 0, map) || !build_plane_map(cb, 1, map) ||
        !build_plane_map(cr, 2, map)) {
        n148_transform_map_release(map);
        return 0;
    }
    return n148_transform_map_validate(map);
}

static int plane_for_index(const N148TransformMap *map, size_t index) {
    if (!map || index >= map->count) return -1;
    if (index >= map->plane_offsets[2]) return 2;
    if (index >= map->plane_offsets[1]) return 1;
    return 0;
}

int n148_transform_map_validate(const N148TransformMap *map) {
    if (!map || !map->strategies || map->count == 0 ||
        map->plane_offsets[0] != 0) return 0;
    size_t expected_offset = 0;
    for (int plane = 0; plane < 3; plane++) {
        if (expected_offset > map->count ||
            map->plane_offsets[plane] != expected_offset ||
            map->columns[plane] <= 0 || map->rows[plane] <= 0 ||
            map->blocks_x[plane] <= 0 || map->blocks_y[plane] <= 0 ||
            (size_t) map->columns[plane] >
                SIZE_MAX / (size_t) map->rows[plane]) return 0;
        size_t plane_count = (size_t) map->columns[plane] *
            (size_t) map->rows[plane];
        if (plane_count > map->count - expected_offset) return 0;
        for (size_t local = 0; local < plane_count; local++) {
            uint8_t strategy = map->strategies[expected_offset + local];
            int column = (int)(local % (size_t) map->columns[plane]);
            int row = (int)(local / (size_t) map->columns[plane]);
            if (strategy >= N148_TRANSFORM_STRATEGY_COUNT ||
                (strategy == N148_TRANSFORM_DCT16 &&
                 (column * 2 + 1 >= map->blocks_x[plane] ||
                  row * 2 + 1 >= map->blocks_y[plane]))) return 0;
        }
        expected_offset += plane_count;
    }
    return expected_offset == map->count;
}

int n148_luma_transform_dct32_origin(const N148TransformMap *map,
                                     int macro_x, int macro_y) {
    if (!map || !map->strategies || macro_x < 0 || macro_y < 0 ||
        (macro_x & 1) || (macro_y & 1) ||
        macro_x + 1 >= map->columns[0] || macro_y + 1 >= map->rows[0] ||
        macro_x * 2 + 3 >= map->blocks_x[0] ||
        macro_y * 2 + 3 >= map->blocks_y[0]) return 0;
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            size_t index = (size_t)(macro_y + dy) *
                (size_t) map->columns[0] + (size_t)(macro_x + dx);
            if (index >= map->plane_offsets[1] ||
                map->strategies[index] != N148_TRANSFORM_DCT4) return 0;
        }
    }
    return 1;
}

int n148_luma_transform_dct32_member(const N148TransformMap *map,
                                     int macro_x, int macro_y) {
    if (macro_x < 0 || macro_y < 0) return 0;
    return n148_luma_transform_dct32_origin(
        map, macro_x & ~1, macro_y & ~1);
}

int n148_luma_transform_map_validate_extended(const N148TransformMap *map,
                                               int allow_dct32) {
    if (!n148_transform_map_validate(map) || map->plane_offsets[1] == 0 ||
        map->plane_offsets[1] > map->count ||
        (allow_dct32 != 0 && allow_dct32 != 1)) return 0;
    for (int row = 0; row < map->rows[0]; row++) {
        for (int column = 0; column < map->columns[0]; column++) {
            size_t index = (size_t) row * (size_t) map->columns[0] +
                (size_t) column;
            uint8_t strategy = map->strategies[index];
            if (strategy == N148_TRANSFORM_DCT8 ||
                strategy == N148_TRANSFORM_DCT16) continue;
            if (!allow_dct32 || strategy != N148_TRANSFORM_DCT4 ||
                !n148_luma_transform_dct32_member(map, column, row))
                return 0;
        }
    }
    for (size_t index = map->plane_offsets[1]; index < map->count; index++)
        if (map->strategies[index] != N148_TRANSFORM_DCT8) return 0;
    return 1;
}

int n148_luma_transform_map_validate(const N148TransformMap *map) {
    return n148_luma_transform_map_validate_extended(map, 0);
}

uint8_t n148_transform_map_predict(const N148TransformMap *map,
                                   size_t index) {
    int plane = plane_for_index(map, index);
    if (plane < 0 || map->columns[plane] <= 0)
        return N148_TRANSFORM_INITIAL_STRATEGY;
    size_t local = index - map->plane_offsets[plane];
    size_t column = local % (size_t) map->columns[plane];
    if (column > 0) return map->strategies[index - 1];
    if (local >= (size_t) map->columns[plane])
        return map->strategies[index - (size_t) map->columns[plane]];
    return N148_TRANSFORM_INITIAL_STRATEGY;
}

int n148_transform_strategy_at(const N148TransformMap *map, int plane,
                               int macro_x, int macro_y, uint8_t *strategy) {
    if (!map || !map->strategies || !strategy || plane < 0 || plane > 2 ||
        macro_x < 0 || macro_y < 0 || macro_x >= map->columns[plane] ||
        macro_y >= map->rows[plane]) return 0;
    size_t index = map->plane_offsets[plane] +
        (size_t) macro_y * (size_t) map->columns[plane] + (size_t) macro_x;
    if (index >= map->count ||
        map->strategies[index] >= N148_TRANSFORM_STRATEGY_COUNT) return 0;
    *strategy = map->strategies[index];
    return 1;
}

void n148_transform_map_release(N148TransformMap *map) {
    if (!map) return;
    free(map->strategies);
    memset(map, 0, sizeof(*map));
}

static const int16_t *matrix_for_size(int size) {
    if (size == 4) return &DCT4_Q14[0][0];
    if (size == 16) return &DCT16_Q14[0][0];
    if (size == 32) return &DCT32_Q14[0][0];
    return NULL;
}

static int64_t round_q28(int64_t value) {
    if (value >= 0)
        return (value + N148_DCT_SCALE_SQUARED / 2) /
            N148_DCT_SCALE_SQUARED;
    return -((-value + N148_DCT_SCALE_SQUARED / 2) /
             N148_DCT_SCALE_SQUARED);
}

static inline void transform4_forward_1d(const int64_t input[4],
                                          int64_t output[4]) {
    int64_t outer_sum = input[0] + input[3];
    int64_t inner_sum = input[1] + input[2];
    int64_t outer_diff = input[0] - input[3];
    int64_t inner_diff = input[1] - input[2];
    output[0] = 8192 * (outer_sum + inner_sum);
    output[1] = 10703 * outer_diff + 4433 * inner_diff;
    output[2] = 8192 * (outer_sum - inner_sum);
    output[3] = 4433 * outer_diff - 10703 * inner_diff;
}

static inline void transform4_inverse_1d(const int64_t input[4],
                                          int64_t output[4]) {
    int64_t even_sum = 8192 * (input[0] + input[2]);
    int64_t even_diff = 8192 * (input[0] - input[2]);
    int64_t odd_0 = 10703 * input[1] + 4433 * input[3];
    int64_t odd_1 = 4433 * input[1] - 10703 * input[3];
    output[0] = even_sum + odd_0;
    output[1] = even_diff + odd_1;
    output[2] = even_diff - odd_1;
    output[3] = even_sum - odd_0;
}

static int transform4_forward(const int32_t *input, int32_t *output) {
    int64_t intermediate[16];
    int32_t result[16];
    for (int column = 0; column < 4; column++) {
        int64_t values[4], transformed[4];
        for (int row = 0; row < 4; row++)
            values[row] = input[row * 4 + column];
        transform4_forward_1d(values, transformed);
        for (int row = 0; row < 4; row++)
            intermediate[row * 4 + column] = transformed[row];
    }
    for (int row = 0; row < 4; row++) {
        int64_t transformed[4];
        transform4_forward_1d(intermediate + row * 4, transformed);
        for (int column = 0; column < 4; column++) {
            int64_t value = round_q28(transformed[column]);
            if (value < INT32_MIN || value > INT32_MAX) return 0;
            result[row * 4 + column] = (int32_t) value;
        }
    }
    memcpy(output, result, sizeof(result));
    return 1;
}

static int transform4_inverse(const int32_t *input, int32_t *output) {
    int64_t intermediate[16];
    int32_t result[16];
    for (int column = 0; column < 4; column++) {
        int64_t values[4], transformed[4];
        for (int row = 0; row < 4; row++)
            values[row] = input[row * 4 + column];
        transform4_inverse_1d(values, transformed);
        for (int row = 0; row < 4; row++)
            intermediate[row * 4 + column] = transformed[row];
    }
    for (int row = 0; row < 4; row++) {
        int64_t transformed[4];
        transform4_inverse_1d(intermediate + row * 4, transformed);
        for (int column = 0; column < 4; column++) {
            int64_t value = round_q28(transformed[column]);
            if (value < INT32_MIN || value > INT32_MAX) return 0;
            result[row * 4 + column] = (int32_t) value;
        }
    }
    memcpy(output, result, sizeof(result));
    return 1;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static inline void store_interleaved64_avx2(int64_t *destination,
                                            __m256i even, __m256i odd) {
    __m256i low = _mm256_unpacklo_epi64(even, odd);
    __m256i high = _mm256_unpackhi_epi64(even, odd);
    _mm256_storeu_si256((__m256i *) destination,
                        _mm256_permute2x128_si256(low, high, 0x20));
    _mm256_storeu_si256((__m256i *)(destination + 4),
                        _mm256_permute2x128_si256(low, high, 0x31));
}

__attribute__((target("avx2")))
static inline __m256i round_q28_avx2(__m256i values) {
    __m256i zero = _mm256_setzero_si256();
    __m256i negative = _mm256_cmpgt_epi64(zero, values);
    __m256i absolute = _mm256_sub_epi64(
        _mm256_xor_si256(values, negative), negative);
    __m256i rounded = _mm256_srli_epi64(_mm256_add_epi64(
        absolute, _mm256_set1_epi64x(N148_DCT_SCALE_SQUARED / 2)), 28);
    return _mm256_sub_epi64(_mm256_xor_si256(rounded, negative), negative);
}

__attribute__((target("avx2")))
static inline void transpose4x4_i64(__m256i v[4]) {
    __m256i a = _mm256_unpacklo_epi64(v[0], v[1]);
    __m256i b = _mm256_unpackhi_epi64(v[0], v[1]);
    __m256i c = _mm256_unpacklo_epi64(v[2], v[3]);
    __m256i d = _mm256_unpackhi_epi64(v[2], v[3]);
    v[0] = _mm256_permute2x128_si256(a, c, 0x20);
    v[1] = _mm256_permute2x128_si256(b, d, 0x20);
    v[2] = _mm256_permute2x128_si256(a, c, 0x31);
    v[3] = _mm256_permute2x128_si256(b, d, 0x31);
}

__attribute__((target("avx2")))
static inline void transform4_pass_avx2(__m256i v[4], int inverse) {
    const __m256i c1 = _mm256_set1_epi64x(10703);
    const __m256i c2 = _mm256_set1_epi64x(4433);
    __m256i a, b, c, d;
    if (inverse) {
        a = _mm256_slli_epi64(_mm256_add_epi64(v[0], v[2]), 13);
        b = _mm256_slli_epi64(_mm256_sub_epi64(v[0], v[2]), 13);
        c = _mm256_add_epi64(_mm256_mul_epi32(v[1], c1),
                             _mm256_mul_epi32(v[3], c2));
        d = _mm256_sub_epi64(_mm256_mul_epi32(v[1], c2),
                             _mm256_mul_epi32(v[3], c1));
        v[0] = _mm256_add_epi64(a, c);
        v[1] = _mm256_add_epi64(b, d);
        v[2] = _mm256_sub_epi64(b, d);
        v[3] = _mm256_sub_epi64(a, c);
    } else {
        a = _mm256_add_epi64(v[0], v[3]);
        b = _mm256_add_epi64(v[1], v[2]);
        c = _mm256_sub_epi64(v[0], v[3]);
        d = _mm256_sub_epi64(v[1], v[2]);
        v[0] = _mm256_slli_epi64(_mm256_add_epi64(a, b), 13);
        v[1] = _mm256_add_epi64(_mm256_mul_epi32(c, c1),
                             _mm256_mul_epi32(d, c2));
        v[2] = _mm256_slli_epi64(_mm256_sub_epi64(a, b), 13);
        v[3] = _mm256_sub_epi64(_mm256_mul_epi32(c, c2),
                             _mm256_mul_epi32(d, c1));
    }
}

/* The image-domain 4x4 transform uses the same Q14 matrix, pass order and
   final Q28 rounding as the general path. With |input| <= 8191 every
   multiplicand in both passes fits int32; products and sums stay int64.
   Larger internal inputs retain the general checked implementation. */
__attribute__((target("avx2")))
static int transform4_avx2(const int32_t *input, int32_t *output, int inverse) {
    __m128i rows[4];
    __m256i v[4];
    for (int row = 0; row < 4; row++)
        rows[row] = _mm_loadu_si128((const __m128i *)(input + 4 * row));
    __m128i packed0 = _mm_packs_epi32(rows[0], rows[1]);
    __m128i packed1 = _mm_packs_epi32(rows[2], rows[3]);
    __m128i low = _mm_set1_epi16(-8191), high = _mm_set1_epi16(8191);
    __m128i outside = _mm_or_si128(
        _mm_or_si128(_mm_cmpgt_epi16(low, packed0), _mm_cmpgt_epi16(packed0, high)),
        _mm_or_si128(_mm_cmpgt_epi16(low, packed1), _mm_cmpgt_epi16(packed1, high)));
    if (_mm_movemask_epi8(outside)) return 0;
    for (int row = 0; row < 4; row++) v[row] = _mm256_cvtepi32_epi64(rows[row]);
    transform4_pass_avx2(v, inverse);
    transpose4x4_i64(v);
    transform4_pass_avx2(v, inverse);
    transpose4x4_i64(v);
    const __m256i compact = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
    for (int row = 0; row < 4; row++) {
        __m256i rounded = round_q28_avx2(v[row]);
        __m256i packed = _mm256_permutevar8x32_epi32(rounded, compact);
        _mm_storeu_si128((__m128i *)(output + 4 * row), _mm256_castsi256_si128(packed));
    }
    return 1;
}

__attribute__((target("avx2")))
static inline int store_rounded8_avx2(int32_t *destination,
                                       __m256i even, __m256i odd) {
    even = round_q28_avx2(even);
    odd = round_q28_avx2(odd);
    __m256i maximum = _mm256_set1_epi64x(INT32_MAX);
    __m256i minimum = _mm256_set1_epi64x(INT32_MIN);
    __m256i overflow = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpgt_epi64(even, maximum),
                        _mm256_cmpgt_epi64(minimum, even)),
        _mm256_or_si256(_mm256_cmpgt_epi64(odd, maximum),
                        _mm256_cmpgt_epi64(minimum, odd)));
    if (!_mm256_testz_si256(overflow, overflow)) return 0;
    __m256i low_dwords = _mm256_set1_epi64x(0x0b0a090803020100ll);
    __m256i compact_even = _mm256_shuffle_epi8(even, low_dwords);
    __m256i compact_odd = _mm256_shuffle_epi8(odd, low_dwords);
    _mm256_storeu_si256((__m256i *) destination,
                        _mm256_unpacklo_epi32(compact_even, compact_odd));
    return 1;
}

__attribute__((target("avx2")))
static void transform_first_pass_avx2(const int32_t *input, int size,
                                      int inverse, const int16_t *matrix,
                                      int64_t *intermediate) {
    for (int outer = 0; outer < size; outer++) {
        for (int x = 0; x < size; x += 8) {
            __m256i even = _mm256_setzero_si256();
            __m256i odd = _mm256_setzero_si256();
            for (int inner = 0; inner < size; inner++) {
                int32_t coefficient = inverse ?
                    matrix[inner * size + outer] :
                    matrix[outer * size + inner];
                __m256i values = _mm256_loadu_si256(
                    (const __m256i *)(input + inner * size + x));
                __m256i factor = _mm256_set1_epi32(coefficient);
                even = _mm256_add_epi64(
                    even, _mm256_mul_epi32(values, factor));
                odd = _mm256_add_epi64(
                    odd, _mm256_mul_epi32(
                        _mm256_srli_epi64(values, 32), factor));
            }
            store_interleaved64_avx2(
                intermediate + outer * size + x, even, odd);
        }
    }
}

__attribute__((target("avx2")))
static int inverse_second_pass_avx2(const int64_t *intermediate, int size,
                                    const int16_t *matrix, int32_t *result) {
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x += 8) {
            __m256i even = _mm256_setzero_si256();
            __m256i odd = _mm256_setzero_si256();
            for (int frequency_x = 0; frequency_x < size; frequency_x++) {
                __m128i packed = _mm_loadu_si128((const __m128i *)(
                    matrix + frequency_x * size + x));
                __m256i coefficients = _mm256_cvtepi16_epi32(packed);
                __m256i value = _mm256_set1_epi32((int32_t)
                    intermediate[y * size + frequency_x]);
                even = _mm256_add_epi64(
                    even, _mm256_mul_epi32(coefficients, value));
                odd = _mm256_add_epi64(
                    odd, _mm256_mul_epi32(
                        _mm256_srli_epi64(coefficients, 32), value));
            }
            if (!store_rounded8_avx2(result + y * size + x, even, odd))
                return 0;
        }
    }
    return 1;
}

__attribute__((target("avx2")))
static void inverse_sparse16_first_pass_avx2(
    const int32_t input[16 * 16], uint32_t active_rows,
    uint32_t active_columns, int64_t intermediate[16 * 16]) {
    int active_frequencies[16];
    int active_count = 0;
    for (int frequency_y = 0; frequency_y < 16; frequency_y++) {
        if (active_rows & (1u << frequency_y))
            active_frequencies[active_count++] = frequency_y;
    }
    if (active_count <= 2) {
        int first_frequency = active_frequencies[0];
        int second_frequency = active_count == 2 ? active_frequencies[1] : 0;
        for (int y = 0; y < 16; y++) {
            __m256i first_factor = _mm256_set1_epi32(
                DCT16_Q14[first_frequency][y]);
            __m256i second_factor = _mm256_setzero_si256();
            if (active_count == 2)
                second_factor = _mm256_set1_epi32(
                    DCT16_Q14[second_frequency][y]);
            for (int x = 0; x < 16; x += 8) {
                if (!(active_columns & (0xffu << x))) continue;
                __m256i values = _mm256_loadu_si256(
                    (const __m256i *)(input + first_frequency * 16 + x));
                __m256i even = _mm256_mul_epi32(values, first_factor);
                __m256i odd = _mm256_mul_epi32(
                    _mm256_srli_epi64(values, 32), first_factor);
                if (active_count == 2) {
                    values = _mm256_loadu_si256((const __m256i *)(
                        input + second_frequency * 16 + x));
                    even = _mm256_add_epi64(
                        even, _mm256_mul_epi32(values, second_factor));
                    odd = _mm256_add_epi64(
                        odd, _mm256_mul_epi32(
                            _mm256_srli_epi64(values, 32), second_factor));
                }
                store_interleaved64_avx2(
                    intermediate + y * 16 + x, even, odd);
            }
        }
        return;
    }
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x += 8) {
            if (!(active_columns & (0xffu << x))) continue;
            __m256i even = _mm256_setzero_si256();
            __m256i odd = _mm256_setzero_si256();
            for (int frequency = 0; frequency < active_count; frequency++) {
                int frequency_y = active_frequencies[frequency];
                __m256i values = _mm256_loadu_si256(
                    (const __m256i *)(input + frequency_y * 16 + x));
                __m256i factor = _mm256_set1_epi32(
                    DCT16_Q14[frequency_y][y]);
                even = _mm256_add_epi64(
                    even, _mm256_mul_epi32(values, factor));
                odd = _mm256_add_epi64(
                    odd, _mm256_mul_epi32(
                        _mm256_srli_epi64(values, 32), factor));
            }
            store_interleaved64_avx2(
                intermediate + y * 16 + x, even, odd);
        }
    }
}

__attribute__((target("avx2")))
static int inverse_sparse16_second_pass_avx2(
    const int64_t intermediate[16 * 16], uint32_t active_columns,
    int32_t result[16 * 16]) {
    int active_frequencies[16];
    int active_count = 0;
    for (int frequency_x = 0; frequency_x < 16; frequency_x++) {
        if (active_columns & (1u << frequency_x))
            active_frequencies[active_count++] = frequency_x;
    }
    if (active_count <= 2) {
        int first_frequency = active_frequencies[0];
        int second_frequency = active_count == 2 ? active_frequencies[1] : 0;
        for (int y = 0; y < 16; y++) {
            __m256i first_value = _mm256_set1_epi32((int32_t)
                intermediate[y * 16 + first_frequency]);
            __m256i second_value = _mm256_setzero_si256();
            if (active_count == 2)
                second_value = _mm256_set1_epi32((int32_t)
                    intermediate[y * 16 + second_frequency]);
            for (int x = 0; x < 16; x += 8) {
                __m128i packed = _mm_loadu_si128((const __m128i *)(
                    DCT16_Q14[first_frequency] + x));
                __m256i coefficients = _mm256_cvtepi16_epi32(packed);
                __m256i even = _mm256_mul_epi32(coefficients, first_value);
                __m256i odd = _mm256_mul_epi32(
                    _mm256_srli_epi64(coefficients, 32), first_value);
                if (active_count == 2) {
                    packed = _mm_loadu_si128((const __m128i *)(
                        DCT16_Q14[second_frequency] + x));
                    coefficients = _mm256_cvtepi16_epi32(packed);
                    even = _mm256_add_epi64(
                        even, _mm256_mul_epi32(coefficients, second_value));
                    odd = _mm256_add_epi64(
                        odd, _mm256_mul_epi32(
                            _mm256_srli_epi64(coefficients, 32),
                            second_value));
                }
                if (!store_rounded8_avx2(result + y * 16 + x, even, odd))
                    return 0;
            }
        }
        return 1;
    }
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x += 8) {
            __m256i even = _mm256_setzero_si256();
            __m256i odd = _mm256_setzero_si256();
            for (int frequency = 0; frequency < active_count; frequency++) {
                int frequency_x = active_frequencies[frequency];
                __m128i packed = _mm_loadu_si128((const __m128i *)(
                    DCT16_Q14[frequency_x] + x));
                __m256i coefficients = _mm256_cvtepi16_epi32(packed);
                __m256i value = _mm256_set1_epi32((int32_t)
                    intermediate[y * 16 + frequency_x]);
                even = _mm256_add_epi64(
                    even, _mm256_mul_epi32(coefficients, value));
                odd = _mm256_add_epi64(
                    odd, _mm256_mul_epi32(
                        _mm256_srli_epi64(coefficients, 32), value));
            }
            if (!store_rounded8_avx2(result + y * 16 + x, even, odd))
                return 0;
        }
    }
    return 1;
}

#endif

int n148_variable_inverse_sparse16(const int32_t input[16 * 16],
                                   uint32_t active_rows,
                                   uint32_t active_columns,
                                   int32_t output[16 * 16]) {
    if (!input || !output ||
        ((active_rows | active_columns) & ~0xffffu) ||
        (!!active_rows != !!active_columns)) return 0;
    if (!active_rows) {
        memset(output, 0, 16 * 16 * sizeof(*output));
        return 1;
    }
    int64_t intermediate[16 * 16] = {0};
    int32_t result[16 * 16];
#if defined(__x86_64__) || defined(__i386__)
    int use_avx2 = n148_cpu_level() >= N148_CPU_AVX2;
    if (use_avx2)
        inverse_sparse16_first_pass_avx2(
            input, active_rows, active_columns, intermediate);
#else
    int use_avx2 = 0;
#endif
    if (!use_avx2) {
        for (int y = 0; y < 16; y++) {
            for (int frequency_x = 0; frequency_x < 16; frequency_x++) {
                if (!(active_columns & (1u << frequency_x))) continue;
                int64_t sum = 0;
                for (int frequency_y = 0; frequency_y < 16; frequency_y++) {
                    if (!(active_rows & (1u << frequency_y))) continue;
                    sum += (int64_t) DCT16_Q14[frequency_y][y] *
                        input[frequency_y * 16 + frequency_x];
                }
                intermediate[y * 16 + frequency_x] = sum;
            }
        }
    }
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2) {
        int fits32 = 1;
        uint32_t columns = active_columns;
        while (columns && fits32) {
            int x = __builtin_ctz(columns);
            columns &= columns - 1;
            for (int y = 0; y < 16; y++) {
                int64_t value = intermediate[y * 16 + x];
                if (value < INT32_MIN || value > INT32_MAX) {
                    fits32 = 0;
                    break;
                }
            }
        }
        if (fits32) {
            if (!inverse_sparse16_second_pass_avx2(
                    intermediate, active_columns, result)) return 0;
            memcpy(output, result, sizeof(result));
            return 1;
        }
    }
#endif
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int64_t sum = 0;
            for (int frequency_x = 0; frequency_x < 16; frequency_x++) {
                if (!(active_columns & (1u << frequency_x))) continue;
                sum += intermediate[y * 16 + frequency_x] *
                    DCT16_Q14[frequency_x][x];
            }
            int64_t value = round_q28(sum);
            if (value < INT32_MIN || value > INT32_MAX) return 0;
            result[y * 16 + x] = (int32_t) value;
        }
    }
    memcpy(output, result, sizeof(result));
    return 1;
}

static int transform_2d(const int32_t *input, int size, int inverse,
                        int32_t *output) {
    const int16_t *matrix = matrix_for_size(size);
    if (!input || !output || !matrix) return 0;
    if (size == 4) {
#if defined(__x86_64__) || defined(__i386__)
        if (n148_cpu_level() >= N148_CPU_AVX2 &&
            transform4_avx2(input, output, inverse)) return 1;
#endif
        return inverse ? transform4_inverse(input, output) :
                         transform4_forward(input, output);
    }
    int64_t intermediate[32 * 32];
    int32_t result[32 * 32];
#if defined(__x86_64__) || defined(__i386__)
    int use_avx2 = n148_cpu_level() >= N148_CPU_AVX2;
    if (use_avx2)
        transform_first_pass_avx2(input, size, inverse, matrix, intermediate);
#else
    int use_avx2 = 0;
#endif
    if (!inverse) {
        if (!use_avx2) {
            for (int frequency_y = 0; frequency_y < size; frequency_y++) {
                for (int x = 0; x < size; x++) {
                    int64_t sum = 0;
                    for (int y = 0; y < size; y++)
                        sum += (int64_t) matrix[frequency_y * size + y] *
                            input[y * size + x];
                    intermediate[frequency_y * size + x] = sum;
                }
            }
        }
#if defined(__x86_64__) || defined(__i386__)
        if (use_avx2) {
            int fits32 = 1;
            for (int index = 0; index < size * size; index++) {
                if (intermediate[index] < INT32_MIN ||
                    intermediate[index] > INT32_MAX) {
                    fits32 = 0;
                    break;
                }
            }
            if (fits32) {
                const int16_t *transposed = size == 16 ?
                    &DCT16_TRANSPOSE_Q14[0][0] : &DCT32_TRANSPOSE_Q14[0][0];
                if (!inverse_second_pass_avx2(intermediate, size,
                                              transposed, result)) return 0;
                memcpy(output, result,
                       (size_t) size * (size_t) size * sizeof(*output));
                return 1;
            }
        }
#endif
        for (int frequency_y = 0; frequency_y < size; frequency_y++) {
            for (int frequency_x = 0; frequency_x < size; frequency_x++) {
                int64_t sum = 0;
                for (int x = 0; x < size; x++)
                    sum += intermediate[frequency_y * size + x] *
                        matrix[frequency_x * size + x];
                int64_t value = round_q28(sum);
                if (value < INT32_MIN || value > INT32_MAX) return 0;
                result[frequency_y * size + frequency_x] = (int32_t) value;
            }
        }
    } else {
        if (!use_avx2) {
            for (int y = 0; y < size; y++) {
                for (int frequency_x = 0; frequency_x < size; frequency_x++) {
                    int64_t sum = 0;
                    for (int frequency_y = 0; frequency_y < size; frequency_y++)
                        sum += (int64_t) matrix[frequency_y * size + y] *
                            input[frequency_y * size + frequency_x];
                    intermediate[y * size + frequency_x] = sum;
                }
            }
        }
#if defined(__x86_64__) || defined(__i386__)
        if (use_avx2) {
            int fits32 = 1;
            for (int index = 0; index < size * size; index++) {
                if (intermediate[index] < INT32_MIN ||
                    intermediate[index] > INT32_MAX) {
                    fits32 = 0;
                    break;
                }
            }
            if (fits32) {
                if (!inverse_second_pass_avx2(intermediate, size,
                                              matrix, result)) return 0;
                memcpy(output, result,
                       (size_t) size * (size_t) size * sizeof(*output));
                return 1;
            }
        }
#endif
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                int64_t sum = 0;
                for (int frequency_x = 0; frequency_x < size; frequency_x++)
                    sum += intermediate[y * size + frequency_x] *
                        matrix[frequency_x * size + x];
                int64_t value = round_q28(sum);
                if (value < INT32_MIN || value > INT32_MAX) return 0;
                result[y * size + x] = (int32_t) value;
            }
        }
    }
    memcpy(output, result, (size_t) size * (size_t) size * sizeof(*output));
    return 1;
}

int n148_variable_forward(const int32_t *input, int size, int32_t *output) {
    return transform_2d(input, size, 0, output);
}

int n148_variable_inverse(const int32_t *input, int size, int32_t *output) {
    return transform_2d(input, size, 1, output);
}

#include "variable_zigzag_tables.inc"

uint64_t n148_variable_transform_matrix_hash(void) {
    uint64_t hash = 14695981039346656037ull;
    const int16_t *tables[2] = {&DCT4_Q14[0][0], &DCT16_Q14[0][0]};
    const size_t counts[2] = {16, 256};
    for (int table = 0; table < 2; table++) {
        for (size_t index = 0; index < counts[table]; index++) {
            uint16_t value = (uint16_t) tables[table][index];
            hash ^= (uint8_t) value;
            hash *= 1099511628211ull;
            hash ^= (uint8_t)(value >> 8);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

uint64_t n148_variable_transform_matrix32_hash(void) {
    uint64_t hash = 14695981039346656037ull;
    const int16_t *values = &DCT32_Q14[0][0];
    for (size_t index = 0; index < 32u * 32u; index++) {
        uint16_t value = (uint16_t) values[index];
        hash ^= (uint8_t) value;
        hash *= 1099511628211ull;
        hash ^= (uint8_t)(value >> 8);
        hash *= 1099511628211ull;
    }
    return hash;
}

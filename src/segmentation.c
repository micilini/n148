/*
 * N.148i frame-level segmentation
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * The encoder classifies the complete frame before coding any block. This is
 * intentionally different from the rejected local-threshold experiment:
 * four ordered centres adapt to each image's own activity distribution. Only
 * the map geometry and quantizer scales are normative to the decoder.
 */

#include "segmentation.h"
#include "cpu.h"
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int map_dimensions(int width, int height, int *columns, int *rows,
                          size_t *count) {
    if (width <= 0 || height <= 0 || !columns || !rows || !count) return 0;
    int map_columns = width / N148_SEGMENT_REGION_SIZE +
        (width % N148_SEGMENT_REGION_SIZE != 0);
    int map_rows = height / N148_SEGMENT_REGION_SIZE +
        (height % N148_SEGMENT_REGION_SIZE != 0);
    if ((size_t) map_columns > SIZE_MAX / (size_t) map_rows) return 0;
    size_t map_count = (size_t) map_columns * (size_t) map_rows;
    if (map_count == 0 || map_count > UINT32_MAX) return 0;
    *columns = map_columns;
    *rows = map_rows;
    *count = map_count;
    return 1;
}

int n148_segmentation_map_allocate(int width, int height,
                                   N148SegmentationMap *map) {
    if (!map) return 0;
    memset(map, 0, sizeof(*map));
    if (!map_dimensions(width, height, &map->columns, &map->rows,
                        &map->count)) return 0;
    map->levels = (uint8_t *) calloc(map->count, 1);
    if (!map->levels) {
        memset(map, 0, sizeof(*map));
        return 0;
    }
    for (int segment = 0; segment < N148_SEGMENT_COUNT; segment++)
        map->quant_scale[segment] = 100;
    return 1;
}

int n148_segmentation_map_validate(const N148SegmentationMap *map) {
    if (!map || !map->levels || map->count == 0 || map->columns <= 0 ||
        map->rows <= 0 || (size_t) map->columns >
            SIZE_MAX / (size_t) map->rows ||
        map->count != (size_t) map->columns * (size_t) map->rows ||
        map->adaptive_filter > 1) return 0;
    for (size_t index = 0; index < map->count; index++)
        if (map->levels[index] >= N148_SEGMENT_COUNT) return 0;
    for (int segment = 0; segment < N148_SEGMENT_COUNT; segment++)
        if (map->quant_scale[segment] < N148_SEGMENT_MIN_SCALE ||
            map->quant_scale[segment] > N148_SEGMENT_MAX_SCALE) return 0;
    return 1;
}

static int compare_u32(const void *first, const void *second) {
    uint32_t a = *(const uint32_t *) first;
    uint32_t b = *(const uint32_t *) second;
    return (a > b) - (a < b);
}

static int sample_or_neutral(const Plane *plane, int x, int y) {
    if (x < 0 || y < 0 || x >= plane->width || y >= plane->height) return 128;
    return plane->data[(long) y * plane->width + x];
}

static int clamp_sample(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

/* Approximate the residual susceptibility that the real encoder will see.
   Each 8x8 cell prices DC, vertical, horizontal and TrueMotion prediction
   from original neighbours, then contributes only its best absolute error.
   This separates predictable edges and ramps from genuinely irregular
   texture much better than a raw variance or gradient threshold. */
static uint32_t region_activity_scalar(const Plane *plane, int x0, int y0) {
    int x1 = x0 + N148_SEGMENT_REGION_SIZE;
    int y1 = y0 + N148_SEGMENT_REGION_SIZE;
    if (x1 > plane->width) x1 = plane->width;
    if (y1 > plane->height) y1 = plane->height;
    uint64_t activity = 0;
    uint32_t samples = 0;
    for (int block_y = y0; block_y < y1; block_y += 8) {
        for (int block_x = x0; block_x < x1; block_x += 8) {
            int block_x1 = block_x + 8;
            int block_y1 = block_y + 8;
            if (block_x1 > x1) block_x1 = x1;
            if (block_y1 > y1) block_y1 = y1;
            uint64_t errors[4] = {0, 0, 0, 0};
            uint32_t block_samples = 0;
            int dc_sum = 0;
            int dc_count = 0;
            if (block_y > 0) {
                for (int x = block_x; x < block_x1; x++) {
                    dc_sum += sample_or_neutral(plane, x, block_y - 1);
                    dc_count++;
                }
            }
            if (block_x > 0) {
                for (int y = block_y; y < block_y1; y++) {
                    dc_sum += sample_or_neutral(plane, block_x - 1, y);
                    dc_count++;
                }
            }
            int dc = dc_count ? (dc_sum + dc_count / 2) / dc_count : 128;
            int top_left = sample_or_neutral(
                plane, block_x - 1, block_y - 1);
            for (int y = block_y; y < block_y1; y++) {
                for (int x = block_x; x < block_x1; x++) {
                    int source = sample_or_neutral(plane, x, y);
                    int top = sample_or_neutral(plane, x, block_y - 1);
                    int left = sample_or_neutral(plane, block_x - 1, y);
                    int predictions[4] = {
                        dc, top, left, clamp_sample(top + left - top_left),
                    };
                    for (int mode = 0; mode < 4; mode++) {
                        int error = source - predictions[mode];
                        if (error < 0) error = -error;
                        if (error > 64) error = 64;
                        errors[mode] += (uint32_t) error;
                    }
                    block_samples++;
                }
            }
            uint64_t best = errors[0];
            for (int mode = 1; mode < 4; mode++)
                if (errors[mode] < best) best = errors[mode];
            activity += best;
            samples += block_samples;
        }
    }
    return samples ?
        (uint32_t)((activity * 256u + samples / 2u) / samples) : 0;
}

/* Mean local8x8 source variance in fixed-point units. The structural
   policy uses the SSIM contrast denominator for region allocation. */
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static uint32_t region_activity_vector(const Plane *plane, int x0, int y0) {
    uint32_t activity=0;
    const __m128i limit=_mm_set1_epi16(64);
    for (int by=y0; by<y0+16; by+=8) for (int bx=x0; bx<x0+16; bx+=8) {
        int dc_sum=0, dc_count=0;
        if (by>0) for (int x=bx; x<bx+8; x++) {
            dc_sum+=plane->data[(long)(by-1)*plane->width+x]; dc_count++;
        }
        if (bx>0) for (int y=by; y<by+8; y++) {
            dc_sum+=plane->data[(long)y*plane->width+bx-1]; dc_count++;
        }
        int dc=dc_count ? (dc_sum+dc_count/2)/dc_count : 128;
        int top_left=sample_or_neutral(plane,bx-1,by-1);
        __m128i top8 = by>0 ? _mm_loadl_epi64((const __m128i *)(
            plane->data+(long)(by-1)*plane->width+bx)) : _mm_set1_epi8((char)128);
        __m128i top=_mm_cvtepu8_epi16(top8);
        __m128i totals01=_mm_setzero_si128(), totals23=_mm_setzero_si128();
        for (int y=by; y<by+8; y++) {
            __m128i pixels=_mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)(
                plane->data+(long)y*plane->width+bx)));
            int left_value=sample_or_neutral(plane,bx-1,y);
            __m128i left=_mm_set1_epi16((short)left_value);
            __m128i planar=_mm_add_epi16(top,_mm_set1_epi16((short)(left_value-top_left)));
            planar=_mm_min_epi16(_mm_max_epi16(planar,_mm_setzero_si128()),_mm_set1_epi16(255));
            __m128i e0=_mm_min_epi16(_mm_abs_epi16(_mm_sub_epi16(pixels,_mm_set1_epi16((short)dc))),limit);
            __m128i e1=_mm_min_epi16(_mm_abs_epi16(_mm_sub_epi16(pixels,top)),limit);
            __m128i e2=_mm_min_epi16(_mm_abs_epi16(_mm_sub_epi16(pixels,left)),limit);
            __m128i e3=_mm_min_epi16(_mm_abs_epi16(_mm_sub_epi16(pixels,planar)),limit);
            totals01=_mm_add_epi64(totals01,_mm_sad_epu8(_mm_packus_epi16(e0,e1),_mm_setzero_si128()));
            totals23=_mm_add_epi64(totals23,_mm_sad_epu8(_mm_packus_epi16(e2,e3),_mm_setzero_si128()));
        }
        uint64_t errors[4];
        _mm_storeu_si128((__m128i *)errors,totals01);
        _mm_storeu_si128((__m128i *)(errors+2),totals23);
        uint64_t best=errors[0];
        for (int k=1; k<4; k++) if (errors[k]<best) best=errors[k];
        activity+=(uint32_t)best;
    }
    /* Exactly 256 source samples: (sum*256 + 128)/256 equals sum. */
    return activity;
}
#endif

static uint32_t region_activity(const Plane *plane, int x0, int y0) {
#if defined(__x86_64__) || defined(__i386__)
    if (x0>=0 && y0>=0 && plane->width-x0>=16 && plane->height-y0>=16 &&
        n148_cpu_level()>=N148_CPU_AVX2) return region_activity_vector(plane,x0,y0);
#endif
    return region_activity_scalar(plane,x0,y0);
}

static uint32_t region_source_variance(const Plane *plane, int x0, int y0,
                                       int predictable) {
    uint64_t total = 0;
    unsigned int count = 0;
    for (int by = y0; by < y0 + 16 && by < plane->height; by += 8) {
        for (int bx = x0; bx < x0 + 16 && bx < plane->width; bx += 8) {
            uint64_t sum = 0, square = 0;
            unsigned int n = 0;
            for (int y = by; y < by + 8 && y < plane->height; y++) {
                for (int x = bx; x < bx + 8 && x < plane->width; x++) {
                    unsigned int v = plane->data[(long)y * plane->width + x];
                    sum += v; square += v * v; n++;
                }
            }
            total += 256 * (n * square - sum * sum) / n;
            count += n;
        }
    }
    if (!predictable) return count ? (uint32_t)(total / count) : 0;
    uint64_t residual = region_activity(plane, x0, y0);
    uint32_t activity = (uint32_t)((2 * residual * residual + 128) / 256);
    uint32_t variance = count ? (uint32_t)(total / count) : 0;
    return activity < variance ? activity : variance;
}

static int nearest_centre(uint32_t score,
                          const uint32_t centres[N148_SEGMENT_COUNT]) {
    int best = 0;
    uint32_t best_distance = score > centres[0] ?
        score - centres[0] : centres[0] - score;
    for (int level = 1; level < N148_SEGMENT_COUNT; level++) {
        uint32_t distance = score > centres[level] ?
            score - centres[level] : centres[level] - score;
        if (distance < best_distance) {
            best = level;
            best_distance = distance;
        }
    }
    return best;
}

static int build_map(const Plane *luma, N148SegmentationMap *map,
                      int source_variance) {
    if (!luma || !luma->data || !map ||
        !n148_segmentation_map_allocate(luma->width, luma->height, map))
        return 0;
    if (map->count > SIZE_MAX / sizeof(uint32_t)) {
        n148_segmentation_map_release(map);
        return 0;
    }
    uint32_t *scores = (uint32_t *) malloc(map->count * sizeof(*scores));
    uint32_t *sorted = (uint32_t *) malloc(map->count * sizeof(*sorted));
    if (!scores || !sorted) {
        free(scores);
        free(sorted);
        n148_segmentation_map_release(map);
        return 0;
    }
    for (int row = 0; row < map->rows; row++) {
        for (int column = 0; column < map->columns; column++) {
            size_t index = (size_t) row * (size_t) map->columns +
                (size_t) column;
            scores[index] = source_variance ? region_source_variance(
                luma, column * N148_SEGMENT_REGION_SIZE,
                row * N148_SEGMENT_REGION_SIZE, source_variance == 2) : region_activity(
                luma, column * N148_SEGMENT_REGION_SIZE,
                row * N148_SEGMENT_REGION_SIZE);
            if (source_variance == 2) scores[index] = (uint32_t)(
                256.0 * log(scores[index] / 256.0 + 29.26125) + 0.5);
            sorted[index] = scores[index];
        }
    }
    qsort(sorted, map->count, sizeof(*sorted), compare_u32);
    uint32_t centres[N148_SEGMENT_COUNT];
    for (int level = 0; level < N148_SEGMENT_COUNT; level++) {
        size_t numerator = (size_t)(2 * level + 1) * map->count;
        size_t position = numerator / (2u * N148_SEGMENT_COUNT);
        if (position >= map->count) position = map->count - 1;
        centres[level] = sorted[position];
    }
    for (int iteration = 0; iteration < 8; iteration++) {
        uint64_t sums[N148_SEGMENT_COUNT] = {0};
        uint32_t counts[N148_SEGMENT_COUNT] = {0};
        for (size_t index = 0; index < map->count; index++) {
            int level = nearest_centre(scores[index], centres);
            sums[level] += scores[index];
            counts[level]++;
        }
        int changed = 0;
        for (int level = 0; level < N148_SEGMENT_COUNT; level++) {
            if (!counts[level]) continue;
            uint32_t centre = (uint32_t)((sums[level] + counts[level] / 2u) /
                                         counts[level]);
            if (centre != centres[level]) changed = 1;
            centres[level] = centre;
        }
        if (!changed) break;
    }
    uint64_t weighted_sum = 0;
    for (size_t index = 0; index < map->count; index++) {
        int level = nearest_centre(scores[index], centres);
        map->levels[index] = (uint8_t) level;
        weighted_sum += centres[level];
    }
    if (source_variance) {
        double log_sum = 0.0;
        for (size_t index = 0; index < map->count; index++)
            log_sum += source_variance == 2 ? (centres[map->levels[index]] / 256.0) :
                log(centres[map->levels[index]] / 256.0 + 29.26125);
        double log_mean = log_sum / map->count;
        for (int level = 0; level < N148_SEGMENT_COUNT; level++) {
            double log_activity = source_variance == 2 ? (centres[level] / 256.0) :
                log(centres[level] / 256.0 + 29.26125);
            /* Eighth-root allocation balances RGB squared error with the
               structural preference for finer steps in smooth regions. */
            int scale = (int)(100.0 * exp(0.125 * (log_activity - log_mean)) + 0.5);
            if (scale < N148_SEGMENT_MIN_SCALE) scale = N148_SEGMENT_MIN_SCALE;
            if (scale > N148_SEGMENT_MAX_SCALE) scale = N148_SEGMENT_MAX_SCALE;
            map->quant_scale[level] = (uint8_t) scale;
        }
    } else {
        uint32_t weighted_mean = (uint32_t)(
            (weighted_sum + map->count / 2u) / map->count);
        uint32_t range = centres[N148_SEGMENT_COUNT - 1] - centres[0];
        for (int level = 0; level < N148_SEGMENT_COUNT; level++) {
            int64_t difference = (int64_t) centres[level] - weighted_mean;
            int delta = 0;
            if (range) {
                int64_t scaled = 32 * difference;
                delta = (int)((scaled >= 0 ? scaled + range / 2u :
                               scaled - range / 2u) / (int64_t) range);
            }
            int scale = 100 + delta;
            if (scale < N148_SEGMENT_MIN_SCALE) scale = N148_SEGMENT_MIN_SCALE;
            if (scale > N148_SEGMENT_MAX_SCALE) scale = N148_SEGMENT_MAX_SCALE;
            /* Preserve smooth surfaces aggressively and cap the texture penalty.
               The frame-derived middle scales still adapt to its distribution. */
            if (level == 0 && scale > 88) scale = 88;
            if (level == 1 && scale > 95) scale = 95;
            if (level == 3 && scale > 118) scale = 118;
            map->quant_scale[level] = (uint8_t) scale;
        }
    }
    free(scores);
    free(sorted);
    return n148_segmentation_map_validate(map);
}

int n148_segmentation_map_build(const Plane *luma, N148SegmentationMap *map) {
    return build_map(luma, map, 0);
}

int n148_segmentation_map_build_structural(const Plane *luma,
                                           N148SegmentationMap *map) {
    return build_map(luma, map, 1);
}

int n148_segmentation_map_build_detail(const Plane *luma,
                                       N148SegmentationMap *map) {
    return build_map(luma, map, 2);
}

uint8_t n148_segmentation_map_predict(const N148SegmentationMap *map,
                                      size_t index) {
    if (!map || !map->levels || index >= map->count || map->columns <= 0)
        return N148_SEGMENT_INITIAL;
    size_t column = index % (size_t) map->columns;
    if (column > 0) return map->levels[index - 1];
    if (index >= (size_t) map->columns)
        return map->levels[index - (size_t) map->columns];
    return N148_SEGMENT_INITIAL;
}

int n148_segmentation_level_for_block(const N148SegmentationMap *map,
                                      int block_x, int block_y,
                                      int subsample_x, int subsample_y,
                                      uint8_t *level) {
    if (!n148_segmentation_map_validate(map) || !level || block_x < 0 ||
        block_y < 0 || (subsample_x != 0 && subsample_x != 1) ||
        (subsample_y != 0 && subsample_y != 1)) return 0;
    size_t canvas_x = (size_t) block_x * 8u * (1u << subsample_x);
    size_t canvas_y = (size_t) block_y * 8u * (1u << subsample_y);
    size_t region_x = canvas_x / N148_SEGMENT_REGION_SIZE;
    size_t region_y = canvas_y / N148_SEGMENT_REGION_SIZE;
    if (region_x >= (size_t) map->columns ||
        region_y >= (size_t) map->rows) return 0;
    size_t index = region_y * (size_t) map->columns + region_x;
    if (index >= map->count || map->levels[index] >= N148_SEGMENT_COUNT)
        return 0;
    *level = map->levels[index];
    return 1;
}

int n148_segmentation_filter_level(const N148SegmentationMap *map,
                                   uint8_t segment,
                                   uint8_t *filter_level) {
    if (!filter_level || !n148_segmentation_map_validate(map) ||
        segment >= N148_SEGMENT_COUNT) return 0;
    /* Give the smoothest class a slightly wider edge threshold, keep the
       middle of the frame at the accepted level, and reduce strength for the
       most irregular detail class. Large level swings were perceptually
       unstable during calibration. */
    static const uint8_t level[N148_SEGMENT_COUNT] = {2, 1, 1, 0};
    *filter_level = level[segment];
    return 1;
}

int n148_segmentation_adjust_table(const int input[8][8],
                                   const N148SegmentationMap *map,
                                   uint8_t level, int plane_index,
                                   int output[8][8]) {
    /* Chroma remains neutral under segmentation. Calibrated chroma quantization
       already tuned it on a
       held-out set, while the channel diagnosis locates the remaining gap in
       luma. Conservative luma scales limit metric regressions during tuning. */
    if (!input || !output || !n148_segmentation_map_validate(map) ||
        level >= N148_SEGMENT_COUNT || plane_index < 0 || plane_index > 2)
        return 0;
    int scale = plane_index == 0 ? map->quant_scale[level] : 100;
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int value = (input[row][column] * scale + 50) / 100;
            if (value < 1) value = 1;
            if (value > 255) value = 255;
            output[row][column] = value;
        }
    }
    return 1;
}

void n148_segmentation_map_release(N148SegmentationMap *map) {
    if (!map) return;
    free(map->levels);
    memset(map, 0, sizeof(*map));
}

/*
 * N.148i activity-masked quantization.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * The map is deliberately small and causal to decode. Level selection is an
 * encoder heuristic; region geometry and quantization multipliers are the
 * normative pieces stored by the adaptive-quantization syntax.
 */

#include "adaptive_quant.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int map_dimensions(int width, int height, int *columns, int *rows,
                          size_t *count) {
    if (width <= 0 || height <= 0 || !columns || !rows || !count) return 0;
    int map_columns = width / N148_AQ_REGION_SIZE +
        (width % N148_AQ_REGION_SIZE != 0);
    int map_rows = height / N148_AQ_REGION_SIZE +
        (height % N148_AQ_REGION_SIZE != 0);
    if ((size_t) map_columns > SIZE_MAX / (size_t) map_rows) return 0;
    size_t map_count = (size_t) map_columns * (size_t) map_rows;
    if (map_count == 0 || map_count > UINT32_MAX) return 0;
    *columns = map_columns;
    *rows = map_rows;
    *count = map_count;
    return 1;
}

int n148_adaptive_map_allocate(int width, int height, N148AdaptiveMap *map) {
    if (!map) return 0;
    memset(map, 0, sizeof(*map));
    if (!map_dimensions(width, height, &map->columns, &map->rows,
                        &map->count)) return 0;
    map->levels = (uint8_t *) calloc(map->count, 1);
    if (!map->levels) {
        memset(map, 0, sizeof(*map));
        return 0;
    }
    return 1;
}

static uint16_t activity_score(const Plane *plane, int x0, int y0) {
    int x1 = x0 + N148_AQ_REGION_SIZE;
    int y1 = y0 + N148_AQ_REGION_SIZE;
    if (x1 > plane->width) x1 = plane->width;
    if (y1 > plane->height) y1 = plane->height;
    uint32_t sum = 0;
    uint32_t comparisons = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t *row = plane->data + (long) y * plane->width;
        for (int x = x0; x < x1; x++) {
            if (x >= x0 + 2) {
                int difference = (int) row[x] - 2 * (int) row[x - 1] +
                    (int) row[x - 2];
                int magnitude = difference < 0 ? -difference : difference;
                if (magnitude > 32) magnitude = 32;
                sum += (uint32_t) magnitude;
                comparisons++;
            }
            if (y >= y0 + 2) {
                int difference = (int) row[x] -
                    2 * (int) plane->data[(long)(y - 1) * plane->width + x] +
                    (int) plane->data[(long)(y - 2) * plane->width + x];
                int magnitude = difference < 0 ? -difference : difference;
                if (magnitude > 32) magnitude = 32;
                sum += (uint32_t) magnitude;
                comparisons++;
            }
        }
    }
    if (comparisons == 0) return 0;
    return (uint16_t)((sum * 256u + comparisons / 2u) / comparisons);
}

int n148_adaptive_map_build(const Plane *intensity, N148AdaptiveMap *map) {
    if (!intensity || !intensity->data || !map ||
        !n148_adaptive_map_allocate(intensity->width, intensity->height, map))
        return 0;
    for (int row = 0; row < map->rows; row++) {
        for (int column = 0; column < map->columns; column++) {
            size_t index = (size_t) row * (size_t) map->columns +
                (size_t) column;
            uint16_t score = activity_score(
                intensity, column * N148_AQ_REGION_SIZE,
                row * N148_AQ_REGION_SIZE);
            map->levels[index] = score < 2u * 256u ? 0 :
                score < 8u * 256u ? 1 :
                score < 18u * 256u ? 2 : 3;
        }
    }
    return 1;
}

uint8_t n148_adaptive_map_predict(const N148AdaptiveMap *map, size_t index) {
    if (!map || !map->levels || index >= map->count || map->columns <= 0)
        return N148_AQ_INITIAL_LEVEL;
    size_t column = index % (size_t) map->columns;
    if (column > 0) return map->levels[index - 1];
    if (index >= (size_t) map->columns)
        return map->levels[index - (size_t) map->columns];
    return N148_AQ_INITIAL_LEVEL;
}

int n148_adaptive_level_for_block(const N148AdaptiveMap *map,
                                  int block_x, int block_y,
                                  int subsample_x, int subsample_y,
                                  uint8_t *level) {
    if (!map || !map->levels || !level || block_x < 0 || block_y < 0 ||
        (subsample_x != 0 && subsample_x != 1) ||
        (subsample_y != 0 && subsample_y != 1)) return 0;
    size_t canvas_x = (size_t) block_x * 8u * (1u << subsample_x);
    size_t canvas_y = (size_t) block_y * 8u * (1u << subsample_y);
    size_t region_x = canvas_x / N148_AQ_REGION_SIZE;
    size_t region_y = canvas_y / N148_AQ_REGION_SIZE;
    if (region_x >= (size_t) map->columns ||
        region_y >= (size_t) map->rows) return 0;
    size_t index = region_y * (size_t) map->columns + region_x;
    if (index >= map->count || map->levels[index] >= N148_AQ_LEVEL_COUNT)
        return 0;
    *level = map->levels[index];
    return 1;
}

int n148_adaptive_adjust_table(const int input[8][8], uint8_t level,
                               int output[8][8]) {
    static const int numerator[N148_AQ_LEVEL_COUNT] = {90, 97, 108, 118};
    static const int denominator[N148_AQ_LEVEL_COUNT] = {100, 100, 100, 100};
    if (!input || !output || level >= N148_AQ_LEVEL_COUNT) return 0;
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int value = (input[row][column] * numerator[level] +
                         denominator[level] / 2) / denominator[level];
            if (value < 1) value = 1;
            if (value > 255) value = 255;
            output[row][column] = value;
        }
    }
    return 1;
}

void n148_adaptive_map_release(N148AdaptiveMap *map) {
    if (!map) return;
    free(map->levels);
    memset(map, 0, sizeof(*map));
}

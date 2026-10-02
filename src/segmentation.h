/*
 * N.148i frame-level segmentation
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_SEGMENTATION_H
#define N148_SEGMENTATION_H

#include <stddef.h>
#include <stdint.h>

#include "ppm.h"

#define N148_SEGMENT_REGION_SIZE 16
#define N148_SEGMENT_COUNT 4
#define N148_SEGMENT_INITIAL 1
#define N148_SEGMENT_MIN_SCALE 75
#define N148_SEGMENT_MAX_SCALE 125

typedef struct {
    uint8_t *levels;
    size_t count;
    int columns;
    int rows;
    uint8_t quant_scale[N148_SEGMENT_COUNT];
    uint8_t adaptive_filter;
} N148SegmentationMap;

/* Build a deterministic, frame-level activity classification from luma. */
int n148_segmentation_map_build(const Plane *luma,
                                N148SegmentationMap *map);
int n148_segmentation_map_build_structural(const Plane *luma,
                                           N148SegmentationMap *map);

/* Allocate the normative 16x16 map geometry for a decoded canvas. */
int n148_segmentation_map_allocate(int width, int height,
                                   N148SegmentationMap *map);
int n148_segmentation_map_validate(const N148SegmentationMap *map);

/* Predict one map entry from information already available to the decoder. */
int n148_segmentation_map_build_detail(const Plane *luma,
                                       N148SegmentationMap *map);

uint8_t n148_segmentation_map_predict(const N148SegmentationMap *map,
                                      size_t index);

/* Resolve an 8x8 plane block to its full-resolution 16x16 segment. */
int n148_segmentation_level_for_block(const N148SegmentationMap *map,
                                      int block_x, int block_y,
                                      int subsample_x, int subsample_y,
                                      uint8_t *level);

/* Translate the ordered segment to the loop filter's strength class. */
int n148_segmentation_filter_level(const N148SegmentationMap *map,
                                   uint8_t segment, uint8_t *filter_level);

/* Apply the normative per-segment quantizer scale. Plane zero is luma. */
int n148_segmentation_adjust_table(const int input[8][8],
                                   const N148SegmentationMap *map,
                                   uint8_t level, int plane_index,
                                   int output[8][8]);

void n148_segmentation_map_release(N148SegmentationMap *map);

#endif

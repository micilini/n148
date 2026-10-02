/*
 * N.148i perceptual colour transform.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_PERCEPTUAL_H
#define N148_PERCEPTUAL_H

#include <stdint.h>

/* Prepare the deterministic transfer tables before starting worker threads. */
void n148_perceptual_init(void);

/* Convert packed sRGB bytes to intensity, red-green, and blue-yellow planes. */
void n148_rgb_to_perceptual(const uint8_t *rgb, long count,
                            uint8_t *intensity, uint8_t *red_green,
                            uint8_t *blue_yellow);

/* Convert one reconstructed perceptual sample back to packed sRGB. The two
   opponent samples carry four fractional bits from chroma interpolation. */
void n148_perceptual_to_rgb(uint8_t intensity,
                            uint16_t red_green_scaled,
                            uint16_t blue_yellow_scaled,
                            uint8_t rgb[3]);

#endif

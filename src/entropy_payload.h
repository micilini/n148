/*
 * N.148i entropy-payload interface
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_ENTROPY_PAYLOAD_H
#define N148_ENTROPY_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "adaptive_quant.h"
#include "encoder.h"
#include "ppm.h"
#include "variable_transform.h"

#define N148_EXTENDED_HEADER_SIZE 32u
#define N148_FEATURE_RANS (1u << 0)
#define N148_FEATURE_CONTEXT (1u << 1)
#define N148_FEATURE_INTRA (1u << 2)
#define N148_FEATURE_PERCEPTUAL_COLOR (1u << 3)
#define N148_FEATURE_ADAPTIVE_QUANT (1u << 4)
#define N148_FEATURE_VARIABLE_TRANSFORM (1u << 5)
#define N148_FEATURE_RDO (1u << 6)
#define N148_FEATURE_LOOP_FILTER (1u << 7)
#define N148_FORMAT_2_SUPPORTED_FEATURES \
    (N148_FEATURE_RANS | N148_FEATURE_CONTEXT | N148_FEATURE_INTRA | \
     N148_FEATURE_PERCEPTUAL_COLOR | N148_FEATURE_ADAPTIVE_QUANT | \
     N148_FEATURE_VARIABLE_TRANSFORM | N148_FEATURE_RDO | \
     N148_FEATURE_LOOP_FILTER)

typedef struct {
    uint64_t hash;
    uint32_t symbol_count;
} N148ContextTrace;

typedef struct {
    uint8_t *values;
    size_t count;
} N148PredictionModes;

typedef struct {
    uint8_t *metadata;
    size_t metadata_size;
    uint8_t *payload;
    size_t payload_size;
} N148EncodedPayload;

int n148_encode_format_2(Plane *y, Plane *cb, Plane *cr, int quality,
                   uint32_t features, int effort, N148EncodedPayload *output);

int n148_decode_format_2(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma,
                   uint32_t features,
                   Plane *y, Plane *cb, Plane *cr);

/* Validation hook: recover the exact zig-zag quantized coefficient arrays. */
int n148_decode_format_2_coefficients(const uint8_t *metadata, size_t metadata_size,
                                const uint8_t *payload, size_t payload_size,
                                int width, int height, int chroma,
                                uint32_t features,
                                N148CoeffPlane coefficients[3],
                                N148ContextTrace *trace,
                                N148PredictionModes *prediction,
                                N148AdaptiveMap *adaptive_map,
                                N148TransformMap *transform_map);

/* Validation hook: derive the encoder-side model sequence from coefficients. */
int n148_trace_base_contexts(const N148CoeffPlane coefficients[3],
                           int width, int height, int chroma,
                           uint32_t features,
                           N148ContextTrace *trace);

void n148_encoded_payload_release(N148EncodedPayload *encoded);
void n148_prediction_modes_release(N148PredictionModes *prediction);

#endif

/*
 * N.148i entropy payload
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * rANS changes only representation. Context modeling selects probability models
 * from previously decoded coefficients. Blocks are still transformed and
 * quantized by the exact format-1 path, and amplitude bits remain raw, which keeps
 * the syntax easy to inspect and proves entropy coding cannot alter values.
 */

#include "entropy_payload.h"

#include <limits.h>
#include <math.h>
#ifdef N148_RANS_DIAGNOSTICS
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "adaptive_quant.h"
#include "decoder.h"
#include "header.h"
#include "huffman.h"
#include "intra.h"
#include "loop_filter.h"
#include "rans.h"
#include "segmentation.h"
#include "variable_transform.h"
#include "predictive_codec.h"
#include "directional_intra.h"

#define ENTROPY_METADATA_REVISION_BASE 1u
#define ENTROPY_METADATA_REVISION_RICH 2u
#define PAYLOAD_REVISION_ENTROPY 1u
#define PAYLOAD_REVISION_INTRA 2u
#define PAYLOAD_REVISION_ADAPTIVE_QUANT 3u
#define PAYLOAD_REVISION_VARIABLE_TRANSFORM 4u
#define PAYLOAD_REVISION_DIRECTIONAL_INTRA 5u
#define PAYLOAD_REVISION_JOINT_CHROMA 6u
#define PAYLOAD_REVISION_SEGMENTATION 7u
#define PAYLOAD_REVISION_JOINT_SEGMENTATION 8u
#define PAYLOAD_REVISION_ADAPTIVE_FILTER 9u
#define PAYLOAD_REVISION_JOINT_ADAPTIVE_FILTER 10u
#define PAYLOAD_REVISION_VARIABLE_LUMA 11u
#define PAYLOAD_REVISION_RICH_CONTEXT 12u
#define INTRA_MODE_CODER_RANS 1u
#define ADAPTIVE_MAP_CODER_RANS 1u
#define TRANSFORM_MAP_CODER_RANS 2u
#define PARTITION_MAP_CODER_RANS 4u
#define SEGMENT_MAP_CODER_RANS 8u
#define ENTROPY_CODER_HUFFMAN 0u
#define ENTROPY_CODER_RANS 1u
#define BASE_COEFFICIENT_MODEL_COUNT 4
#define CONTEXT_DC_BUCKETS 3
#define CONTEXT_BANDS 4
#define CONTEXT_ACTIVITY_BUCKETS 2
#define CONTEXT_DC_MODELS (2 * CONTEXT_DC_BUCKETS)
#define CONTEXT_AC_MODELS \
    (2 * CONTEXT_BANDS * CONTEXT_ACTIVITY_BUCKETS)
#define CONTEXT_MODEL_COUNT \
    (CONTEXT_DC_MODELS + CONTEXT_AC_MODELS)
#define BLOCK_INTRA_MODE_CONTEXTS (N148_INTRA_MODE_COUNT + 2)
#define BLOCK_INTRA_MODELS (2 * BLOCK_INTRA_MODE_CONTEXTS)
#define ADAPTIVE_MAP_MODEL_COUNT 1
#define TRANSFORM_MAP_MODEL_COUNT 1
#define DIRECTIONAL_MODE_CONTEXTS (N148_DIRECTIONAL_INTRA_MODE_COUNT + 2)
#define DIRECTIONAL_MODELS (2 * DIRECTIONAL_MODE_CONTEXTS)
#define PARTITION_MODEL_COUNT 1
#define SEGMENTATION_MODEL_COUNT 1
#define LUMA_TRANSFORM_MODEL_COUNT 1
#define RICH_LUMA_SIZE_CLASSES 3
#define RICH_POSITION_BUCKETS 5
#define RICH_ACTIVITY_BUCKETS 3
#define RICH_PREDICTION_CLASSES 2
#define RICH_LUMA_DC_MODELS \
    (RICH_LUMA_SIZE_CLASSES * CONTEXT_DC_BUCKETS * \
     RICH_PREDICTION_CLASSES)
#define RICH_LUMA_AC_MODELS \
    (RICH_LUMA_SIZE_CLASSES * RICH_POSITION_BUCKETS * \
     RICH_ACTIVITY_BUCKETS * RICH_PREDICTION_CLASSES)
#define RICH_CHROMA_DC_MODELS CONTEXT_DC_BUCKETS
#define RICH_CHROMA_AC_MODELS \
    (CONTEXT_BANDS * CONTEXT_ACTIVITY_BUCKETS)
#define RICH_CONTEXT_MODEL_COUNT \
    (RICH_LUMA_DC_MODELS + RICH_LUMA_AC_MODELS + \
     RICH_CHROMA_DC_MODELS + RICH_CHROMA_AC_MODELS)
#define RICH_ADAPTIVE_MODEL_COUNT \
    (RICH_LUMA_DC_MODELS + RICH_LUMA_AC_MODELS)
#define RICH_ADAPTIVE_MASK_SIZE \
    ((RICH_ADAPTIVE_MODEL_COUNT + 7) / 8)
#define MAX_MODEL_COUNT 256
#define PLANE_COUNT 3

enum {
    BASE_DC_LUMA_MODEL = 0,
    BASE_AC_LUMA_MODEL = 1,
    BASE_DC_CHROMA_MODEL = 2,
    BASE_AC_CHROMA_MODEL = 3,
};

#define CONTEXT_TRACE_OFFSET 14695981039346656037ull
#define CONTEXT_TRACE_PRIME 1099511628211ull

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int valid;
} ByteWriter;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t position;
    int valid;
} ByteReader;

typedef struct {
    ByteWriter symbols;
    ByteWriter model_indexes;
    ByteWriter amplitudes;
    size_t amplitude_bits;
} TokenStream;

typedef struct {
    const uint8_t *next;
    const uint8_t *end;
    uint64_t cache;
    size_t bits_remaining;
    unsigned int cached_bits;
    int valid;
} BitReader;

typedef struct {
    ByteWriter *writer;
    size_t bit_count;
} PackedWriter;

typedef struct {
    ByteReader *reader;
    size_t bit_count;
    int valid;
} PackedReader;

static void writer_init(ByteWriter *writer) {
    memset(writer, 0, sizeof(*writer));
    writer->valid = 1;
}

static int writer_reserve(ByteWriter *writer, size_t extra) {
    if (!writer->valid || extra > SIZE_MAX - writer->size) return 0;
    size_t needed = writer->size + extra;
    if (needed <= writer->capacity) return 1;
    size_t capacity = writer->capacity ? writer->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    uint8_t *grown = (uint8_t *) realloc(writer->data, capacity);
    if (!grown) {
        writer->valid = 0;
        return 0;
    }
    writer->data = grown;
    writer->capacity = capacity;
    return 1;
}

static int writer_bytes(ByteWriter *writer, const void *data, size_t size) {
    if (!writer_reserve(writer, size)) return 0;
    if (size) memcpy(writer->data + writer->size, data, size);
    writer->size += size;
    return 1;
}

static int writer_u8(ByteWriter *writer, uint8_t value) {
    return writer_bytes(writer, &value, 1);
}

static int writer_u32(ByteWriter *writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t) value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static int writer_varuint(ByteWriter *writer, uint32_t value) {
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7;
        if (value) byte |= 0x80u;
        if (!writer_u8(writer, byte)) return 0;
    } while (value);
    return 1;
}

static uint8_t *writer_detach(ByteWriter *writer, size_t *size) {
    if (!writer->valid) return NULL;
    uint8_t *data = writer->data;
    *size = writer->size;
    writer->data = NULL;
    writer->size = 0;
    writer->capacity = 0;
    return data;
}

static void writer_release(ByteWriter *writer) {
    free(writer->data);
    memset(writer, 0, sizeof(*writer));
}

static void reader_init(ByteReader *reader, const uint8_t *data, size_t size) {
    reader->data = data;
    reader->size = size;
    reader->position = 0;
    reader->valid = data != NULL || size == 0;
}

static const uint8_t *reader_bytes(ByteReader *reader, size_t size) {
    if (!reader->valid || reader->position > reader->size ||
        size > reader->size - reader->position) {
        reader->valid = 0;
        return NULL;
    }
    const uint8_t *result = reader->data + reader->position;
    reader->position += size;
    return result;
}

static uint8_t reader_u8(ByteReader *reader) {
    const uint8_t *bytes = reader_bytes(reader, 1);
    return bytes ? bytes[0] : 0;
}

static uint32_t reader_u32(ByteReader *reader) {
    const uint8_t *bytes = reader_bytes(reader, 4);
    return bytes ? (uint32_t) bytes[0] |
        ((uint32_t) bytes[1] << 8) |
        ((uint32_t) bytes[2] << 16) |
        ((uint32_t) bytes[3] << 24) : 0;
}

static uint32_t reader_varuint(ByteReader *reader) {
    uint32_t value = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        uint8_t byte = reader_u8(reader);
        if (!reader->valid) return 0;
        if (shift == 28 && (byte & 0xf0u)) {
            reader->valid = 0;
            return 0;
        }
        value |= (uint32_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) {
            if (shift > 0 && (byte & 0x7fu) == 0) reader->valid = 0;
            return reader->valid ? value : 0;
        }
    }
    reader->valid = 0;
    return 0;
}

static int reader_finished(const ByteReader *reader) {
    return reader->valid && reader->position == reader->size;
}

static void packed_writer_init(PackedWriter *packed, ByteWriter *writer) {
    packed->writer = writer;
    packed->bit_count = 0;
}

static int packed_writer_bits(PackedWriter *packed, uint32_t value, int count) {
    for (int bit = count - 1; bit >= 0; bit--) {
        size_t offset = packed->bit_count & 7u;
        if (offset == 0 && !writer_u8(packed->writer, 0)) return 0;
        if (value & (1u << bit))
            packed->writer->data[packed->writer->size - 1] |=
                (uint8_t)(1u << (7u - offset));
        packed->bit_count++;
    }
    return 1;
}

static void packed_reader_init(PackedReader *packed, ByteReader *reader) {
    packed->reader = reader;
    packed->bit_count = 0;
    packed->valid = reader->valid;
}

static uint32_t packed_reader_bits(PackedReader *packed, int count) {
    uint32_t value = 0;
    if (!packed->valid || count < 0 || count > 16) {
        packed->valid = 0;
        return 0;
    }
    for (int bit = 0; bit < count; bit++) {
        size_t byte = packed->reader->position + (packed->bit_count >> 3);
        if (byte >= packed->reader->size) {
            packed->valid = 0;
            return 0;
        }
        value = (value << 1) |
            ((packed->reader->data[byte] >>
              (7u - (packed->bit_count & 7u))) & 1u);
        packed->bit_count++;
    }
    return value;
}

static int packed_reader_finish(PackedReader *packed) {
    if (!packed->valid) {
        packed->reader->valid = 0;
        return 0;
    }
    size_t bytes = (packed->bit_count + 7u) / 8u;
    if (packed->reader->position > packed->reader->size ||
        bytes > packed->reader->size - packed->reader->position) {
        packed->reader->valid = 0;
        return 0;
    }
    if ((packed->bit_count & 7u) && bytes) {
        uint8_t padding =
            (uint8_t)((1u << (8u - (packed->bit_count & 7u))) - 1u);
        if (packed->reader->data[packed->reader->position + bytes - 1] &
            padding) {
            packed->reader->valid = 0;
            return 0;
        }
    }
    packed->reader->position += bytes;
    return 1;
}

static int coefficient_category(int value) {
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

static uint32_t coefficient_amplitude(int value, int bits) {
    if (bits == 0) return 0;
    uint32_t mask = (1u << bits) - 1u;
    return value < 0 ? ((uint32_t)(value - 1) & mask) :
        ((uint32_t) value & mask);
}

static int token_bits(TokenStream *tokens, uint32_t value, int count) {
    if (count < 0 || count > 16) return 0;
    /* Insert the next byte fragment in stream order, including when the
       previous coefficient left a partially filled byte. */
    while (count) {
        unsigned offset = (unsigned)(tokens->amplitude_bits & 7u);
        if (offset == 0 && !writer_u8(&tokens->amplitudes, 0)) return 0;
        int chunk = 8 - (int)offset;
        if (chunk > count) chunk = count;
        unsigned bits = (value >> (count - chunk)) & ((1u << chunk) - 1u);
        tokens->amplitudes.data[tokens->amplitudes.size - 1] |=
            (uint8_t)(bits << (8 - offset - chunk));
        tokens->amplitude_bits += (unsigned)chunk;
        count -= chunk;
    }
    return 1;
}

static int token_symbol(TokenStream *tokens, int model, uint8_t symbol) {
    return writer_u8(&tokens->symbols, symbol) &&
        writer_u8(&tokens->model_indexes, (uint8_t) model);
}

static int context_dc_bucket(int previous_category) {
    if (previous_category == 0) return 0;
    return previous_category <= 3 ? 1 : 2;
}

static int context_band(int position) {
    return (position >= 6) + (position >= 15) + (position >= 28);
}

static int neighbor_is_active(const N148CoeffPlane *coefficients,
                              long block, int blocks_x, int position) {
    if (block % blocks_x != 0) {
        int value = coefficients->coefficients[(block - 1) * 64 + position];
        if (value <= -4 || value >= 4) return 1;
    }
    if (block >= blocks_x) {
        int value = coefficients->coefficients[
            (block - blocks_x) * 64 + position];
        if (value <= -4 || value >= 4) return 1;
    }
    return 0;
}

static int context_dc_model(int plane_class, int previous_category) {
    return plane_class * CONTEXT_DC_BUCKETS +
        context_dc_bucket(previous_category);
}

static int context_ac_model(const N148CoeffPlane *coefficients,
                            long block, int blocks_x, int plane_class,
                            int position, int previous_category) {
    int active = previous_category >= 3 ||
        neighbor_is_active(coefficients, block, blocks_x, position);
    return CONTEXT_DC_MODELS +
        ((plane_class * CONTEXT_BANDS + context_band(position)) *
         CONTEXT_ACTIVITY_BUCKETS) + active;
}

static void context_trace_init(N148ContextTrace *trace) {
    if (!trace) return;
    trace->hash = CONTEXT_TRACE_OFFSET;
    trace->symbol_count = 0;
}

static int context_trace_add(N148ContextTrace *trace, int model) {
    if (!trace) return 1;
    if (trace->symbol_count == UINT32_MAX || model < 0 || model > UINT8_MAX)
        return 0;
    trace->hash ^= (uint8_t) model;
    trace->hash *= CONTEXT_TRACE_PRIME;
    trace->symbol_count++;
    return 1;
}

static int tokenize_plane(const N148CoeffPlane *coefficients,
                          int blocks_x, int plane_class, int use_context,
                          int absolute_dc,
                          TokenStream *tokens) {
    int previous_dc = 0;
    int previous_dc_category = 0;
    int base_dc_model = plane_class ? BASE_DC_CHROMA_MODEL : BASE_DC_LUMA_MODEL;
    int base_ac_model = plane_class ? BASE_AC_CHROMA_MODEL : BASE_AC_LUMA_MODEL;

    for (long block = 0; block < coefficients->count; block++) {
        const short *values = coefficients->coefficients + block * 64;
        int difference = absolute_dc ? values[0] : values[0] - previous_dc;
        previous_dc = values[0];
        int bits = coefficient_category(difference);
        int model = use_context ?
            context_dc_model(plane_class, previous_dc_category) :
            base_dc_model;
        if (bits > 16 || !token_symbol(tokens, model, (uint8_t) bits) ||
            !token_bits(tokens, coefficient_amplitude(difference, bits), bits)) {
            return 0;
        }
        previous_dc_category = bits;

        int next_position = 1;
        int previous_ac_category = 0;
        unsigned long long mask = coefficients->nonzero_masks[block] & ~1ull;
        while (mask) {
            int position = 0;
#if defined(__GNUC__) || defined(__clang__)
            position = __builtin_ctzll(mask);
#else
            unsigned long long probe = mask;
            while (!(probe & 1u)) { position++; probe >>= 1; }
#endif
            int run = position - next_position;
            while (run > 15) {
                model = use_context ? context_ac_model(
                    coefficients, block, blocks_x, plane_class,
                    next_position, previous_ac_category) : base_ac_model;
                if (!token_symbol(tokens, model, 0xf0u)) return 0;
                next_position += 16;
                previous_ac_category = 0;
                run -= 16;
            }
            int value = values[position];
            bits = coefficient_category(value);
            model = use_context ? context_ac_model(
                coefficients, block, blocks_x, plane_class,
                next_position, previous_ac_category) : base_ac_model;
            if (bits < 1 || bits > 15 ||
                !token_symbol(tokens, model,
                              (uint8_t)((run << 4) | bits)) ||
                !token_bits(tokens, coefficient_amplitude(value, bits), bits)) {
                return 0;
            }
            next_position = position + 1;
            previous_ac_category = bits;
            mask &= mask - 1;
        }
        if (next_position < 64) {
            model = use_context ? context_ac_model(
                coefficients, block, blocks_x, plane_class,
                next_position, previous_ac_category) : base_ac_model;
            if (!token_symbol(tokens, model, 0)) return 0;
        }
    }
    return tokens->symbols.valid && tokens->model_indexes.valid &&
        tokens->amplitudes.valid &&
        tokens->symbols.size == tokens->model_indexes.size;
}

/* A split directional-intra cell contains four independent 4x4 transforms. Encoding their
   DC and end-of-block events independently is both the truthful syntax and a
   materially better rate model than pretending the packed values form one
   64-coefficient transform. */
static int directional_context_ac_model(const N148CoeffPlane *coefficients,
                               long block, int blocks_x, int plane_class,
                               int packed_position, int local_position,
                               int previous_category) {
    int active = previous_category >= 3 || neighbor_is_active(
        coefficients, block, blocks_x, packed_position);
    return CONTEXT_DC_MODELS +
        ((plane_class * CONTEXT_BANDS + context_band(local_position)) *
         CONTEXT_ACTIVITY_BUCKETS) + active;
}

static int tokenize_directional_coefficient_unit(
    const N148CoeffPlane *coefficients, long block, int blocks_x,
    int plane_class, int use_context, int packed_offset, int length,
    int *previous_dc_category, TokenStream *tokens) {
    const short *values = coefficients->coefficients + block * 64 +
        packed_offset;
    int base_dc_model = plane_class ? BASE_DC_CHROMA_MODEL : BASE_DC_LUMA_MODEL;
    int base_ac_model = plane_class ? BASE_AC_CHROMA_MODEL : BASE_AC_LUMA_MODEL;
    int bits = coefficient_category(values[0]);
    int model = use_context ?
        context_dc_model(plane_class, *previous_dc_category) : base_dc_model;
    if (bits > 16 || !token_symbol(tokens, model, (uint8_t) bits) ||
        !token_bits(tokens, coefficient_amplitude(values[0], bits), bits))
        return 0;
    *previous_dc_category = bits;

    int next_position = 1;
    int previous_ac_category = 0;
    for (int position = 1; position < length; position++) {
        int value = values[position];
        if (value == 0) continue;
        int run = position - next_position;
        while (run > 15) {
            model = use_context ? directional_context_ac_model(
                coefficients, block, blocks_x, plane_class,
                packed_offset + next_position, next_position,
                previous_ac_category) : base_ac_model;
            if (!token_symbol(tokens, model, 0xf0u)) return 0;
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        bits = coefficient_category(value);
        model = use_context ? directional_context_ac_model(
            coefficients, block, blocks_x, plane_class,
            packed_offset + next_position, next_position,
            previous_ac_category) : base_ac_model;
        if (bits < 1 || bits > 15 ||
            !token_symbol(tokens, model, (uint8_t)((run << 4) | bits)) ||
            !token_bits(tokens, coefficient_amplitude(value, bits), bits))
            return 0;
        next_position = position + 1;
        previous_ac_category = bits;
    }
    if (next_position < length) {
        model = use_context ? directional_context_ac_model(
            coefficients, block, blocks_x, plane_class,
            packed_offset + next_position, next_position,
            previous_ac_category) : base_ac_model;
        if (!token_symbol(tokens, model, 0)) return 0;
    }
    return 1;
}

static int tokenize_directional_coefficient_plane(
    const N148CoeffPlane *coefficients, const uint8_t *splits,
    int blocks_x, int plane_class, int use_context, TokenStream *tokens) {
    if (!coefficients || !coefficients->coefficients || !splits ||
        blocks_x <= 0 || !tokens) return 0;
    int previous_dc_category = 0;
    for (long block = 0; block < coefficients->count; block++) {
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int length = splits[block] ? 16 : 64;
        for (int part = 0; part < parts; part++) {
            if (!tokenize_directional_coefficient_unit(
                    coefficients, block, blocks_x, plane_class, use_context,
                    part * 16, length, &previous_dc_category, tokens))
                return 0;
        }
    }
    return tokens->symbols.valid && tokens->model_indexes.valid &&
        tokens->amplitudes.valid &&
        tokens->symbols.size == tokens->model_indexes.size;
}

static int large_transform_ac_model(int plane_class, int local_position,
                             int previous_category) {
    int active = previous_category >= 3;
    return CONTEXT_DC_MODELS +
        ((plane_class * CONTEXT_BANDS + context_band(local_position)) *
         CONTEXT_ACTIVITY_BUCKETS) + active;
}

static int tokenize_large_transform_coefficient_unit(
    const short *values, int length, int plane_class, int use_context,
    int *previous_dc_category, TokenStream *tokens) {
    if (!values || (length != 16 * 16 && length != 32 * 32)) return 0;
    int base_dc_model = plane_class ? BASE_DC_CHROMA_MODEL : BASE_DC_LUMA_MODEL;
    int base_ac_model = plane_class ? BASE_AC_CHROMA_MODEL : BASE_AC_LUMA_MODEL;
    int bits = coefficient_category(values[0]);
    int model = use_context ?
        context_dc_model(plane_class, *previous_dc_category) : base_dc_model;
    if (bits > 16 || !token_symbol(tokens, model, (uint8_t) bits) ||
        !token_bits(tokens, coefficient_amplitude(values[0], bits), bits))
        return 0;
    *previous_dc_category = bits;
    int next_position = 1;
    int previous_ac_category = 0;
    for (int position = 1; position < length; position++) {
        int value = values[position];
        if (value == 0) continue;
        int run = position - next_position;
        while (run > 15) {
            model = use_context ? large_transform_ac_model(
                plane_class, next_position, previous_ac_category) :
                base_ac_model;
            if (!token_symbol(tokens, model, 0xf0u)) return 0;
            next_position += 16;
            previous_ac_category = 0;
            run -= 16;
        }
        bits = coefficient_category(value);
        model = use_context ? large_transform_ac_model(
            plane_class, next_position, previous_ac_category) :
            base_ac_model;
        if (bits < 1 || bits > 15 ||
            !token_symbol(tokens, model, (uint8_t)((run << 4) | bits)) ||
            !token_bits(tokens, coefficient_amplitude(value, bits), bits))
            return 0;
        next_position = position + 1;
        previous_ac_category = bits;
    }
    if (next_position < length) {
        model = use_context ? large_transform_ac_model(
            plane_class, next_position, previous_ac_category) :
            base_ac_model;
        if (!token_symbol(tokens, model, 0)) return 0;
    }
    return 1;
}

static int tokenize_variable_luma_coefficient_plane(
    const N148CoeffPlane *coefficients, const uint8_t *splits,
    int blocks_x, int blocks_y, int use_context,
    const N148TransformMap *transform_map, TokenStream *tokens) {
    if (!coefficients || !coefficients->coefficients || !splits ||
        blocks_x <= 0 || blocks_y <= 0 || !tokens ||
        coefficients->count != (long) blocks_x * blocks_y ||
        !n148_luma_transform_map_validate_extended(transform_map, 1))
        return 0;
    int previous_dc_category = 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                short sequence[32 * 32];
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                if (!tokenize_large_transform_coefficient_unit(
                        sequence, 32 * 32, 0, use_context,
                        &previous_dc_category, tokens)) return 0;
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                short sequence[16 * 16];
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_x + (cell & 1);
                    int block_y = first_y + (cell >> 1);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                if (!tokenize_large_transform_coefficient_unit(
                        sequence, 16 * 16, 0, use_context,
                        &previous_dc_category, tokens)) return 0;
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    int length = splits[block] ? 16 : 64;
                    for (int part = 0; part < parts; part++)
                        if (!tokenize_directional_coefficient_unit(
                                coefficients, block, blocks_x, 0,
                                use_context, part * 16, length,
                                &previous_dc_category, tokens)) return 0;
                }
            }
        }
    }
    return tokens->symbols.valid && tokens->model_indexes.valid &&
        tokens->amplitudes.valid &&
        tokens->symbols.size == tokens->model_indexes.size;
}

static int rich_position_bucket(int position) {
    if (position <= 3) return position - 1;
    if (position < 15) return 3;
    return 4;
}

static int rich_prediction_class(int mode) {
    return n148_intra_base_mode(mode) == N148_DIRECTIONAL_INTRA_DC ? 0 : 1;
}

static int rich_luma_dc_model(int size_class, int prediction_mode,
                                 int previous_category) {
    int prediction_class = rich_prediction_class(prediction_mode);
    return (size_class * RICH_PREDICTION_CLASSES + prediction_class) *
        CONTEXT_DC_BUCKETS + context_dc_bucket(previous_category);
}

static int rich_activity_bucket(const N148CoeffPlane *coefficients,
                                   long block, int blocks_x,
                                   int packed_position,
                                   int previous_magnitude) {
    int magnitude_sum = previous_magnitude;
    if (coefficients && packed_position >= 0 && packed_position < 64) {
        if (block % blocks_x != 0) {
            int value = coefficients->coefficients[
                (block - 1) * 64 + packed_position];
            magnitude_sum += value < 0 ? -value : value;
        }
        if (block >= blocks_x) {
            int value = coefficients->coefficients[
                (block - blocks_x) * 64 + packed_position];
            magnitude_sum += value < 0 ? -value : value;
        }
    }
    if (magnitude_sum == 0) return 0;
    return magnitude_sum <= 3 ? 1 : 2;
}

static int rich_luma_ac_model(int size_class, int prediction_mode,
                                 int position, int activity_bucket) {
    int prediction_class = rich_prediction_class(prediction_mode);
    return RICH_LUMA_DC_MODELS +
        ((((size_class * RICH_PREDICTION_CLASSES + prediction_class) *
            RICH_POSITION_BUCKETS) +
           rich_position_bucket(position)) *
          RICH_ACTIVITY_BUCKETS) + activity_bucket;
}

static int rich_chroma_dc_model(int previous_category) {
    return RICH_LUMA_DC_MODELS + RICH_LUMA_AC_MODELS +
        context_dc_bucket(previous_category);
}

static int rich_chroma_ac_model(const N148CoeffPlane *coefficients,
                                   long block, int blocks_x,
                                   int packed_position, int local_position,
                                   int previous_category) {
    int active = previous_category >= 3 || neighbor_is_active(
        coefficients, block, blocks_x, packed_position);
    return RICH_LUMA_DC_MODELS + RICH_LUMA_AC_MODELS +
        RICH_CHROMA_DC_MODELS +
        context_band(local_position) * CONTEXT_ACTIVITY_BUCKETS + active;
}

static int tokenize_rich_coefficient_unit(
    const N148CoeffPlane *coefficients, long block, int blocks_x,
    int plane_class, int packed_offset, int length, int size_class,
    int prediction_mode, int *previous_dc_category, TokenStream *tokens) {
    const short *values = coefficients->coefficients + block * 64 +
        packed_offset;
    int bits = coefficient_category(values[0]);
    int model = plane_class ?
        rich_chroma_dc_model(*previous_dc_category) :
        rich_luma_dc_model(
            size_class, prediction_mode, *previous_dc_category);
    if (bits > 16 || !token_symbol(tokens, model, (uint8_t) bits) ||
        !token_bits(tokens, coefficient_amplitude(values[0], bits), bits))
        return 0;
    *previous_dc_category = bits;

    int next_position = 1;
    int previous_ac_category = 0;
    int previous_magnitude = 0;
    for (int position = 1; position < length; position++) {
        int value = values[position];
        if (value == 0) continue;
        int run = position - next_position;
        while (run > 15) {
            if (plane_class) {
                model = rich_chroma_ac_model(
                    coefficients, block, blocks_x,
                    packed_offset + next_position, next_position,
                    previous_ac_category);
            } else {
                int activity = rich_activity_bucket(
                    coefficients, block, blocks_x,
                    packed_offset + next_position, previous_magnitude);
                model = rich_luma_ac_model(
                    size_class, prediction_mode, next_position, activity);
            }
            if (!token_symbol(tokens, model, 0xf0u)) return 0;
            next_position += 16;
            previous_ac_category = 0;
            previous_magnitude = 0;
            run -= 16;
        }
        bits = coefficient_category(value);
        if (plane_class) {
            model = rich_chroma_ac_model(
                coefficients, block, blocks_x,
                packed_offset + next_position, next_position,
                previous_ac_category);
        } else {
            int activity = rich_activity_bucket(
                coefficients, block, blocks_x,
                packed_offset + next_position, previous_magnitude);
            model = rich_luma_ac_model(
                size_class, prediction_mode, next_position, activity);
        }
        if (bits < 1 || bits > 15 ||
            !token_symbol(tokens, model, (uint8_t)((run << 4) | bits)) ||
            !token_bits(tokens, coefficient_amplitude(value, bits), bits))
            return 0;
        next_position = position + 1;
        previous_ac_category = bits;
        previous_magnitude = value < 0 ? -value : value;
    }
    if (next_position < length) {
        if (plane_class) {
            model = rich_chroma_ac_model(
                coefficients, block, blocks_x,
                packed_offset + next_position, next_position,
                previous_ac_category);
        } else {
            int activity = rich_activity_bucket(
                coefficients, block, blocks_x,
                packed_offset + next_position, previous_magnitude);
            model = rich_luma_ac_model(
                size_class, prediction_mode, next_position, activity);
        }
        if (!token_symbol(tokens, model, 0)) return 0;
    }
    return 1;
}

static int tokenize_rich_large_coefficient_unit(
    const short *values, int length, int size_class, int prediction_mode,
    int *previous_dc_category, TokenStream *tokens) {
    if (!values || (length != 16 * 16 && length != 32 * 32)) return 0;
    int bits = coefficient_category(values[0]);
    int model = rich_luma_dc_model(
        size_class, prediction_mode, *previous_dc_category);
    if (bits > 16 || !token_symbol(tokens, model, (uint8_t) bits) ||
        !token_bits(tokens, coefficient_amplitude(values[0], bits), bits))
        return 0;
    *previous_dc_category = bits;
    int next_position = 1;
    int previous_magnitude = 0;
    for (int position = 1; position < length; position++) {
        int value = values[position];
        if (value == 0) continue;
        int run = position - next_position;
        while (run > 15) {
            int activity = previous_magnitude == 0 ? 0 :
                (previous_magnitude <= 3 ? 1 : 2);
            model = rich_luma_ac_model(
                size_class, prediction_mode, next_position, activity);
            if (!token_symbol(tokens, model, 0xf0u)) return 0;
            next_position += 16;
            previous_magnitude = 0;
            run -= 16;
        }
        bits = coefficient_category(value);
        int activity = previous_magnitude == 0 ? 0 :
            (previous_magnitude <= 3 ? 1 : 2);
        model = rich_luma_ac_model(
            size_class, prediction_mode, next_position, activity);
        if (bits < 1 || bits > 15 ||
            !token_symbol(tokens, model, (uint8_t)((run << 4) | bits)) ||
            !token_bits(tokens, coefficient_amplitude(value, bits), bits))
            return 0;
        next_position = position + 1;
        previous_magnitude = value < 0 ? -value : value;
    }
    if (next_position < length) {
        int activity = previous_magnitude == 0 ? 0 :
            (previous_magnitude <= 3 ? 1 : 2);
        model = rich_luma_ac_model(
            size_class, prediction_mode, next_position, activity);
        if (!token_symbol(tokens, model, 0)) return 0;
    }
    return 1;
}

static int tokenize_rich_coefficient_plane(
    const N148CoeffPlane *coefficients, const uint8_t *splits,
    const uint8_t *modes, int blocks_x, int plane_class,
    TokenStream *tokens) {
    if (!coefficients || !coefficients->coefficients || !splits || !modes ||
        blocks_x <= 0 || !tokens) return 0;
    int previous_dc_category = 0;
    for (long block = 0; block < coefficients->count; block++) {
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int length = splits[block] ? 16 : 64;
        int size_class = splits[block] ? 0 : 1;
        for (int part = 0; part < parts; part++) {
            int mode = modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
            if (!tokenize_rich_coefficient_unit(
                    coefficients, block, blocks_x, plane_class,
                    part * 16, length, size_class, mode,
                    &previous_dc_category, tokens)) return 0;
        }
    }
    return tokens->symbols.valid && tokens->model_indexes.valid &&
        tokens->amplitudes.valid &&
        tokens->symbols.size == tokens->model_indexes.size;
}

static int tokenize_rich_luma_coefficient_plane(
    const N148CoeffPlane *coefficients, const uint8_t *splits,
    const uint8_t *modes, int blocks_x, int blocks_y,
    const N148TransformMap *transform_map, TokenStream *tokens) {
    if (!coefficients || !coefficients->coefficients || !splits || !modes ||
        blocks_x <= 0 || blocks_y <= 0 || !tokens ||
        coefficients->count != (long) blocks_x * blocks_y ||
        !n148_luma_transform_map_validate_extended(transform_map, 1))
        return 0;
    int previous_dc_category = 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            long first_block = (long) first_y * blocks_x + first_x;
            int mode = modes[first_block * N148_INTRA_MODE_SLOTS_PER_BLOCK];
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                short sequence[32 * 32];
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                if (!tokenize_rich_large_coefficient_unit(
                        sequence, 32 * 32, 2, mode,
                        &previous_dc_category, tokens)) return 0;
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                short sequence[16 * 16];
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_x + (cell & 1);
                    int block_y = first_y + (cell >> 1);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(sequence + cell * 64,
                           coefficients->coefficients + block * 64,
                           64 * sizeof(*sequence));
                }
                if (!tokenize_rich_large_coefficient_unit(
                        sequence, 16 * 16, 2, mode,
                        &previous_dc_category, tokens)) return 0;
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    int length = splits[block] ? 16 : 64;
                    int size_class = splits[block] ? 0 : 1;
                    for (int part = 0; part < parts; part++) {
                        mode = modes[block *
                            N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
                        if (!tokenize_rich_coefficient_unit(
                                coefficients, block, blocks_x, 0,
                                part * 16, length, size_class, mode,
                                &previous_dc_category, tokens)) return 0;
                    }
                }
            }
        }
    }
    return tokens->symbols.valid && tokens->model_indexes.valid &&
        tokens->amplitudes.valid &&
        tokens->symbols.size == tokens->model_indexes.size;
}

/* Prediction modes are spatially correlated. Contexts distinguish the first
   block, agreement with one available neighbor, and disagreement between two
   available neighbors. Luma and chroma keep separate models. */
static int prediction_mode_context(const uint8_t *modes, long block,
                                   int blocks_x) {
    int has_left = block % blocks_x != 0;
    int has_top = block >= blocks_x;
    if (!has_left && !has_top) return 0;
    int left = has_left ? modes[block - 1] : -1;
    int top = has_top ? modes[block - blocks_x] : -1;
    if (has_left && has_top && left != top) return N148_INTRA_MODE_COUNT + 1;
    return 1 + (has_left ? left : top);
}

static int prediction_mode_model(int coefficient_model_count,
                                 int plane_class, const uint8_t *modes,
                                 long block, int blocks_x) {
    return coefficient_model_count + plane_class * BLOCK_INTRA_MODE_CONTEXTS +
        prediction_mode_context(modes, block, blocks_x);
}

static int tokenize_prediction_plane(const uint8_t *modes, long count,
                                     int blocks_x, int plane_class,
                                     int coefficient_model_count,
                                     TokenStream *tokens) {
    if (!modes || count <= 0 || blocks_x <= 0) return 0;
    for (long block = 0; block < count; block++) {
        int mode = modes[block];
        int model = prediction_mode_model(
            coefficient_model_count, plane_class, modes, block, blocks_x);
        if (mode < 0 || mode >= N148_INTRA_MODE_COUNT ||
            !token_symbol(tokens, model, (uint8_t) mode)) return 0;
    }
    return 1;
}

static long directional_mode_index(int block_x, int block_y, int part,
                          int blocks_x) {
    return ((long) block_y * blocks_x + block_x) *
        N148_INTRA_MODE_SLOTS_PER_BLOCK + part;
}

static int directional_virtual_mode(const uint8_t *modes, int virtual_x,
                           int virtual_y, int blocks_x) {
    int block_x = virtual_x / 2;
    int block_y = virtual_y / 2;
    int part = (virtual_y & 1) * 2 + (virtual_x & 1);
    return modes[directional_mode_index(block_x, block_y, part, blocks_x)];
}

static int directional_reference_prediction(const uint8_t *modes, long block,
                                   int part, int blocks_x,
                                   int base_mode) {
    int block_x = (int)(block % blocks_x);
    int block_y = (int)(block / blocks_x);
    int virtual_x = block_x * 2 + (part & 1);
    int virtual_y = block_y * 2 + (part >> 1);
    int has_left = virtual_x > 0;
    int has_top = virtual_y > 0;
    int left = has_left ?
        directional_virtual_mode(modes, virtual_x - 1, virtual_y, blocks_x) : 0;
    int top = has_top ?
        directional_virtual_mode(modes, virtual_x, virtual_y - 1, blocks_x) : 0;
    int left_matches = has_left &&
        n148_intra_base_mode(left) == base_mode;
    int top_matches = has_top &&
        n148_intra_base_mode(top) == base_mode;
    if (left_matches && top_matches) {
        int left_reference = n148_intra_reference_index(left);
        int top_reference = n148_intra_reference_index(top);
        return left_reference == top_reference ? left_reference : 0;
    }
    if (left_matches) return n148_intra_reference_index(left);
    if (top_matches) return n148_intra_reference_index(top);
    return 0;
}

static int directional_mode_signal_symbol(const uint8_t *modes, long block, int part,
                                 int blocks_x, int mode,
                                 int allowed_modes) {
    if (allowed_modes <= N148_DIRECTIONAL_INTRA_MODE_COUNT) return mode;
    int base_mode = n148_intra_base_mode(mode);
    int reference = n148_intra_reference_index(mode);
    int prediction = directional_reference_prediction(
        modes, block, part, blocks_x, base_mode);
    int residual = (reference + N148_INTRA_REFERENCE_COUNT - prediction) %
        N148_INTRA_REFERENCE_COUNT;
    return base_mode + N148_DIRECTIONAL_INTRA_MODE_COUNT * residual;
}

static int directional_mode_from_signal(const uint8_t *modes, long block, int part,
                               int blocks_x, int symbol,
                               int allowed_modes) {
    if (allowed_modes <= N148_DIRECTIONAL_INTRA_MODE_COUNT) return symbol;
    int base_mode = n148_intra_base_mode(symbol);
    int residual = n148_intra_reference_index(symbol);
    int prediction = directional_reference_prediction(
        modes, block, part, blocks_x, base_mode);
    int reference = (prediction + residual) %
        N148_INTRA_REFERENCE_COUNT;
    return base_mode + N148_DIRECTIONAL_INTRA_MODE_COUNT * reference;
}

/* Directional intra represents every 8x8 cell as a 2x2 virtual mode grid. An unsplit cell
   repeats its one mode in all four positions, which lets the same causal
   context code both 8x8 and 4x4 prediction without an ambiguous grammar. */
static int directional_prediction_mode_context(const uint8_t *modes, long block,
                                      int part, int blocks_x) {
    int block_x = (int)(block % blocks_x);
    int block_y = (int)(block / blocks_x);
    int virtual_x = block_x * 2 + (part & 1);
    int virtual_y = block_y * 2 + (part >> 1);
    int has_left = virtual_x > 0;
    int has_top = virtual_y > 0;
    if (!has_left && !has_top) return 0;
    int left = has_left ? n148_intra_base_mode(
        directional_virtual_mode(modes, virtual_x - 1, virtual_y, blocks_x)) : -1;
    int top = has_top ? n148_intra_base_mode(
        directional_virtual_mode(modes, virtual_x, virtual_y - 1, blocks_x)) : -1;
    if (has_left && has_top && left != top)
        return N148_DIRECTIONAL_INTRA_MODE_COUNT + 1;
    return 1 + (has_left ? left : top);
}

static int directional_prediction_mode_model(int coefficient_model_count,
                                    int plane_class, const uint8_t *modes,
                                    long block, int part, int blocks_x) {
    return coefficient_model_count + plane_class * DIRECTIONAL_MODE_CONTEXTS +
        directional_prediction_mode_context(modes, block, part, blocks_x);
}

static int tokenize_directional_prediction_plane(
    const uint8_t *modes, long block_count, int blocks_x, int plane_class,
    int coefficient_model_count, int allowed_modes, const uint8_t *splits,
    TokenStream *tokens) {
    if (!modes || block_count <= 0 || blocks_x <= 0 ||
        allowed_modes < 1 ||
        allowed_modes > N148_PACKED_INTRA_MODE_COUNT ||
        !splits)
        return 0;
    for (long block = 0; block < block_count; block++) {
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int mode_limit = allowed_modes;
        for (int part = 0; part < parts; part++) {
            long index = block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part;
            int mode = modes[index];
            int model = directional_prediction_mode_model(
                coefficient_model_count, plane_class, modes, block, part,
                blocks_x);
            int symbol = directional_mode_signal_symbol(
                modes, block, part, blocks_x, mode, allowed_modes);
            if (mode < 0 || mode >= mode_limit ||
                (splits[block] &&
                 n148_intra_reference_index(mode) != 0) ||
                symbol < 0 || symbol >= allowed_modes ||
                !token_symbol(tokens, model, (uint8_t) symbol)) return 0;
        }
    }
    return 1;
}

static int tokenize_variable_luma_prediction_plane(
    const uint8_t *modes, long block_count, int blocks_x, int blocks_y,
    int coefficient_model_count, int allowed_modes, const uint8_t *splits,
    const N148TransformMap *transform_map, TokenStream *tokens) {
    if (!modes || block_count != (long) blocks_x * blocks_y ||
        blocks_x <= 0 || blocks_y <= 0 || !splits || !tokens ||
        allowed_modes < 1 ||
        allowed_modes > N148_PACKED_INTRA_MODE_COUNT ||
        !n148_luma_transform_map_validate_extended(transform_map, 1))
        return 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                long first_block = (long) first_y * blocks_x + first_x;
                int mode = modes[first_block *
                    N148_INTRA_MODE_SLOTS_PER_BLOCK];
                if (mode < 0 || mode >= allowed_modes) return 0;
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    for (int part = 0;
                         part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                        if (modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK +
                                  part] != mode) return 0;
                }
                int model = directional_prediction_mode_model(
                    coefficient_model_count, 0, modes, first_block, 0,
                    blocks_x);
                int symbol = directional_mode_signal_symbol(
                    modes, first_block, 0, blocks_x, mode, allowed_modes);
                if (symbol < 0 || symbol >= allowed_modes ||
                    !token_symbol(tokens, model, (uint8_t) symbol)) return 0;
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                long first_block = (long) first_y * blocks_x + first_x;
                int mode = modes[first_block *
                    N148_INTRA_MODE_SLOTS_PER_BLOCK];
                if (mode < 0 || mode >= allowed_modes) return 0;
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_x + (cell & 1);
                    int block_y = first_y + (cell >> 1);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    for (int part = 0;
                         part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                        if (modes[block *
                                  N148_INTRA_MODE_SLOTS_PER_BLOCK + part] != mode)
                            return 0;
                }
                int model = directional_prediction_mode_model(
                    coefficient_model_count, 0, modes, first_block, 0,
                    blocks_x);
                int symbol = directional_mode_signal_symbol(
                    modes, first_block, 0, blocks_x, mode, allowed_modes);
                if (symbol < 0 || symbol >= allowed_modes ||
                    !token_symbol(tokens, model, (uint8_t) symbol)) return 0;
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    for (int part = 0; part < parts; part++) {
                        int mode = modes[block *
                            N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
                        int model = directional_prediction_mode_model(
                            coefficient_model_count, 0, modes, block, part,
                            blocks_x);
                        int symbol = directional_mode_signal_symbol(
                            modes, block, part, blocks_x, mode,
                            allowed_modes);
                        if (mode < 0 || mode >= allowed_modes ||
                            (splits[block] &&
                             n148_intra_reference_index(mode) != 0) ||
                            symbol < 0 || symbol >= allowed_modes ||
                            !token_symbol(tokens, model, (uint8_t) symbol))
                            return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static int tokenize_partition_map(const N148PartitionMap *map,
                                     int model, TokenStream *tokens) {
    if (!map || !map->split || model < 0 ||
        !n148_partition_map_validate(map)) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t split = map->split[index];
        uint8_t prediction = n148_partition_map_predict(map, index);
        if (split > 1 || prediction > 1 ||
            !token_symbol(tokens, model, split ^ prediction)) return 0;
    }
    return 1;
}

static int tokenize_variable_luma_partition_map(
    const N148PartitionMap *map, const N148TransformMap *transform_map,
    int model, TokenStream *tokens) {
    if (!map || !map->split || !transform_map || model < 0 || !tokens ||
        !n148_partition_map_validate(map) ||
        !n148_luma_transform_map_validate_extended(transform_map, 1) ||
        map->blocks_x[0] != transform_map->blocks_x[0] ||
        map->blocks_y[0] != transform_map->blocks_y[0]) return 0;
    for (size_t index = 0; index < map->count; index++) {
        if (index < map->plane_offsets[1]) {
            size_t local = index;
            int block_x = (int)(local % (size_t) map->blocks_x[0]);
            int block_y = (int)(local / (size_t) map->blocks_x[0]);
            size_t transform_index = (size_t)(block_y / 2) *
                (size_t) transform_map->columns[0] +
                (size_t)(block_x / 2);
            if (n148_luma_transform_dct32_member(
                    transform_map, block_x / 2, block_y / 2) ||
                transform_map->strategies[transform_index] ==
                    N148_TRANSFORM_DCT16) {
                if (map->split[index]) return 0;
                continue;
            }
        }
        uint8_t split = map->split[index];
        uint8_t prediction = n148_partition_map_predict(map, index);
        if (split > 1 || prediction > 1 ||
            !token_symbol(tokens, model, split ^ prediction)) return 0;
    }
    return 1;
}

static int tokenize_segmentation_map(const N148SegmentationMap *map,
                                        int model, TokenStream *tokens) {
    if (!map || !map->levels || model < 0 ||
        !n148_segmentation_map_validate(map)) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t level = map->levels[index];
        uint8_t prediction = n148_segmentation_map_predict(map, index);
        if (level >= N148_SEGMENT_COUNT || prediction >= N148_SEGMENT_COUNT)
            return 0;
        uint8_t delta = (uint8_t)(
            (level + N148_SEGMENT_COUNT - prediction) &
            (N148_SEGMENT_COUNT - 1));
        if (!token_symbol(tokens, model, delta)) return 0;
    }
    return 1;
}

static int tokenize_adaptive_map(const N148AdaptiveMap *map, int model,
                                 TokenStream *tokens) {
    if (!map || !map->levels || map->count == 0 || model < 0) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t level = map->levels[index];
        uint8_t prediction = n148_adaptive_map_predict(map, index);
        if (level >= N148_AQ_LEVEL_COUNT ||
            prediction >= N148_AQ_LEVEL_COUNT) return 0;
        uint8_t delta = (uint8_t)((level + N148_AQ_LEVEL_COUNT - prediction) &
                                  (N148_AQ_LEVEL_COUNT - 1));
        if (!token_symbol(tokens, model, delta)) return 0;
    }
    return 1;
}

static int tokenize_transform_map(const N148TransformMap *map, int model,
                                  TokenStream *tokens) {
    if (!map || !map->strategies || map->count == 0 || model < 0 ||
        !n148_transform_map_validate(map)) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t strategy = map->strategies[index];
        uint8_t prediction = n148_transform_map_predict(map, index);
        if (strategy >= N148_TRANSFORM_STRATEGY_COUNT ||
            prediction >= N148_TRANSFORM_STRATEGY_COUNT) return 0;
        uint8_t delta = (uint8_t)(
            (strategy + N148_TRANSFORM_STRATEGY_COUNT - prediction) %
            N148_TRANSFORM_STRATEGY_COUNT);
        if (!token_symbol(tokens, model, delta)) return 0;
    }
    return 1;
}

static int tokenize_luma_transform_map(
    const N148TransformMap *map, int model, int dct32,
    TokenStream *tokens) {
    if (!map || !map->strategies || model < 0 || !tokens ||
        !n148_luma_transform_map_validate_extended(map, dct32)) return 0;
    for (size_t index = 0; index < map->plane_offsets[1]; index++) {
        uint8_t strategy = map->strategies[index];
        uint8_t prediction = n148_transform_map_predict(map, index);
        if ((strategy != N148_TRANSFORM_DCT8 &&
             strategy != N148_TRANSFORM_DCT16 &&
             !(dct32 && strategy == N148_TRANSFORM_DCT4)) ||
            prediction >= N148_TRANSFORM_STRATEGY_COUNT) return 0;
        uint8_t delta = (uint8_t)(
            (strategy + N148_TRANSFORM_STRATEGY_COUNT - prediction) %
            N148_TRANSFORM_STRATEGY_COUNT);
        if (!token_symbol(tokens, model, delta)) return 0;
    }
    return 1;
}

static void token_stream_release(TokenStream *tokens) {
    writer_release(&tokens->symbols);
    writer_release(&tokens->model_indexes);
    writer_release(&tokens->amplitudes);
    tokens->amplitude_bits = 0;
}

int n148_trace_base_contexts(const N148CoeffPlane coefficients[3],
                           int width, int height, int chroma,
                           uint32_t features,
                           N148ContextTrace *trace) {
    if (!coefficients || !trace || width <= 0 || height <= 0 ||
        chroma < CHROMA_444 || chroma > CHROMA_420 ||
        (features & ~N148_FORMAT_2_SUPPORTED_FEATURES)) return 0;
    TokenStream tokens;
    memset(&tokens, 0, sizeof(tokens));
    writer_init(&tokens.symbols);
    writer_init(&tokens.model_indexes);
    writer_init(&tokens.amplitudes);
    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    int widths[3] = {width, chroma_width, chroma_width};
    int heights[3] = {height, chroma_height, chroma_height};
    int success = 1;
    for (int plane = 0; plane < 3 && success; plane++) {
        int blocks_x = widths[plane] / 8 + (widths[plane] % 8 != 0);
        int blocks_y = heights[plane] / 8 + (heights[plane] % 8 != 0);
        if ((size_t) blocks_x > SIZE_MAX / (size_t) blocks_y) {
            success = 0;
            break;
        }
        size_t expected_blocks = (size_t) blocks_x * (size_t) blocks_y;
        if (!coefficients[plane].coefficients ||
            !coefficients[plane].nonzero_masks ||
            expected_blocks > LONG_MAX ||
            coefficients[plane].count != (long) expected_blocks ||
            !tokenize_plane(&coefficients[plane], blocks_x,
                            plane == 0 ? 0 : 1,
                            (features & N148_FEATURE_CONTEXT) != 0,
                            (features & N148_FEATURE_INTRA) != 0,
                            &tokens)) success = 0;
    }
    context_trace_init(trace);
    if (success && tokens.model_indexes.size <= UINT32_MAX) {
        for (size_t index = 0; index < tokens.model_indexes.size; index++) {
            if (!context_trace_add(trace, tokens.model_indexes.data[index])) {
                success = 0;
                break;
            }
        }
    } else {
        success = 0;
    }
    token_stream_release(&tokens);
    if (!success) memset(trace, 0, sizeof(*trace));
    return success;
}

static int models_from_tokens(const TokenStream *tokens,
                              N148RansModel models[MAX_MODEL_COUNT],
                              int model_count) {
    uint32_t counts[MAX_MODEL_COUNT][N148_RANS_ALPHABET_SIZE] = {{0}};
    if (model_count <= 0 || model_count > MAX_MODEL_COUNT) return 0;
    if (tokens->symbols.size != tokens->model_indexes.size ||
        tokens->symbols.size > UINT32_MAX) return 0;
    for (size_t index = 0; index < tokens->symbols.size; index++) {
        int model = tokens->model_indexes.data[index];
        int symbol = tokens->symbols.data[index];
        if (model < 0 || model >= model_count ||
            counts[model][symbol] == UINT32_MAX) return 0;
        counts[model][symbol]++;
    }
    for (int model = 0; model < model_count; model++) {
        /* Some contexts can be unreachable in a small image. A one-symbol
           placeholder keeps the table layout fixed without affecting bits. */
        if (counts[model][0] == 0) {
            int empty = 1;
            for (int symbol = 1; symbol < N148_RANS_ALPHABET_SIZE; symbol++)
                empty &= counts[model][symbol] == 0;
            if (empty) counts[model][0] = 1;
        }
        if (!n148_rans_build_model(counts[model], &models[model])) return 0;
    }
    return 1;
}

/* Rich contexts inherit a compact image-specific parent model. Only the
   22 legacy parent distributions are signalled; adaptation then specializes
   each child context independently as symbols arrive. */
static int rich_parent_model(int child_model) {
    if (child_model < 0 || child_model >= RICH_CONTEXT_MODEL_COUNT)
        return -1;
    if (child_model < RICH_LUMA_DC_MODELS)
        return child_model % CONTEXT_DC_BUCKETS;
    child_model -= RICH_LUMA_DC_MODELS;
    if (child_model < RICH_LUMA_AC_MODELS) {
        int activity = child_model % RICH_ACTIVITY_BUCKETS;
        int context = child_model / RICH_ACTIVITY_BUCKETS;
        int position_bucket = context % RICH_POSITION_BUCKETS;
        int band = position_bucket <= 2 ? 0 :
            (position_bucket == 3 ? 1 : 2);
        int active = activity == RICH_ACTIVITY_BUCKETS - 1;
        return CONTEXT_DC_MODELS +
            band * CONTEXT_ACTIVITY_BUCKETS + active;
    }
    child_model -= RICH_LUMA_AC_MODELS;
    if (child_model < RICH_CHROMA_DC_MODELS)
        return CONTEXT_DC_BUCKETS + child_model;
    child_model -= RICH_CHROMA_DC_MODELS;
    return CONTEXT_DC_MODELS +
        CONTEXT_BANDS * CONTEXT_ACTIVITY_BUCKETS + child_model;
}

static int count_table_is_empty(
    const uint32_t counts[N148_RANS_ALPHABET_SIZE]) {
    for (int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE; symbol++)
        if (counts[symbol]) return 0;
    return 1;
}

static int models_from_rich_tokens(
    const TokenStream *tokens,
    N148RansModel models[MAX_MODEL_COUNT], int model_count) {
    uint32_t counts[MAX_MODEL_COUNT][N148_RANS_ALPHABET_SIZE] = {{0}};
    N148RansModel parents[CONTEXT_MODEL_COUNT];
    if (!tokens || model_count <= RICH_CONTEXT_MODEL_COUNT ||
        model_count > MAX_MODEL_COUNT ||
        tokens->symbols.size != tokens->model_indexes.size ||
        tokens->symbols.size > UINT32_MAX) return 0;
    for (size_t index = 0; index < tokens->symbols.size; index++) {
        int logical = tokens->model_indexes.data[index];
        if (logical < 0 || logical >= model_count) return 0;
        int stored = logical < RICH_CONTEXT_MODEL_COUNT ?
            rich_parent_model(logical) :
            CONTEXT_MODEL_COUNT + logical -
                RICH_CONTEXT_MODEL_COUNT;
        int symbol = tokens->symbols.data[index];
        if (stored < 0 || stored >= MAX_MODEL_COUNT ||
            counts[stored][symbol] == UINT32_MAX) return 0;
        counts[stored][symbol]++;
    }
    for (int parent = 0; parent < CONTEXT_MODEL_COUNT; parent++) {
        if (count_table_is_empty(counts[parent])) counts[parent][0] = 1;
        if (!n148_rans_build_model(counts[parent], &parents[parent]))
            return 0;
    }
    for (int child = 0; child < RICH_CONTEXT_MODEL_COUNT; child++) {
        int parent = rich_parent_model(child);
        if (parent < 0) return 0;
        models[child] = parents[parent];
    }
    for (int logical = RICH_CONTEXT_MODEL_COUNT;
         logical < model_count; logical++) {
        int stored = CONTEXT_MODEL_COUNT + logical -
            RICH_CONTEXT_MODEL_COUNT;
        if (count_table_is_empty(counts[stored])) counts[stored][0] = 1;
        if (!n148_rans_build_model(counts[stored], &models[logical]))
            return 0;
    }
    return 1;
}

static int select_rich_adaptive_models(
    const TokenStream *tokens, const N148RansModel *models,
    uint8_t mask[RICH_ADAPTIVE_MASK_SIZE]) {
    if (!tokens || !models || !mask ||
        tokens->symbols.size != tokens->model_indexes.size) return 0;
    N148RansAdaptiveModel adaptive[RICH_ADAPTIVE_MODEL_COUNT];
    long double static_log[RICH_ADAPTIVE_MODEL_COUNT] = {0};
    long double adaptive_log[RICH_ADAPTIVE_MODEL_COUNT] = {0};
    uint32_t event_count[RICH_ADAPTIVE_MODEL_COUNT] = {0};
    memset(mask, 0, RICH_ADAPTIVE_MASK_SIZE);
    for (int model = 0; model < RICH_ADAPTIVE_MODEL_COUNT; model++) {
        if (!n148_rans_adaptive_model_init(&adaptive[model], &models[model]))
            return 0;
    }
    for (size_t index = 0; index < tokens->symbols.size; index++) {
        unsigned int model = tokens->model_indexes.data[index];
        if (model >= RICH_ADAPTIVE_MODEL_COUNT) continue;
        uint8_t symbol = tokens->symbols.data[index];
        uint16_t fixed_frequency = models[model].frequency[symbol];
        uint16_t moving_frequency = adaptive[model].frequency[symbol];
        if (!fixed_frequency || !moving_frequency ||
            event_count[model] == UINT32_MAX) return 0;
        static_log[model] += logl((long double) fixed_frequency);
        adaptive_log[model] += logl((long double) moving_frequency);
        event_count[model]++;
        if (!n148_rans_adaptive_model_update(
                &adaptive[model], symbol,
                N148_ENTROPY_ADAPT_RATE_SHIFT)) return 0;
    }
    const long double one_bit = logl(2.0L);
    for (int model = 0; model < RICH_ADAPTIVE_MODEL_COUNT; model++) {
        /* One bit of hysteresis avoids selecting a context on arithmetic
           noise that the interleaved byte-rANS state cannot realize. */
        if (event_count[model] > 1 &&
            adaptive_log[model] > static_log[model] + one_bit)
            mask[model >> 3] |= (uint8_t)(1u << (model & 7));
    }
    return 1;
}

static int write_model(ByteWriter *writer, const N148RansModel *model) {
    if (model->used_symbols == 0) return 0;
    uint8_t symbols[N148_RANS_ALPHABET_SIZE];
    int symbol_count = 0;
    for (int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE; symbol++) {
        if (model->frequency[symbol]) symbols[symbol_count++] = (uint8_t) symbol;
    }
    if (symbol_count != model->used_symbols ||
        !writer_u8(writer, symbol_count == 256 ? 0 : (uint8_t) symbol_count) ||
        !writer_u8(writer, symbols[0])) return 0;

    PackedWriter packed;
    packed_writer_init(&packed, writer);
    for (int index = 1; index < symbol_count; index++) {
        int delta = symbols[index] - symbols[index - 1];
        if (delta <= 15) {
            if (!packed_writer_bits(&packed, (uint32_t)(delta - 1), 4)) return 0;
        } else if (!packed_writer_bits(&packed, 15, 4) ||
                   !packed_writer_bits(&packed, symbols[index], 8)) {
            return 0;
        }
    }
    for (int index = 0; index < symbol_count - 1; index++)
        if (!writer_varuint(writer, model->frequency[symbols[index]])) return 0;
    return 1;
}

static int read_model(ByteReader *reader, N148RansModel *model) {
    memset(model, 0, sizeof(*model));
    uint16_t used_code = reader_u8(reader);
    uint16_t used = used_code ? used_code : 256;
    uint8_t symbols[N148_RANS_ALPHABET_SIZE];
    symbols[0] = reader_u8(reader);
    if (!reader->valid) return 0;
    PackedReader packed;
    packed_reader_init(&packed, reader);
    for (uint16_t index = 1; index < used; index++) {
        uint32_t delta_code = packed_reader_bits(&packed, 4);
        uint32_t symbol = delta_code < 15 ?
            (uint32_t) symbols[index - 1] + delta_code + 1u :
            packed_reader_bits(&packed, 8);
        if (!packed.valid || symbol <= symbols[index - 1] || symbol > 255 ||
            (delta_code == 15 && symbol <= symbols[index - 1] + 15u))
            return 0;
        symbols[index] = (uint8_t) symbol;
    }
    if (!packed_reader_finish(&packed)) return 0;

    uint32_t remaining = N148_RANS_TOTAL;
    for (uint16_t index = 0; index < used; index++) {
        uint32_t frequency;
        uint32_t symbols_after = (uint32_t) used - index - 1;
        if (symbols_after == 0) {
            frequency = remaining;
        } else {
            frequency = reader_varuint(reader);
            if (!reader->valid || frequency == 0 ||
                frequency > remaining - symbols_after) return 0;
        }
        model->frequency[symbols[index]] = (uint16_t) frequency;
        remaining -= frequency;
    }
    return remaining == 0 && n148_rans_prepare_model(model);
}

static int write_rich_models(ByteWriter *writer,
                                const N148RansModel *models,
                                int model_count,
                                const uint8_t adaptive_mask[
                                    RICH_ADAPTIVE_MASK_SIZE]) {
    if (!writer || !models || !adaptive_mask ||
        model_count <= RICH_CONTEXT_MODEL_COUNT ||
        model_count > MAX_MODEL_COUNT ||
        !writer_u8(writer, 2u) || !writer_u8(writer, ENTROPY_CODER_RANS) ||
        !writer_u8(writer, (uint8_t) model_count) ||
        !writer_u8(writer, N148_RANS_SCALE_BITS) ||
        !writer_u8(writer, CONTEXT_MODEL_COUNT) ||
        !writer_u8(writer, RICH_ADAPTIVE_MASK_SIZE) ||
        !writer_bytes(writer, adaptive_mask,
                      RICH_ADAPTIVE_MASK_SIZE)) return 0;
    for (int parent = 0; parent < CONTEXT_MODEL_COUNT; parent++) {
        int representative = -1;
        for (int child = 0; child < RICH_CONTEXT_MODEL_COUNT; child++) {
            if (rich_parent_model(child) == parent) {
                representative = child;
                break;
            }
        }
        /* A compact child layout may intentionally omit one legacy band.
           Such a parent is never consulted after parsing; serialize the
           deterministic model-zero placeholder to keep the grammar fixed. */
        if (representative < 0) representative = 0;
        if (!write_model(writer, &models[representative])) return 0;
    }
    for (int model = RICH_CONTEXT_MODEL_COUNT;
         model < model_count; model++) {
        if (!write_model(writer, &models[model])) return 0;
    }
    return 1;
}

static int parse_rich_models(
    const uint8_t *metadata, size_t metadata_size,
    N148RansModel models[MAX_MODEL_COUNT], int expected_model_count,
    uint8_t adaptive_mask[RICH_ADAPTIVE_MASK_SIZE],
    size_t *bytes_consumed) {
    if (!metadata || !models || !adaptive_mask || !bytes_consumed ||
        expected_model_count <= RICH_CONTEXT_MODEL_COUNT ||
        expected_model_count > MAX_MODEL_COUNT) return 0;
    ByteReader reader;
    reader_init(&reader, metadata, metadata_size);
    if (reader_u8(&reader) != 2u ||
        reader_u8(&reader) != ENTROPY_CODER_RANS ||
        reader_u8(&reader) != expected_model_count ||
        reader_u8(&reader) != N148_RANS_SCALE_BITS ||
        reader_u8(&reader) != CONTEXT_MODEL_COUNT ||
        reader_u8(&reader) != RICH_ADAPTIVE_MASK_SIZE) return 0;
    const uint8_t *stored_mask = reader_bytes(
        &reader, RICH_ADAPTIVE_MASK_SIZE);
    if (!stored_mask) return 0;
    memcpy(adaptive_mask, stored_mask, RICH_ADAPTIVE_MASK_SIZE);
    unsigned int tail_bits = RICH_ADAPTIVE_MODEL_COUNT & 7;
    if (tail_bits &&
        (adaptive_mask[RICH_ADAPTIVE_MASK_SIZE - 1] >> tail_bits) != 0)
        return 0;
    N148RansModel parents[CONTEXT_MODEL_COUNT];
    for (int parent = 0; parent < CONTEXT_MODEL_COUNT; parent++)
        if (!read_model(&reader, &parents[parent])) return 0;
    for (int child = 0; child < RICH_CONTEXT_MODEL_COUNT; child++) {
        int parent = rich_parent_model(child);
        if (parent < 0) return 0;
        models[child] = parents[parent];
    }
    for (int model = RICH_CONTEXT_MODEL_COUNT;
         model < expected_model_count; model++)
        if (!read_model(&reader, &models[model])) return 0;
    if (!reader.valid) return 0;
    *bytes_consumed = reader.position;
    return 1;
}

static int encode_rans(Plane *y, Plane *cb, Plane *cr, int quality,
                       uint32_t features, int effort,
                       N148EncodedPayload *output) {
    N148CoeffPlane coefficients[3] = {{0}};
    TokenStream tokens;
    N148RansModel models[MAX_MODEL_COUNT];
    N148AdaptiveMap adaptive_map = {0};
    N148TransformMap transform_map = {0};
    uint8_t *rans_stream = NULL;
    uint8_t *prediction_modes = NULL;
    size_t rans_size = 0;
    size_t prediction_mode_count = 0;
    ByteWriter metadata, payload;
    writer_init(&metadata);
    writer_init(&payload);
    memset(&tokens, 0, sizeof(tokens));
    writer_init(&tokens.symbols);
    writer_init(&tokens.model_indexes);
    writer_init(&tokens.amplitudes);
    int success = 0;
    int use_context = (features & N148_FEATURE_CONTEXT) != 0;
    int use_intra = (features & N148_FEATURE_INTRA) != 0;
    int use_perceptual =
        (features & N148_FEATURE_PERCEPTUAL_COLOR) != 0;
    int use_adaptive =
        (features & N148_FEATURE_ADAPTIVE_QUANT) != 0;
    int use_transform =
        (features & N148_FEATURE_VARIABLE_TRANSFORM) != 0;
    int use_rdo = (features & N148_FEATURE_RDO) != 0;
    int use_loop_filter =
        (features & N148_FEATURE_LOOP_FILTER) != 0;
    int coefficient_model_count = use_context ?
        CONTEXT_MODEL_COUNT : BASE_COEFFICIENT_MODEL_COUNT;
    int model_count = coefficient_model_count +
        (use_intra ? BLOCK_INTRA_MODELS : 0) +
        (use_adaptive ? ADAPTIVE_MAP_MODEL_COUNT : 0) +
        (use_transform ? TRANSFORM_MAP_MODEL_COUNT : 0);
    int y_blocks_x = y->width / 8 + (y->width % 8 != 0);
    int cb_blocks_x = cb->width / 8 + (cb->width % 8 != 0);
    int cr_blocks_x = cr->width / 8 + (cr->width % 8 != 0);

    if (use_adaptive && !n148_adaptive_map_build(y, &adaptive_map))
        goto cleanup;
    if (use_transform &&
        !n148_transform_map_build(y, cb, cr, &transform_map)) goto cleanup;
    if (use_intra) {
        if (!n148_intra_quantize_planes(y, cb, cr, quality, use_perceptual,
                                        use_adaptive ? &adaptive_map : NULL,
                                        use_transform ? &transform_map : NULL,
                                        use_rdo ? effort : 0,
                                        use_loop_filter,
                                        coefficients,
                                        &prediction_modes,
                                        &prediction_mode_count)) goto cleanup;
    } else if (!n148_quantize_planes_ex(y, cb, cr, quality, use_perceptual,
                                        coefficients)) {
        goto cleanup;
    }
    if (!tokenize_plane(&coefficients[0], y_blocks_x, 0, use_context,
                        use_intra,
                        &tokens) ||
        !tokenize_plane(&coefficients[1], cb_blocks_x, 1, use_context,
                        use_intra,
                        &tokens) ||
        !tokenize_plane(&coefficients[2], cr_blocks_x, 1, use_context,
                        use_intra,
                        &tokens) ||
        (use_intra &&
         (!tokenize_prediction_plane(
              prediction_modes, coefficients[0].count, y_blocks_x, 0,
              coefficient_model_count, &tokens) ||
          !tokenize_prediction_plane(
              prediction_modes + coefficients[0].count,
              coefficients[1].count, cb_blocks_x, 1,
              coefficient_model_count, &tokens) ||
          !tokenize_prediction_plane(
              prediction_modes + coefficients[0].count +
                  coefficients[1].count,
              coefficients[2].count, cr_blocks_x, 1,
              coefficient_model_count, &tokens))) ||
        (use_adaptive && !tokenize_adaptive_map(
            &adaptive_map, coefficient_model_count + BLOCK_INTRA_MODELS,
            &tokens)) ||
        (use_transform && !tokenize_transform_map(
            &transform_map, coefficient_model_count + BLOCK_INTRA_MODELS +
                (use_adaptive ? ADAPTIVE_MAP_MODEL_COUNT : 0),
            &tokens)) ||
        !models_from_tokens(&tokens, models, model_count) ||
        !n148_rans_encode_mixed(tokens.symbols.data,
                                tokens.model_indexes.data,
                                tokens.symbols.size, models,
                                (size_t) model_count,
                                &rans_stream, &rans_size)) goto cleanup;

    if (!writer_u8(&metadata, ENTROPY_METADATA_REVISION_BASE) ||
        !writer_u8(&metadata, ENTROPY_CODER_RANS) ||
        !writer_u8(&metadata, (uint8_t) model_count) ||
        !writer_u8(&metadata, N148_RANS_SCALE_BITS)) goto cleanup;
    for (int model = 0; model < model_count; model++)
        if (!write_model(&metadata, &models[model])) goto cleanup;

    uint8_t payload_version = use_transform ? PAYLOAD_REVISION_VARIABLE_TRANSFORM :
        (use_adaptive ? PAYLOAD_REVISION_ADAPTIVE_QUANT :
        (use_intra ? PAYLOAD_REVISION_INTRA :
                     PAYLOAD_REVISION_ENTROPY));
    if (!writer_u8(&payload, payload_version) ||
        !writer_u8(&payload, PLANE_COUNT) ||
        !writer_u8(&payload, use_intra ? INTRA_MODE_CODER_RANS : 0) ||
        !writer_u8(&payload,
                   (use_adaptive ? ADAPTIVE_MAP_CODER_RANS : 0) |
                   (use_transform ? TRANSFORM_MAP_CODER_RANS : 0)))
        goto cleanup;
    if (tokens.symbols.size > UINT32_MAX || rans_size > UINT32_MAX ||
        tokens.amplitude_bits > UINT32_MAX ||
        prediction_mode_count > UINT32_MAX ||
        transform_map.count > UINT32_MAX ||
        tokens.amplitudes.size != (tokens.amplitude_bits + 7u) / 8u ||
        !writer_u32(&payload, (uint32_t) tokens.symbols.size) ||
        !writer_u32(&payload, (uint32_t) rans_size) ||
        !writer_u32(&payload, (uint32_t) tokens.amplitude_bits) ||
        (use_intra && !writer_u32(
            &payload, (uint32_t) prediction_mode_count)) ||
        (use_adaptive && !writer_u32(
            &payload, (uint32_t) adaptive_map.count)) ||
        (use_transform && !writer_u32(
            &payload, (uint32_t) transform_map.count)) ||
        !writer_bytes(&payload, rans_stream, rans_size) ||
        !writer_bytes(&payload, tokens.amplitudes.data,
                      tokens.amplitudes.size)) goto cleanup;
    output->metadata = writer_detach(&metadata, &output->metadata_size);
    output->payload = writer_detach(&payload, &output->payload_size);
    if ((!output->metadata && output->metadata_size) ||
        (!output->payload && output->payload_size)) goto cleanup;
    success = 1;

cleanup:
    if (!success) n148_encoded_payload_release(output);
    writer_release(&metadata);
    writer_release(&payload);
    free(rans_stream);
    free(prediction_modes);
    n148_adaptive_map_release(&adaptive_map);
    n148_transform_map_release(&transform_map);
    for (int plane = 0; plane < 3; plane++) {
        n148_free_coeff_plane(&coefficients[plane]);
    }
    token_stream_release(&tokens);
    return success;
}

static int write_huffman_metadata(ByteWriter *metadata,
                                  const HuffSpec specs[HUFFMAN_TABLE_COUNT]) {
    if (!writer_u8(metadata, ENTROPY_METADATA_REVISION_BASE) ||
        !writer_u8(metadata, ENTROPY_CODER_HUFFMAN) ||
        !writer_u8(metadata, HUFFMAN_TABLE_COUNT) ||
        !writer_u8(metadata, 0)) return 0;
    for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
        if (!huffman_spec_is_valid(&specs[table])) return 0;
        for (int length = 1; length <= 16; length++)
            if (!writer_u8(metadata, (uint8_t) specs[table].bits[length]))
                return 0;
        if (!writer_bytes(metadata, specs[table].values,
                          (size_t) specs[table].value_count)) return 0;
    }
    return 1;
}

static int encode_huffman(Plane *y, Plane *cb, Plane *cr, int quality,
                          N148EncodedPayload *output) {
    HuffSpec specs[HUFFMAN_TABLE_COUNT];
    EncodeStats stats;
    unsigned char *payload = NULL;
    ByteWriter metadata;
    writer_init(&metadata);
    int success = 0;
    if (!encode_image(y, cb, cr, quality, 1, specs, &payload, &stats) ||
        stats.data_size <= 0 || !write_huffman_metadata(&metadata, specs)) {
        goto cleanup;
    }
    output->metadata = writer_detach(&metadata, &output->metadata_size);
    output->payload = payload;
    output->payload_size = (size_t) stats.data_size;
    payload = NULL;
    success = 1;
cleanup:
    free(payload);
    writer_release(&metadata);
    if (!success) n148_encoded_payload_release(output);
    return success;
}

int n148_encode_format_2(Plane *y, Plane *cb, Plane *cr, int quality,
                   uint32_t features, int effort, N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        effort < 0 || effort > 9 ||
        (features & ~N148_FORMAT_2_SUPPORTED_FEATURES) ||
        ((features & (N148_FEATURE_CONTEXT | N148_FEATURE_INTRA |
                      N148_FEATURE_PERCEPTUAL_COLOR |
                      N148_FEATURE_ADAPTIVE_QUANT |
                      N148_FEATURE_VARIABLE_TRANSFORM |
                      N148_FEATURE_RDO |
                      N148_FEATURE_LOOP_FILTER)) &&
         !(features & N148_FEATURE_RANS)) ||
        ((features & N148_FEATURE_ADAPTIVE_QUANT) &&
         !(features & N148_FEATURE_INTRA)) ||
        ((features & N148_FEATURE_VARIABLE_TRANSFORM) &&
         !(features & N148_FEATURE_INTRA)) ||
        ((features & N148_FEATURE_RDO) &&
         !(features & N148_FEATURE_INTRA)) ||
        ((features & N148_FEATURE_LOOP_FILTER) &&
         !(features & N148_FEATURE_INTRA))) return 0;
    memset(output, 0, sizeof(*output));
    if (features & N148_FEATURE_RANS)
        return encode_rans(y, cb, cr, quality, features, effort, output);
    return encode_huffman(y, cb, cr, quality, output);
}

int n148_format_3_features_valid(uint32_t features) {
    if (features & ~N148_FORMAT_3_SUPPORTED_FEATURES) return 0;
    if (!(features & N148_FEATURE_RANS) ||
        !(features & N148_FEATURE_INTRA)) return 0;
    if ((features & (N148_FEATURE_CONTEXT | N148_FEATURE_RDO |
                     N148_FEATURE_LOOP_FILTER |
                     N148_FEATURE_DIRECTIONAL_INTRA |
                     N148_FEATURE_INTRA_4X4 |
                     N148_FEATURE_CONTEXTUAL_RDO |
                     N148_FEATURE_PERCEPTUAL_TRELLIS)) &&
        !(features & N148_FEATURE_RANS)) return 0;
    if ((features & (N148_FEATURE_RDO | N148_FEATURE_LOOP_FILTER |
                     N148_FEATURE_DIRECTIONAL_INTRA |
                     N148_FEATURE_INTRA_4X4 |
                     N148_FEATURE_CONTEXTUAL_RDO |
                     N148_FEATURE_PERCEPTUAL_TRELLIS)) &&
        !(features & N148_FEATURE_INTRA)) return 0;
    if ((features & N148_FEATURE_CONTEXTUAL_RDO) &&
        (!(features & N148_FEATURE_RDO) ||
         !(features & N148_FEATURE_CONTEXT))) return 0;
    if ((features & N148_FEATURE_PERCEPTUAL_TRELLIS) &&
        !(features & N148_FEATURE_CONTEXTUAL_RDO)) return 0;
    return 1;
}

void n148_predictive_preparation_release(N148PredictivePreparation *prepared) {
    if (!prepared) return;
    free(prepared->prediction_modes);
    n148_partition_map_release(&prepared->partition_map);
    n148_segmentation_map_release(&prepared->segmentation_map);
    n148_transform_map_release(&prepared->transform_map);
    for (int plane = 0; plane < 3; plane++)
        n148_free_coeff_plane(&prepared->coefficients[plane]);
    memset(prepared, 0, sizeof(*prepared));
}

int n148_predictive_encode_cached(Plane *y, Plane *cb, Plane *cr, int quality,
                                  int chroma_quality, uint32_t features,
                                  int effort, int joint_chroma,
                                  int calibrated_chroma, int segmented,
                                  int adaptive_filter, int calibrated_luma,
                                  int variable_luma,
                                  int perceptual_luma_trellis,
                                  int quant_adaptive_filter, int dct32,
                                  uint32_t entropy_tools,
                                  N148EncodedPayload *output,
                                  N148PredictivePreparation *prepared) {
    N148CoeffPlane coefficients[3] = {{0}};
    TokenStream tokens;
    N148RansModel models[MAX_MODEL_COUNT];
    N148PartitionMap partition_map = {0};
    N148SegmentationMap segmentation_map = {0};
    N148TransformMap transform_map = {0};
    uint8_t *rans_stream = NULL;
    uint8_t *prediction_modes = NULL;
    uint8_t adaptive_mask[RICH_ADAPTIVE_MASK_SIZE] = {0};
    size_t rans_size = 0;
    size_t prediction_mode_count = 0;
    ByteWriter metadata, payload;
    writer_init(&metadata);
    writer_init(&payload);
    memset(&tokens, 0, sizeof(tokens));
    writer_init(&tokens.symbols);
    writer_init(&tokens.model_indexes);
    writer_init(&tokens.amplitudes);
    int success = 0;
    int use_context = (features & N148_FEATURE_CONTEXT) != 0;
    int use_directional =
        (features & N148_FEATURE_DIRECTIONAL_INTRA) != 0;
    int use_split = (features & N148_FEATURE_INTRA_4X4) != 0;
    int use_contextual_rdo =
        (features & N148_FEATURE_CONTEXTUAL_RDO) != 0;
    int use_perceptual_trellis =
        (features & N148_FEATURE_PERCEPTUAL_TRELLIS) != 0;
    int trellis_policy = use_perceptual_trellis ?
        (perceptual_luma_trellis ? N148_TRELLIS_PERCEPTUAL_LUMA : N148_TRELLIS_LEGACY) :
        N148_TRELLIS_OFF;
    int use_rdo = (features & N148_FEATURE_RDO) != 0;
    int use_loop_filter =
        (features & N148_FEATURE_LOOP_FILTER) != 0;
    int adaptive_entropy =
        (entropy_tools & N148_ENTROPY_ADAPTIVE) != 0;
    int rich_context =
        (entropy_tools & N148_ENTROPY_RICH_CONTEXT) != 0;
    int multiple_references =
        (entropy_tools & N148_ENTROPY_MULTIPLE_REFERENCES) != 0;
    int loop_filter_policy = use_loop_filter ?
        (quant_adaptive_filter ? N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA :
                                 N148_LOOP_FILTER_LEGACY) :
        N148_LOOP_FILTER_OFF;
    int coefficient_model_count = rich_context ?
        RICH_CONTEXT_MODEL_COUNT : use_context ?
        CONTEXT_MODEL_COUNT : BASE_COEFFICIENT_MODEL_COUNT;
    int model_count = coefficient_model_count + DIRECTIONAL_MODELS +
        (use_split ? PARTITION_MODEL_COUNT : 0) +
        (segmented ? SEGMENTATION_MODEL_COUNT : 0) +
        (variable_luma ? LUMA_TRANSFORM_MODEL_COUNT : 0);
    int y_blocks_x = y->width / 8 + (y->width % 8 != 0);
    int y_blocks_y = y->height / 8 + (y->height % 8 != 0);
    int cb_blocks_x = cb->width / 8 + (cb->width % 8 != 0);
    int cr_blocks_x = cr->width / 8 + (cr->width % 8 != 0);
    int chroma = CHROMA_444;
    if (cb->width < y->width)
        chroma = cb->height < y->height ? CHROMA_420 : CHROMA_422;

    if ((adaptive_filter != 0 && adaptive_filter != 1) ||
        (calibrated_luma != 0 && calibrated_luma != 1) ||
        (variable_luma != 0 && variable_luma != 1) ||
        (perceptual_luma_trellis != 0 && perceptual_luma_trellis != 1) ||
        (quant_adaptive_filter != 0 && quant_adaptive_filter != 1) ||
        (dct32 != 0 && dct32 != 1) ||
        (entropy_tools & ~(N148_ENTROPY_ADAPTIVE |
                           N148_ENTROPY_RICH_CONTEXT |
                           N148_ENTROPY_MULTIPLE_REFERENCES)) ||
        (adaptive_entropy && !(features & N148_FEATURE_RANS)) ||
        (rich_context && (!use_context || !adaptive_entropy)) ||
        (multiple_references && (!use_directional ||
                                 !use_contextual_rdo)) ||
        (dct32 && !variable_luma) ||
        (quant_adaptive_filter && (!use_loop_filter || !calibrated_luma)) ||
        (variable_luma && (!use_contextual_rdo || !use_split)) ||
        (adaptive_filter && !segmented)) goto cleanup;
    if (prepared && prepared->valid) {
        memcpy(coefficients, prepared->coefficients, sizeof(coefficients));
        partition_map = prepared->partition_map;
        segmentation_map = prepared->segmentation_map;
        transform_map = prepared->transform_map;
        prediction_modes = prepared->prediction_modes;
        prediction_mode_count = prepared->prediction_mode_count;
    } else {
        if (segmented) {
            int built = n148_fidelity_detail_reconstruction() ?
                n148_segmentation_map_build_detail(y, &segmentation_map) :
                n148_fidelity_structural_quant() ?
                n148_segmentation_map_build_structural(y, &segmentation_map) :
                n148_segmentation_map_build(y, &segmentation_map);
            if (!built) goto cleanup;
            segmentation_map.adaptive_filter = (uint8_t) adaptive_filter;
        }
        if (!n148_partition_map_allocate(y->width, y->height, chroma,
                                        &partition_map) ||
        (variable_luma && !n148_transform_map_allocate(
            y->width, y->height, chroma, &transform_map)) ||
        !(joint_chroma ? n148_joint_chroma_intra_quantize_planes(
              y, cb, cr, quality, chroma_quality, calibrated_chroma,
              calibrated_luma, use_directional, use_split, use_contextual_rdo,
              use_rdo ? effort : 0,
              trellis_policy, loop_filter_policy,
              &partition_map,
              segmented ? &segmentation_map : NULL,
              variable_luma ? &transform_map : NULL,
              coefficients, &prediction_modes,
              &prediction_mode_count) :
          n148_directional_intra_quantize_planes(
              y, cb, cr, quality, chroma_quality, calibrated_chroma,
              calibrated_luma, use_directional, use_split, use_contextual_rdo,
              use_rdo ? effort : 0,
              trellis_policy, loop_filter_policy,
              &partition_map,
              segmented ? &segmentation_map : NULL,
              variable_luma ? &transform_map : NULL,
              coefficients, &prediction_modes,
              &prediction_mode_count))) goto cleanup;
        if (prepared) {
            memcpy(prepared->coefficients, coefficients, sizeof(coefficients));
            prepared->partition_map = partition_map;
            prepared->segmentation_map = segmentation_map;
            prepared->transform_map = transform_map;
            prepared->prediction_modes = prediction_modes;
            prepared->prediction_mode_count = prediction_mode_count;
            prepared->valid = 1;
        }
    }

    size_t y_modes = (size_t) coefficients[0].count *
        N148_INTRA_MODE_SLOTS_PER_BLOCK;
    size_t cb_modes = (size_t) coefficients[1].count *
        N148_INTRA_MODE_SLOTS_PER_BLOCK;
    int allowed_modes = multiple_references ?
        N148_PACKED_INTRA_MODE_COUNT :
        (use_directional ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4);
    int chroma_allowed_modes = use_directional ?
        N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    int segmentation_model = coefficient_model_count + DIRECTIONAL_MODELS +
        (use_split ? PARTITION_MODEL_COUNT : 0);
    int transform_model = segmentation_model +
        (segmented ? SEGMENTATION_MODEL_COUNT : 0);
    int token_ok =
        (!variable_luma || tokenize_luma_transform_map(
            &transform_map, transform_model, dct32, &tokens)) &&
        (!use_split || (variable_luma ? tokenize_variable_luma_partition_map(
            &partition_map, &transform_map,
            coefficient_model_count + DIRECTIONAL_MODELS, &tokens) :
          tokenize_partition_map(
            &partition_map, coefficient_model_count + DIRECTIONAL_MODELS,
            &tokens))) &&
        (!segmented || tokenize_segmentation_map(
            &segmentation_map, segmentation_model, &tokens));
    if (!token_ok) {
#ifdef N148_RANS_DIAGNOSTICS
        fprintf(stderr, "rich-context token-map pass failed\n");
#endif
        goto cleanup;
    }

#define TOKENIZE_MODERN_MODES() \
    ((variable_luma ? tokenize_variable_luma_prediction_plane( \
          prediction_modes, coefficients[0].count, y_blocks_x, y_blocks_y, \
          coefficient_model_count, allowed_modes, \
          partition_map.split + partition_map.plane_offsets[0], \
          &transform_map, &tokens) : \
      tokenize_directional_prediction_plane( \
          prediction_modes, coefficients[0].count, y_blocks_x, 0, \
          coefficient_model_count, allowed_modes, \
          partition_map.split + partition_map.plane_offsets[0], &tokens)) && \
     tokenize_directional_prediction_plane( \
          prediction_modes + y_modes, coefficients[1].count, cb_blocks_x, \
          1, coefficient_model_count, chroma_allowed_modes, \
          partition_map.split + partition_map.plane_offsets[1], &tokens) && \
     (joint_chroma || tokenize_directional_prediction_plane( \
          prediction_modes + y_modes + cb_modes, coefficients[2].count, \
          cr_blocks_x, 1, coefficient_model_count, chroma_allowed_modes, \
          partition_map.split + partition_map.plane_offsets[2], &tokens)))

    if (rich_context) {
        token_ok = TOKENIZE_MODERN_MODES() &&
            (variable_luma ? tokenize_rich_luma_coefficient_plane(
                &coefficients[0],
                partition_map.split + partition_map.plane_offsets[0],
                prediction_modes, y_blocks_x, y_blocks_y,
                &transform_map, &tokens) :
             tokenize_rich_coefficient_plane(
                &coefficients[0],
                partition_map.split + partition_map.plane_offsets[0],
                prediction_modes, y_blocks_x, 0, &tokens)) &&
            tokenize_rich_coefficient_plane(
                &coefficients[1],
                partition_map.split + partition_map.plane_offsets[1],
                prediction_modes + y_modes, cb_blocks_x, 1, &tokens) &&
            tokenize_rich_coefficient_plane(
                &coefficients[2],
                partition_map.split + partition_map.plane_offsets[2],
                prediction_modes + y_modes + cb_modes,
                cr_blocks_x, 1, &tokens);
    } else {
        token_ok =
            (variable_luma ? tokenize_variable_luma_coefficient_plane(
                &coefficients[0],
                partition_map.split + partition_map.plane_offsets[0],
                y_blocks_x, y_blocks_y, use_context,
                &transform_map, &tokens) :
             tokenize_directional_coefficient_plane(
                &coefficients[0],
                partition_map.split + partition_map.plane_offsets[0],
                y_blocks_x, 0, use_context, &tokens)) &&
            tokenize_directional_coefficient_plane(
                &coefficients[1],
                partition_map.split + partition_map.plane_offsets[1],
                cb_blocks_x, 1, use_context, &tokens) &&
            tokenize_directional_coefficient_plane(
                &coefficients[2],
                partition_map.split + partition_map.plane_offsets[2],
                cr_blocks_x, 1, use_context, &tokens) &&
            TOKENIZE_MODERN_MODES();
    }
#undef TOKENIZE_MODERN_MODES
    if (!token_ok) {
#ifdef N148_RANS_DIAGNOSTICS
        fprintf(stderr, "rich-context coefficient/mode pass failed\n");
#endif
        goto cleanup;
    }
    if (!(rich_context ?
          models_from_rich_tokens(&tokens, models, model_count) :
          models_from_tokens(&tokens, models, model_count))) {
#ifdef N148_RANS_DIAGNOSTICS
        fprintf(stderr, "rich-context model construction failed (%d models)\n",
                model_count);
#endif
        goto cleanup;
    }
    if (rich_context &&
        !select_rich_adaptive_models(&tokens, models, adaptive_mask))
        goto cleanup;
    if (adaptive_entropy) {
        if (!n148_rans_encode_mixed_adaptive(
                tokens.symbols.data, tokens.model_indexes.data,
                tokens.symbols.size, models, (size_t) model_count,
                rich_context ?
                    RICH_LUMA_DC_MODELS + RICH_LUMA_AC_MODELS :
                    (size_t) model_count,
                rich_context ? adaptive_mask : NULL,
                rich_context ? RICH_ADAPTIVE_MODEL_COUNT : 0,
                N148_ENTROPY_ADAPT_RATE_SHIFT,
                &rans_stream, &rans_size, NULL)) goto cleanup;
    } else if (!n148_rans_encode_mixed(
                   tokens.symbols.data, tokens.model_indexes.data,
                   tokens.symbols.size, models, (size_t) model_count,
                   &rans_stream, &rans_size)) goto cleanup;
    if (rich_context) {
        uint8_t *fixed_stream = NULL;
        size_t fixed_size = 0;
        if (!n148_rans_encode_mixed(
                tokens.symbols.data, tokens.model_indexes.data,
                tokens.symbols.size, models, (size_t) model_count,
                &fixed_stream, &fixed_size)) goto cleanup;
        if (fixed_size < rans_size) {
            free(rans_stream);
            rans_stream = fixed_stream;
            rans_size = fixed_size;
            fixed_stream = NULL;
            memset(adaptive_mask, 0, sizeof(adaptive_mask));
        }
        free(fixed_stream);
    }

    if (rich_context) {
        if (!write_rich_models(
                &metadata, models, model_count, adaptive_mask)) {
#ifdef N148_RANS_DIAGNOSTICS
            fprintf(stderr, "rich-context metadata serialization failed\n");
#endif
            goto cleanup;
        }
    } else {
        if (!writer_u8(&metadata, ENTROPY_METADATA_REVISION_BASE) ||
            !writer_u8(&metadata, ENTROPY_CODER_RANS) ||
            !writer_u8(&metadata, (uint8_t) model_count) ||
            !writer_u8(&metadata, N148_RANS_SCALE_BITS)) goto cleanup;
        for (int model = 0; model < model_count; model++)
            if (!write_model(&metadata, &models[model])) goto cleanup;
    }

    size_t stored_prediction_modes = joint_chroma ?
        y_modes + cb_modes : prediction_mode_count;
    uint8_t payload_version = rich_context ?
        PAYLOAD_REVISION_RICH_CONTEXT : variable_luma ?
        PAYLOAD_REVISION_VARIABLE_LUMA : adaptive_filter ?
        (joint_chroma ? PAYLOAD_REVISION_JOINT_ADAPTIVE_FILTER :
                        PAYLOAD_REVISION_ADAPTIVE_FILTER) :
        segmented ?
        (joint_chroma ? PAYLOAD_REVISION_JOINT_SEGMENTATION :
                        PAYLOAD_REVISION_SEGMENTATION) :
        (joint_chroma ? PAYLOAD_REVISION_JOINT_CHROMA :
                        PAYLOAD_REVISION_DIRECTIONAL_INTRA);
    if (!writer_u8(&payload, payload_version) ||
        !writer_u8(&payload, PLANE_COUNT) ||
        !writer_u8(&payload, INTRA_MODE_CODER_RANS) ||
        !writer_u8(&payload, (uint8_t)(
            (use_split ? PARTITION_MAP_CODER_RANS : 0) |
            (segmented ? SEGMENT_MAP_CODER_RANS : 0) |
            (variable_luma ? TRANSFORM_MAP_CODER_RANS : 0))))
        goto cleanup;
    if (tokens.symbols.size > UINT32_MAX || rans_size > UINT32_MAX ||
        tokens.amplitude_bits > UINT32_MAX ||
        stored_prediction_modes > UINT32_MAX ||
        partition_map.count > UINT32_MAX ||
        segmentation_map.count > UINT32_MAX ||
        transform_map.plane_offsets[1] > UINT32_MAX ||
        tokens.amplitudes.size != (tokens.amplitude_bits + 7u) / 8u ||
        !writer_u32(&payload, (uint32_t) tokens.symbols.size) ||
        !writer_u32(&payload, (uint32_t) rans_size) ||
        !writer_u32(&payload, (uint32_t) tokens.amplitude_bits) ||
        !writer_u32(&payload, (uint32_t) stored_prediction_modes) ||
        (use_split && !writer_u32(
            &payload, (uint32_t) partition_map.count)) ||
        (segmented && !writer_u32(
            &payload, (uint32_t) segmentation_map.count)) ||
        (variable_luma && !writer_u32(
            &payload, (uint32_t) transform_map.plane_offsets[1])) ||
        (segmented && (!writer_u8(&payload, segmentation_map.quant_scale[0]) ||
                       !writer_u8(&payload, segmentation_map.quant_scale[1]) ||
                       !writer_u8(&payload, segmentation_map.quant_scale[2]) ||
                       !writer_u8(&payload, segmentation_map.quant_scale[3]))) ||
        !writer_bytes(&payload, rans_stream, rans_size) ||
        !writer_bytes(&payload, tokens.amplitudes.data,
                      tokens.amplitudes.size)) goto cleanup;

    output->metadata = writer_detach(&metadata, &output->metadata_size);
    output->payload = writer_detach(&payload, &output->payload_size);
    if ((!output->metadata && output->metadata_size) ||
        (!output->payload && output->payload_size)) goto cleanup;
    success = 1;

cleanup:
    if (!success) n148_encoded_payload_release(output);
    writer_release(&metadata);
    writer_release(&payload);
    free(rans_stream);
    if (!prepared || !prepared->valid) {
        free(prediction_modes);
        n148_partition_map_release(&partition_map);
        n148_segmentation_map_release(&segmentation_map);
        n148_transform_map_release(&transform_map);
        for (int plane = 0; plane < 3; plane++)
            n148_free_coeff_plane(&coefficients[plane]);
    }
    token_stream_release(&tokens);
    return success;
}

int n148_predictive_encode(Plane *y, Plane *cb, Plane *cr, int quality,
                          int chroma_quality, uint32_t features, int effort,
                          int joint_chroma, int calibrated_chroma,
                          int segmented, int adaptive_filter,
                          int calibrated_luma, int variable_luma,
                          int perceptual_luma_trellis, int quant_adaptive_filter,
                          int dct32, uint32_t entropy_tools,
                          N148EncodedPayload *output) {
    return n148_predictive_encode_cached(
        y, cb, cr, quality, chroma_quality, features, effort, joint_chroma,
        calibrated_chroma, segmented, adaptive_filter, calibrated_luma,
        variable_luma, perceptual_luma_trellis, quant_adaptive_filter, dct32,
        entropy_tools, output, NULL);
}

int n148_encode_format_3(Plane *y, Plane *cb, Plane *cr, int quality,
                   uint32_t features, int effort, N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        effort < 0 || effort > 9 || !n148_format_3_features_valid(features))
        return 0;
    memset(output, 0, sizeof(*output));
    return n148_predictive_encode(y, cb, cr, quality, quality, features, effort,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, output);
}

int n148_joint_chroma_encode(
    Plane *y, Plane *cb, Plane *cr, int quality,
    uint32_t features, int effort, N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        effort < 0 || effort > 9 || !n148_format_3_features_valid(features) ||
        !(features & N148_FEATURE_CONTEXTUAL_RDO)) return 0;
    memset(output, 0, sizeof(*output));
    return n148_predictive_encode(y, cb, cr, quality, quality, features, effort,
                                 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, output);
}

int n148_calibrated_chroma_encode(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma,
    N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 ||
        effort < 0 || effort > 9 || !n148_format_3_features_valid(features) ||
        (joint_chroma != 0 && joint_chroma != 1) ||
        (joint_chroma &&
         !(features & N148_FEATURE_CONTEXTUAL_RDO))) return 0;
    memset(output, 0, sizeof(*output));
    return n148_predictive_encode(
        y, cb, cr, quality, chroma_quality, features,
        effort, joint_chroma, 1, 0, 0, 0, 0, 0, 0, 0, 0, output);
}

int n148_segmented_encode(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma,
    N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 ||
        effort < 0 || effort > 9 || !n148_format_3_features_valid(features) ||
        (joint_chroma != 0 && joint_chroma != 1) ||
        !(features & N148_FEATURE_CONTEXTUAL_RDO)) return 0;
    memset(output, 0, sizeof(*output));
    return n148_predictive_encode(
        y, cb, cr, quality, chroma_quality, features,
        effort, joint_chroma, 1, 1, 0, 0, 0, 0, 0, 0, 0, output);
}

int n148_adaptive_filter_encode(
    Plane *y, Plane *cb, Plane *cr, int quality, int chroma_quality,
    uint32_t features, int effort, int joint_chroma,
    N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 ||
        effort < 0 || effort > 9 || !n148_format_3_features_valid(features) ||
        (joint_chroma != 0 && joint_chroma != 1) ||
        !(features & N148_FEATURE_CONTEXTUAL_RDO) ||
        !(features & N148_FEATURE_LOOP_FILTER)) return 0;
    memset(output, 0, sizeof(*output));
    return n148_predictive_encode(
        y, cb, cr, quality, chroma_quality, features,
        effort, joint_chroma, 1, 1, 1, 0, 0, 0, 0, 0, 0, output);
}

static void bit_reader_init(BitReader *reader, const uint8_t *data,
                            size_t byte_count, size_t bit_count) {
    reader->next = data;
    reader->end = data + byte_count;
    reader->cache = 0;
    reader->bits_remaining = bit_count;
    reader->cached_bits = 0;
    reader->valid = bit_count <= SIZE_MAX - 7u &&
        byte_count == (bit_count + 7u) / 8u;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline uint32_t bit_reader_read(BitReader *reader, int count) {
    if (!reader->valid || count < 0 || count > 16 ||
        (size_t) count > reader->bits_remaining) {
        reader->valid = 0;
        return 0;
    }
    if (count == 0) return 0;
    unsigned int available = reader->cached_bits;
    uint64_t cache = reader->cache;
    const uint8_t *next = reader->next;
    if (available < (unsigned int) count) {
        /* Amplitudes are at most 16 bits. Refill four bytes at once whenever
           possible and retain the unused suffix for following coefficients.
           The byte spelling lets compilers select an unaligned load plus a
           byte swap without permitting an out-of-bounds speculative read. */
        if ((size_t)(reader->end - next) >= 4u) {
            uint32_t word = ((uint32_t) next[0] << 24) |
                ((uint32_t) next[1] << 16) |
                ((uint32_t) next[2] << 8) | next[3];
            cache = (cache << 32) | word;
            next += 4;
            available += 32;
        } else {
            do {
                if (next == reader->end) {
                    reader->valid = 0;
                    return 0;
                }
                cache = (cache << 8) | *next++;
                available += 8;
            } while (available < (unsigned int) count);
        }
    }
    available -= (unsigned int) count;
    reader->next = next;
    reader->cache = cache;
    reader->cached_bits = available;
    reader->bits_remaining -= (size_t) count;
    return (uint32_t)(cache >> available) & ((1u << count) - 1u);
}

static inline int decode_amplitude(BitReader *reader, int bits, int *value) {
    if (bits == 0) {
        *value = 0;
        return 1;
    }
    uint32_t amplitude = bit_reader_read(reader, bits);
    if (!reader->valid) return 0;
    /* The top amplitude bit is 0 for a negative coefficient.
       Its subtraction from one produces the same category extension
       without a data-dependent branch. All arithmetic masks are unsigned. */
    uint32_t negative_mask = (amplitude >> (bits - 1)) - 1u;
    *value = (int)amplitude -
        (int)(negative_mask & ((1u << bits) - 1u));
    return 1;
}

/* The format-7 effort-3 profile emits fixed rANS models. Keep its common
   symbol path in the coefficient decoder so it can share state and bounds
   checks with the surrounding loop. Adaptive streams use the general API. */
static inline int decode_rans_payload_symbol(
    N148RansDecoder *decoder, const N148RansModel *model, uint8_t *symbol) {
    if (decoder && decoder->adaptive_models)
        return n148_rans_decode_symbol_with_model(decoder, model, symbol);
    if (!decoder || !model || !symbol || !decoder->valid ||
        model->used_symbols == 0) return 0;
    uint32_t slot = decoder->state & (N148_RANS_TOTAL - 1u);
    uint8_t decoded = model->symbol_for_slot[slot];
    uint32_t pair = model->decode_pair[decoded];
    uint32_t frequency = pair & 0xffffu;
    uint32_t cumulative = pair >> 16;
    /* parse_rans_models/read_rich_models prepare and validate every slot:
       its symbol always has a positive frequency and contains this slot. */
    decoder->state = frequency *
        (decoder->state >> N148_RANS_SCALE_BITS) + slot - cumulative;
    while (decoder->state < N148_RANS_LOWER_BOUND) {
        if (decoder->next == decoder->end) {
            decoder->valid = 0;
            return 0;
        }
        decoder->state = (decoder->state << 8) | *decoder->next++;
    }
    *symbol = decoded;
    return 1;
}

/* When the directional coefficient syntax uses fixed models, keep the rANS
   state local so the compiler does not have to reload it around amplitude-
   reader accesses. Model metadata was validated before this path is selected,
   so the per-symbol adaptive/model checks are unnecessary here. */
typedef struct {
    uint32_t state;
    const uint8_t *next;
    const uint8_t *end;
    uint32_t symbols_remaining;
} FixedRansDecoder;

static inline int decode_context_band(int position) {
    return (position >= 6) + (position >= 15) + (position >= 28);
}

/* Coefficient units are at most 64 entries. Their AC band is selected once
   per symbol; a compact table replaces three comparisons in the fixed path. */
#if CONTEXT_DC_MODELS != 6 || CONTEXT_ACTIVITY_BUCKETS != 2
#error fixed_ac_model_base must match the context model layout
#endif
static const uint8_t fixed_ac_model_base[64] = {
    6, 6, 6, 6, 6, 6, 8, 8, 8, 8, 8, 8, 8, 8, 8, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
};

static void prepare_fixed_ac_models(
    const N148RansModel *models, int plane_class,
    const N148RansModel *ac_models[64][2]) {
    for (int position = 1; position < 64; position++) {
        int base = fixed_ac_model_base[position] +
            plane_class * CONTEXT_AC_MODELS / 2;
        ac_models[position][0] = &models[base];
        ac_models[position][1] = &models[base + 1];
    }
}

static inline int decode_fixed_rans_payload_symbol(
    FixedRansDecoder *decoder, const N148RansModel *model, uint8_t *symbol) {
    if (decoder->symbols_remaining == 0) return 0;
    uint32_t slot = decoder->state & (N148_RANS_TOTAL - 1u);
    uint8_t decoded = model->symbol_for_slot[slot];
    uint32_t pair = model->decode_pair[decoded];
    uint32_t frequency = pair & 0xffffu;
    uint32_t cumulative = pair >> 16;
    decoder->state = frequency *
        (decoder->state >> N148_RANS_SCALE_BITS) + slot - cumulative;
    /* A normalized state is at least 2^23. With a 12-bit slot and positive
       frequency, the updated state is at least 2^11, so at most two input
       bytes can be needed for renormalization. */
    if (decoder->state < N148_RANS_LOWER_BOUND) {
        if (decoder->next == decoder->end) return 0;
        decoder->state = (decoder->state << 8) | *decoder->next++;
        if (decoder->state < N148_RANS_LOWER_BOUND) {
            if (decoder->next == decoder->end) return 0;
            decoder->state = (decoder->state << 8) | *decoder->next++;
        }
    }
    decoder->symbols_remaining--;
    *symbol = decoded;
    return 1;
}

static void fixed_rans_begin(const N148RansDecoder *source,
                             uint32_t symbols_remaining,
                             FixedRansDecoder *fixed) {
    fixed->state = source->state;
    fixed->next = source->next;
    fixed->end = source->end;
    fixed->symbols_remaining = symbols_remaining;
}

static void fixed_rans_commit(const FixedRansDecoder *fixed,
                              N148RansDecoder *destination) {
    destination->state = fixed->state;
    destination->next = fixed->next;
}

static int parse_rans_models(const uint8_t *metadata, size_t metadata_size,
                             N148RansModel models[MAX_MODEL_COUNT],
                             int expected_model_count,
                             size_t *bytes_consumed) {
    ByteReader reader;
    reader_init(&reader, metadata, metadata_size);
    if (reader_u8(&reader) != ENTROPY_METADATA_REVISION_BASE ||
        reader_u8(&reader) != ENTROPY_CODER_RANS ||
        reader_u8(&reader) != expected_model_count ||
        reader_u8(&reader) != N148_RANS_SCALE_BITS) return 0;
    for (int model = 0; model < expected_model_count; model++)
        if (!read_model(&reader, &models[model])) return 0;
    if (!reader.valid || !bytes_consumed) return 0;
    *bytes_consumed = reader.position;
    return 1;
}

static int allocate_coefficients(int width, int height,
                                 N148CoeffPlane *coefficients) {
    size_t blocks_x = (size_t)(width / 8 + (width % 8 != 0));
    size_t blocks_y = (size_t)(height / 8 + (height % 8 != 0));
    if (blocks_y != 0 && blocks_x > SIZE_MAX / blocks_y) return 0;
    size_t count = blocks_x * blocks_y;
    if (count == 0 || count > LONG_MAX ||
        count > SIZE_MAX / (64 * sizeof(short)) ||
        count > SIZE_MAX / sizeof(*coefficients->nonzero_masks))
        return 0;
    size_t coefficient_bytes = count * 64 * sizeof(short);
    size_t mask_bytes = count * sizeof(*coefficients->nonzero_masks);
    if (coefficient_bytes > SIZE_MAX - mask_bytes) return 0;
    coefficients->coefficients = (short *) calloc(
        1, coefficient_bytes + mask_bytes);
    if (!coefficients->coefficients) return 0;
    coefficients->nonzero_masks = (unsigned long long *)
        ((uint8_t *) coefficients->coefficients + coefficient_bytes);
    coefficients->count = (long) count;
    return 1;
}

static int decode_rans_plane(N148RansDecoder *decoder, BitReader *bits,
                             const N148RansModel *models,
                             int blocks_x, int plane_class, int use_context,
                             int absolute_dc,
                             uint32_t symbol_count, uint32_t *decoded_symbols,
                             N148CoeffPlane *coefficients,
                             N148ContextTrace *trace) {
    int previous_dc = 0;
    int previous_dc_category = 0;
    int base_dc_model = plane_class ? BASE_DC_CHROMA_MODEL : BASE_DC_LUMA_MODEL;
    int base_ac_model = plane_class ? BASE_AC_CHROMA_MODEL : BASE_AC_LUMA_MODEL;
    for (long block = 0; block < coefficients->count; block++) {
        short *values = coefficients->coefficients + block * 64;
        uint8_t category;
        int difference;
        int model = use_context ?
            context_dc_model(plane_class, previous_dc_category) :
            base_dc_model;
        if (*decoded_symbols >= symbol_count ||
            !context_trace_add(trace, model) ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &category) ||
            category > 16 ||
            !decode_amplitude(bits, category, &difference)) return 0;
        (*decoded_symbols)++;
        int dc = absolute_dc ? difference : previous_dc + difference;
        if (dc < SHRT_MIN || dc > SHRT_MAX) return 0;
        values[0] = (short) dc;
        if (dc != 0) coefficients->nonzero_masks[block] |= 1ull;
        previous_dc = dc;
        previous_dc_category = category;

        int position = 1;
        int previous_ac_category = 0;
        while (position < 64) {
            uint8_t symbol;
            model = use_context ? context_ac_model(
                coefficients, block, blocks_x, plane_class,
                position, previous_ac_category) : base_ac_model;
            if (*decoded_symbols >= symbol_count ||
                !context_trace_add(trace, model) ||
                !decode_rans_payload_symbol(
                    decoder, &models[model], &symbol)) return 0;
            (*decoded_symbols)++;
            if (symbol == 0) break;
            if (symbol == 0xf0u) {
                position += 16;
                if (position > 64) return 0;
                previous_ac_category = 0;
                continue;
            }
            int run = symbol >> 4;
            int coefficient_bits = symbol & 15;
            if (coefficient_bits == 0) return 0;
            position += run;
            if (position >= 64) return 0;
            int value;
            if (!decode_amplitude(bits, coefficient_bits, &value) ||
                value < SHRT_MIN || value > SHRT_MAX) return 0;
            values[position] = (short) value;
            coefficients->nonzero_masks[block] |= 1ull << position;
            position++;
            previous_ac_category = coefficient_bits;
        }
    }
    return 1;
}

static int decode_directional_coefficient_unit(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, int plane_class,
    int use_context, uint32_t symbol_count, uint32_t *decoded_symbols,
    N148CoeffPlane *coefficients, long block, const short *left_values,
    const short *top_values, int packed_offset, int length,
    int *previous_dc_category) {
    short *values = coefficients->coefficients + block * 64 + packed_offset;
    int base_dc_model = plane_class ? BASE_DC_CHROMA_MODEL : BASE_DC_LUMA_MODEL;
    int base_ac_model = plane_class ? BASE_AC_CHROMA_MODEL : BASE_AC_LUMA_MODEL;
    int model = use_context ?
        context_dc_model(plane_class, *previous_dc_category) : base_dc_model;
    uint8_t category;
    int value;
    if (*decoded_symbols >= symbol_count ||
        !decode_rans_payload_symbol(
            decoder, &models[model], &category) ||
        category > 16 || !decode_amplitude(bits, category, &value) ||
        value < SHRT_MIN || value > SHRT_MAX) return 0;
    values[0] = (short) value;
    if (value != 0)
        coefficients->nonzero_masks[block] |= 1ull << packed_offset;
    *previous_dc_category = category;
    (*decoded_symbols)++;

    int position = 1;
    int previous_ac_category = 0;
    while (position < length) {
        if (use_context) {
            int packed_position = packed_offset + position;
            int active = previous_ac_category >= 3 ||
                (left_values &&
                 (left_values[packed_position] <= -4 ||
                  left_values[packed_position] >= 4)) ||
                (top_values &&
                 (top_values[packed_position] <= -4 ||
                  top_values[packed_position] >= 4));
            model = CONTEXT_DC_MODELS +
                ((plane_class * CONTEXT_BANDS + decode_context_band(position)) *
                 CONTEXT_ACTIVITY_BUCKETS) + active;
        } else {
            model = base_ac_model;
        }
        uint8_t symbol;
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &symbol)) return 0;
        (*decoded_symbols)++;
        if (symbol == 0) break;
        if (symbol == 0xf0u) {
            position += 16;
            if (position > length) return 0;
            previous_ac_category = 0;
            continue;
        }
        int run = symbol >> 4;
        int coefficient_bits = symbol & 15;
        if (coefficient_bits == 0) return 0;
        position += run;
        if (position >= length ||
            !decode_amplitude(bits, coefficient_bits, &value) ||
            value < SHRT_MIN || value > SHRT_MAX) return 0;
        values[position] = (short) value;
        coefficients->nonzero_masks[block] |=
            1ull << (packed_offset + position);
        position++;
        previous_ac_category = coefficient_bits;
    }
    return 1;
}

static int decode_fixed_directional_coefficient_unit(
    FixedRansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, int plane_class, int use_context,
    const N148RansModel *ac_models[64][2],
    N148CoeffPlane *coefficients, long block, uint64_t left_activity,
    uint64_t top_activity, uint64_t *block_activity,
    int packed_offset, int length,
    int *previous_dc_category) {
    short *values = coefficients->coefficients + block * 64 + packed_offset;
    int base_dc_model = plane_class ? BASE_DC_CHROMA_MODEL : BASE_DC_LUMA_MODEL;
    int base_ac_model = plane_class ? BASE_AC_CHROMA_MODEL : BASE_AC_LUMA_MODEL;
    int model = use_context ?
        context_dc_model(plane_class, *previous_dc_category) : base_dc_model;
    uint8_t category;
    int value;
    if (!decode_fixed_rans_payload_symbol(
            decoder, &models[model], &category) ||
        category > 16 || !decode_amplitude(bits, category, &value) ||
        value < SHRT_MIN || value > SHRT_MAX) return 0;
    values[0] = (short) value;
    if (value != 0)
        coefficients->nonzero_masks[block] |= 1ull << packed_offset;
    if (category >= 3)
        *block_activity |= 1ull << packed_offset;
    *previous_dc_category = category;

    int position = 1;
    int previous_ac_category = 0;
    while (position < length) {
        const N148RansModel *ac_model;
        if (use_context) {
            int packed_position = packed_offset + position;
            int active = previous_ac_category >= 3 ||
                (((left_activity | top_activity) >> packed_position) & 1u);
            ac_model = ac_models[position][active];
        } else {
            ac_model = &models[base_ac_model];
        }
        uint8_t symbol;
        if (!decode_fixed_rans_payload_symbol(
                decoder, ac_model, &symbol)) return 0;
        if (symbol == 0) break;
        if (symbol == 0xf0u) {
            position += 16;
            if (position > length) return 0;
            previous_ac_category = 0;
            continue;
        }
        int run = symbol >> 4;
        int coefficient_bits = symbol & 15;
        if (coefficient_bits == 0) return 0;
        position += run;
        if (position >= length ||
            !decode_amplitude(bits, coefficient_bits, &value) ||
            value < SHRT_MIN || value > SHRT_MAX) return 0;
        values[position] = (short) value;
        coefficients->nonzero_masks[block] |=
            1ull << (packed_offset + position);
        if (coefficient_bits >= 3)
            *block_activity |= 1ull << (packed_offset + position);
        position++;
        previous_ac_category = coefficient_bits;
    }
    return 1;
}

static int decode_directional_coefficient_plane(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, const uint8_t *splits, int blocks_x,
    int plane_class, int use_context, uint32_t symbol_count,
    uint32_t *decoded_symbols, N148CoeffPlane *coefficients,
    FixedRansDecoder *fixed) {
    if (!decoder || !bits || !models || !splits || blocks_x <= 0 ||
        !decoded_symbols || !coefficients || !coefficients->coefficients)
        return 0;
    /* A coefficient has magnitude >= 4 exactly when its amplitude category
       is at least three. Cache that fact for the two causal neighbors so the
       fixed-model decoder does not reload and compare both coefficients at
       every context decision. */
    uint64_t *activity_masks = NULL;
    const N148RansModel *fixed_ac_models[64][2];
    if (fixed && use_context) {
        activity_masks = (uint64_t *) malloc(
            (size_t) coefficients->count * sizeof(*activity_masks));
        if (!activity_masks) return 0;
        prepare_fixed_ac_models(models, plane_class, fixed_ac_models);
    }
    int previous_dc_category = 0;
    int block_x = 0;
    for (long block = 0; block < coefficients->count; block++) {
        const short *left_values = block_x ?
            coefficients->coefficients + (block - 1) * 64 : NULL;
        const short *top_values = block >= blocks_x ?
            coefficients->coefficients + (block - blocks_x) * 64 : NULL;
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int length = splits[block] ? 16 : 64;
        uint64_t block_activity = 0;
        uint64_t left_activity = activity_masks && block_x ?
            activity_masks[block - 1] : 0;
        uint64_t top_activity = activity_masks && block >= blocks_x ?
            activity_masks[block - blocks_x] : 0;
        for (int part = 0; part < parts; part++) {
            int success = fixed ?
                decode_fixed_directional_coefficient_unit(
                    fixed, bits, models, plane_class, use_context,
                    fixed_ac_models,
                    coefficients, block, left_activity, top_activity,
                    &block_activity,
                    part * 16, length,
                    &previous_dc_category) :
                decode_directional_coefficient_unit(
                    decoder, bits, models, plane_class, use_context,
                    symbol_count, decoded_symbols, coefficients, block,
                    left_values, top_values, part * 16, length,
                    &previous_dc_category);
            if (!success) {
                free(activity_masks);
                return 0;
            }
        }
        if (activity_masks) activity_masks[block] = block_activity;
        if (++block_x == blocks_x) block_x = 0;
    }
    free(activity_masks);
    return 1;
}

static int decode_large_transform_coefficient_unit(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, int use_context,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    int *previous_dc_category, short *values,
    unsigned long long masks[16], int length) {
    if (!values || (length != 16 * 16 && length != 32 * 32)) return 0;
    int model = use_context ?
        context_dc_model(0, *previous_dc_category) : BASE_DC_LUMA_MODEL;
    uint8_t category;
    int value;
    if (*decoded_symbols >= symbol_count ||
        !decode_rans_payload_symbol(
            decoder, &models[model], &category) ||
        category > 16 || !decode_amplitude(bits, category, &value) ||
        value < SHRT_MIN || value > SHRT_MAX) return 0;
    values[0] = (short) value;
    if (value != 0) masks[0] |= 1ull;
    *previous_dc_category = category;
    (*decoded_symbols)++;
    int position = 1;
    int previous_ac_category = 0;
    while (position < length) {
        model = use_context ? CONTEXT_DC_MODELS +
            decode_context_band(position) * CONTEXT_ACTIVITY_BUCKETS +
            (previous_ac_category >= 3) : BASE_AC_LUMA_MODEL;
        uint8_t symbol;
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &symbol)) return 0;
        (*decoded_symbols)++;
        if (symbol == 0) break;
        if (symbol == 0xf0u) {
            position += 16;
            if (position > length) return 0;
            previous_ac_category = 0;
            continue;
        }
        int run = symbol >> 4;
        int coefficient_bits = symbol & 15;
        if (coefficient_bits == 0) return 0;
        position += run;
        if (position >= length ||
            !decode_amplitude(bits, coefficient_bits, &value) ||
            value < SHRT_MIN || value > SHRT_MAX) return 0;
        values[position] = (short) value;
        masks[position >> 6] |= 1ull << (position & 63);
        position++;
        previous_ac_category = coefficient_bits;
    }
    return 1;
}

static int decode_fixed_large_transform_coefficient_unit(
    FixedRansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, int use_context,
    int *previous_dc_category, short *values,
    unsigned long long masks[16], int length) {
    if (!values || (length != 16 * 16 && length != 32 * 32)) return 0;
    int model = use_context ?
        context_dc_model(0, *previous_dc_category) : BASE_DC_LUMA_MODEL;
    uint8_t category;
    int value;
    if (!decode_fixed_rans_payload_symbol(
            decoder, &models[model], &category) ||
        category > 16 || !decode_amplitude(bits, category, &value) ||
        value < SHRT_MIN || value > SHRT_MAX) return 0;
    values[0] = (short) value;
    if (value != 0) masks[0] |= 1ull;
    *previous_dc_category = category;
    int position = 1;
    int previous_ac_category = 0;
    while (position < length) {
        model = use_context ? CONTEXT_DC_MODELS +
            decode_context_band(position) * CONTEXT_ACTIVITY_BUCKETS +
            (previous_ac_category >= 3) : BASE_AC_LUMA_MODEL;
        uint8_t symbol;
        if (!decode_fixed_rans_payload_symbol(
                decoder, &models[model], &symbol)) return 0;
        if (symbol == 0) break;
        if (symbol == 0xf0u) {
            position += 16;
            if (position > length) return 0;
            previous_ac_category = 0;
            continue;
        }
        int run = symbol >> 4;
        int coefficient_bits = symbol & 15;
        if (coefficient_bits == 0) return 0;
        position += run;
        if (position >= length ||
            !decode_amplitude(bits, coefficient_bits, &value) ||
            value < SHRT_MIN || value > SHRT_MAX) return 0;
        values[position] = (short) value;
        masks[position >> 6] |= 1ull << (position & 63);
        position++;
        previous_ac_category = coefficient_bits;
    }
    return 1;
}

static uint64_t coefficient_activity_mask(const short values[64],
                                          uint64_t nonzero_mask) {
    uint64_t activity = 0;
    while (nonzero_mask) {
        int position = 0;
#if defined(__GNUC__) || defined(__clang__)
        position = __builtin_ctzll(nonzero_mask);
#else
        uint64_t probe = nonzero_mask;
        while (!(probe & 1u)) { position++; probe >>= 1; }
#endif
        int value = values[position];
        if (value <= -4 || value >= 4)
            activity |= 1ull << position;
        nonzero_mask &= nonzero_mask - 1;
    }
    return activity;
}

static int decode_variable_luma_coefficient_plane(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, const uint8_t *splits,
    int blocks_x, int blocks_y, int use_context, uint32_t symbol_count,
    uint32_t *decoded_symbols, const N148TransformMap *transform_map,
    N148CoeffPlane *coefficients, FixedRansDecoder *fixed, int dct32) {
    if (!decoder || !bits || !models || !splits || blocks_x <= 0 ||
        blocks_y <= 0 || !decoded_symbols || !coefficients ||
        !coefficients->coefficients ||
        coefficients->count != (long) blocks_x * blocks_y ||
        !n148_luma_transform_map_validate_extended(transform_map, dct32))
        return 0;
    uint64_t *activity_masks = NULL;
    const N148RansModel *fixed_ac_models[64][2];
    if (fixed && use_context) {
        activity_masks = (uint64_t *) malloc(
            (size_t) coefficients->count * sizeof(*activity_masks));
        if (!activity_masks) return 0;
        prepare_fixed_ac_models(models, 0, fixed_ac_models);
    }
    int previous_dc_category = 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            if (dct32 && n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                short sequence[32 * 32] = {0};
                unsigned long long masks[16] = {0};
                int success = fixed ?
                    decode_fixed_large_transform_coefficient_unit(
                        fixed, bits, models, use_context,
                        &previous_dc_category, sequence, masks,
                        32 * 32) :
                    decode_large_transform_coefficient_unit(
                        decoder, bits, models, use_context, symbol_count,
                        decoded_symbols, &previous_dc_category, sequence, masks,
                        32 * 32);
                if (!success) goto failure;
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        goto failure;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) goto failure;
                    memcpy(coefficients->coefficients + block * 64,
                           sequence + cell * 64,
                           64 * sizeof(*sequence));
                    coefficients->nonzero_masks[block] = masks[cell];
                    if (activity_masks)
                        activity_masks[block] = coefficient_activity_mask(
                            sequence + cell * 64, masks[cell]);
                }
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                short sequence[16 * 16] = {0};
                unsigned long long masks[16] = {0};
                int success = fixed ?
                    decode_fixed_large_transform_coefficient_unit(
                        fixed, bits, models, use_context,
                        &previous_dc_category, sequence, masks,
                        16 * 16) :
                    decode_large_transform_coefficient_unit(
                        decoder, bits, models, use_context, symbol_count,
                        decoded_symbols, &previous_dc_category, sequence, masks,
                        16 * 16);
                if (!success) goto failure;
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_x + (cell & 1);
                    int block_y = first_y + (cell >> 1);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        goto failure;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) goto failure;
                    memcpy(coefficients->coefficients + block * 64,
                           sequence + cell * 64,
                           64 * sizeof(*sequence));
                    coefficients->nonzero_masks[block] = masks[cell];
                    if (activity_masks)
                        activity_masks[block] = coefficient_activity_mask(
                            sequence + cell * 64, masks[cell]);
                }
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    int length = splits[block] ? 16 : 64;
                    const short *left_values = block_x ?
                        coefficients->coefficients + (block - 1) * 64 : NULL;
                    const short *top_values = block_y ?
                        coefficients->coefficients +
                            (block - blocks_x) * 64 : NULL;
                    uint64_t block_activity = 0;
                    uint64_t left_activity = activity_masks && block_x ?
                        activity_masks[block - 1] : 0;
                    uint64_t top_activity = activity_masks && block_y ?
                        activity_masks[block - blocks_x] : 0;
                    for (int part = 0; part < parts; part++) {
                        int success = fixed ?
                            decode_fixed_directional_coefficient_unit(
                                fixed, bits, models, 0, use_context,
                                fixed_ac_models,
                                coefficients, block, left_activity,
                                top_activity, &block_activity,
                                part * 16,
                                length, &previous_dc_category) :
                            decode_directional_coefficient_unit(
                                decoder, bits, models, 0, use_context,
                                symbol_count, decoded_symbols, coefficients,
                                block, left_values, top_values, part * 16,
                                length, &previous_dc_category);
                        if (!success) goto failure;
                    }
                    if (activity_masks)
                        activity_masks[block] = block_activity;
                }
            }
        }
    }
    free(activity_masks);
    return 1;
failure:
    free(activity_masks);
    return 0;
}

static int decode_rich_coefficient_unit(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, int blocks_x, int plane_class,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    N148CoeffPlane *coefficients, long block, int packed_offset, int length,
    int size_class, int prediction_mode, int *previous_dc_category) {
    short *values = coefficients->coefficients + block * 64 + packed_offset;
    const short *left = block % blocks_x ?
        coefficients->coefficients + (block - 1) * 64 + packed_offset : NULL;
    const short *above = block >= blocks_x ?
        coefficients->coefficients + (block - blocks_x) * 64 +
            packed_offset : NULL;
    int luma_ac_base = RICH_LUMA_DC_MODELS +
        (size_class * RICH_PREDICTION_CLASSES +
         rich_prediction_class(prediction_mode)) *
            RICH_POSITION_BUCKETS * RICH_ACTIVITY_BUCKETS;
    int chroma_ac_base = RICH_LUMA_DC_MODELS + RICH_LUMA_AC_MODELS +
        RICH_CHROMA_DC_MODELS;
    int model = plane_class ?
        rich_chroma_dc_model(*previous_dc_category) :
        rich_luma_dc_model(
            size_class, prediction_mode, *previous_dc_category);
    uint8_t category;
    int value;
    if (*decoded_symbols >= symbol_count ||
        !decode_rans_payload_symbol(
            decoder, &models[model], &category) ||
        category > 16 || !decode_amplitude(bits, category, &value) ||
        value < SHRT_MIN || value > SHRT_MAX) return 0;
    values[0] = (short) value;
    if (value != 0)
        coefficients->nonzero_masks[block] |= 1ull << packed_offset;
    *previous_dc_category = category;
    (*decoded_symbols)++;

    int position = 1;
    int previous_ac_category = 0;
    int previous_magnitude = 0;
    while (position < length) {
        if (plane_class) {
            int active = previous_ac_category >= 3;
            if (!active && left) {
                int neighbor = left[position];
                active = neighbor <= -4 || neighbor >= 4;
            }
            if (!active && above) {
                int neighbor = above[position];
                active = neighbor <= -4 || neighbor >= 4;
            }
            model = chroma_ac_base +
                decode_context_band(position) * CONTEXT_ACTIVITY_BUCKETS + active;
        } else {
            int magnitude_sum = previous_magnitude;
            if (left) {
                int neighbor = left[position];
                magnitude_sum += neighbor < 0 ? -neighbor : neighbor;
            }
            if (above) {
                int neighbor = above[position];
                magnitude_sum += neighbor < 0 ? -neighbor : neighbor;
            }
            int activity = magnitude_sum == 0 ? 0 :
                (magnitude_sum <= 3 ? 1 : 2);
            model = luma_ac_base +
                rich_position_bucket(position) * RICH_ACTIVITY_BUCKETS +
                activity;
        }
        uint8_t symbol;
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &symbol)) return 0;
        (*decoded_symbols)++;
        if (symbol == 0) break;
        if (symbol == 0xf0u) {
            position += 16;
            if (position > length) return 0;
            previous_ac_category = 0;
            previous_magnitude = 0;
            continue;
        }
        int run = symbol >> 4;
        int coefficient_bits = symbol & 15;
        if (coefficient_bits == 0) return 0;
        position += run;
        if (position >= length ||
            !decode_amplitude(bits, coefficient_bits, &value) ||
            value < SHRT_MIN || value > SHRT_MAX) return 0;
        values[position] = (short) value;
        coefficients->nonzero_masks[block] |=
            1ull << (packed_offset + position);
        position++;
        previous_ac_category = coefficient_bits;
        previous_magnitude = value < 0 ? -value : value;
    }
    return 1;
}

static int decode_rich_coefficient_plane(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, const uint8_t *splits,
    const uint8_t *modes, int blocks_x, int plane_class,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    N148CoeffPlane *coefficients) {
    if (!decoder || !bits || !models || !splits || !modes ||
        blocks_x <= 0 || !decoded_symbols || !coefficients ||
        !coefficients->coefficients) return 0;
    int previous_dc_category = 0;
    for (long block = 0; block < coefficients->count; block++) {
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int length = splits[block] ? 16 : 64;
        int size_class = splits[block] ? 0 : 1;
        for (int part = 0; part < parts; part++) {
            int mode = modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
            if (!decode_rich_coefficient_unit(
                    decoder, bits, models, blocks_x, plane_class,
                    symbol_count, decoded_symbols, coefficients, block,
                    part * 16, length, size_class, mode,
                    &previous_dc_category)) return 0;
        }
    }
    return 1;
}

static int decode_rich_large_coefficient_unit(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, uint32_t symbol_count,
    uint32_t *decoded_symbols, int size_class, int prediction_mode,
    int *previous_dc_category, short *values,
    unsigned long long masks[16], int length) {
    if (!values || (length != 16 * 16 && length != 32 * 32)) return 0;
    int model = rich_luma_dc_model(
        size_class, prediction_mode, *previous_dc_category);
    uint8_t category;
    int value;
    if (*decoded_symbols >= symbol_count ||
        !decode_rans_payload_symbol(
            decoder, &models[model], &category) ||
        category > 16 || !decode_amplitude(bits, category, &value) ||
        value < SHRT_MIN || value > SHRT_MAX) return 0;
    values[0] = (short) value;
    if (value != 0) masks[0] |= 1ull;
    *previous_dc_category = category;
    (*decoded_symbols)++;
    int position = 1;
    int previous_magnitude = 0;
    while (position < length) {
        int activity = previous_magnitude == 0 ? 0 :
            (previous_magnitude <= 3 ? 1 : 2);
        model = rich_luma_ac_model(
            size_class, prediction_mode, position, activity);
        uint8_t symbol;
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &symbol)) return 0;
        (*decoded_symbols)++;
        if (symbol == 0) break;
        if (symbol == 0xf0u) {
            position += 16;
            if (position > length) return 0;
            previous_magnitude = 0;
            continue;
        }
        int run = symbol >> 4;
        int coefficient_bits = symbol & 15;
        if (coefficient_bits == 0) return 0;
        position += run;
        if (position >= length ||
            !decode_amplitude(bits, coefficient_bits, &value) ||
            value < SHRT_MIN || value > SHRT_MAX) return 0;
        values[position] = (short) value;
        masks[position >> 6] |= 1ull << (position & 63);
        position++;
        previous_magnitude = value < 0 ? -value : value;
    }
    return 1;
}

static int decode_rich_luma_coefficient_plane(
    N148RansDecoder *decoder, BitReader *bits,
    const N148RansModel *models, const uint8_t *splits,
    const uint8_t *modes, int blocks_x, int blocks_y,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    const N148TransformMap *transform_map,
    N148CoeffPlane *coefficients) {
    if (!decoder || !bits || !models || !splits || !modes ||
        blocks_x <= 0 || blocks_y <= 0 || !decoded_symbols ||
        !coefficients || !coefficients->coefficients ||
        coefficients->count != (long) blocks_x * blocks_y ||
        !n148_luma_transform_map_validate_extended(transform_map, 1))
        return 0;
    int previous_dc_category = 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            long first_block = (long) first_y * blocks_x + first_x;
            int mode = modes[first_block * N148_INTRA_MODE_SLOTS_PER_BLOCK];
            if (n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                short sequence[32 * 32] = {0};
                unsigned long long masks[16] = {0};
                if (!decode_rich_large_coefficient_unit(
                        decoder, bits, models, symbol_count, decoded_symbols,
                        2, mode, &previous_dc_category, sequence, masks,
                        32 * 32))
                    return 0;
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(coefficients->coefficients + block * 64,
                           sequence + cell * 64,
                           64 * sizeof(*sequence));
                    coefficients->nonzero_masks[block] = masks[cell];
                }
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                short sequence[16 * 16] = {0};
                unsigned long long masks[16] = {0};
                if (!decode_rich_large_coefficient_unit(
                        decoder, bits, models, symbol_count, decoded_symbols,
                        2, mode, &previous_dc_category, sequence, masks,
                        16 * 16))
                    return 0;
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_x + (cell & 1);
                    int block_y = first_y + (cell >> 1);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    memcpy(coefficients->coefficients + block * 64,
                           sequence + cell * 64,
                           64 * sizeof(*sequence));
                    coefficients->nonzero_masks[block] = masks[cell];
                }
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    int length = splits[block] ? 16 : 64;
                    int size_class = splits[block] ? 0 : 1;
                    for (int part = 0; part < parts; part++) {
                        mode = modes[block *
                            N148_INTRA_MODE_SLOTS_PER_BLOCK + part];
                        if (!decode_rich_coefficient_unit(
                                decoder, bits, models, blocks_x, 0,
                                symbol_count, decoded_symbols, coefficients,
                                block, part * 16, length, size_class, mode,
                                &previous_dc_category)) return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static int decode_prediction_plane(N148RansDecoder *decoder,
                                   const N148RansModel *models,
                                   uint8_t *modes, long count,
                                   int blocks_x, int plane_class,
                                   int coefficient_model_count,
                                   uint32_t symbol_count,
                                   uint32_t *decoded_symbols) {
    if (!decoder || !models || !modes || count <= 0 || blocks_x <= 0 ||
        !decoded_symbols) return 0;
    for (long block = 0; block < count; block++) {
        int model = prediction_mode_model(
            coefficient_model_count, plane_class, modes, block, blocks_x);
        uint8_t mode;
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &mode) ||
            mode >= N148_INTRA_MODE_COUNT) return 0;
        modes[block] = mode;
        (*decoded_symbols)++;
    }
    return 1;
}

static int decode_directional_prediction_plane(
    N148RansDecoder *decoder, const N148RansModel *models, uint8_t *modes,
    long block_count, int blocks_x, int plane_class,
    int coefficient_model_count, int allowed_modes, const uint8_t *splits,
    uint32_t symbol_count, uint32_t *decoded_symbols) {
    if (!decoder || !models || !modes || block_count <= 0 || blocks_x <= 0 ||
        allowed_modes < 1 ||
        allowed_modes > N148_PACKED_INTRA_MODE_COUNT ||
        !splits || !decoded_symbols) return 0;
    for (long block = 0; block < block_count; block++) {
        int parts = splits[block] ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
        int mode_limit = allowed_modes;
        for (int part = 0; part < parts; part++) {
            int model = directional_prediction_mode_model(
                coefficient_model_count, plane_class, modes, block, part,
                blocks_x);
            uint8_t symbol;
            if (*decoded_symbols >= symbol_count ||
                !decode_rans_payload_symbol(
                    decoder, &models[model], &symbol) ||
                symbol >= mode_limit) return 0;
            int mode = directional_mode_from_signal(
                modes, block, part, blocks_x, symbol, allowed_modes);
            if (mode < 0 || mode >= mode_limit ||
                (splits[block] &&
                 n148_intra_reference_index(mode) != 0)) return 0;
            modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part] =
                (uint8_t) mode;
            (*decoded_symbols)++;
        }
        if (!splits[block]) {
            uint8_t mode = modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK];
            for (int part = 1; part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part] = mode;
        }
    }
    return 1;
}

static int decode_variable_luma_prediction_plane(
    N148RansDecoder *decoder, const N148RansModel *models, uint8_t *modes,
    long block_count, int blocks_x, int blocks_y,
    int coefficient_model_count, int allowed_modes, const uint8_t *splits,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    const N148TransformMap *transform_map, int dct32) {
    if (!decoder || !models || !modes ||
        block_count != (long) blocks_x * blocks_y || blocks_x <= 0 ||
        blocks_y <= 0 || allowed_modes < 1 ||
        allowed_modes > N148_PACKED_INTRA_MODE_COUNT || !splits ||
        !decoded_symbols ||
        !n148_luma_transform_map_validate_extended(transform_map, dct32))
        return 0;
    for (int macro_y = 0; macro_y < transform_map->rows[0]; macro_y++) {
        for (int macro_x = 0; macro_x < transform_map->columns[0];
             macro_x++) {
            size_t transform_index = (size_t) macro_y *
                (size_t) transform_map->columns[0] + (size_t) macro_x;
            int first_x = macro_x * 2;
            int first_y = macro_y * 2;
            if (dct32 && n148_luma_transform_dct32_member(
                    transform_map, macro_x, macro_y)) {
                if (!n148_luma_transform_dct32_origin(
                        transform_map, macro_x, macro_y)) continue;
                long first_block = (long) first_y * blocks_x + first_x;
                int model = directional_prediction_mode_model(
                    coefficient_model_count, 0, modes, first_block, 0,
                    blocks_x);
                uint8_t symbol;
                if (*decoded_symbols >= symbol_count ||
                    !decode_rans_payload_symbol(
                        decoder, &models[model], &symbol) ||
                    symbol >= allowed_modes) return 0;
                int mode = directional_mode_from_signal(
                    modes, first_block, 0, blocks_x, symbol,
                    allowed_modes);
                if (mode < 0 || mode >= allowed_modes) return 0;
                (*decoded_symbols)++;
                for (int cell = 0; cell < 16; cell++) {
                    int block_x = first_x + (cell & 3);
                    int block_y = first_y + (cell >> 2);
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    for (int part = 0;
                         part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                        modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part] =
                            (uint8_t) mode;
                }
                continue;
            }
            if (transform_map->strategies[transform_index] ==
                N148_TRANSFORM_DCT16) {
                long first_block = (long) first_y * blocks_x + first_x;
                int model = directional_prediction_mode_model(
                    coefficient_model_count, 0, modes, first_block, 0,
                    blocks_x);
                uint8_t symbol;
                if (*decoded_symbols >= symbol_count ||
                    !decode_rans_payload_symbol(
                        decoder, &models[model], &symbol) ||
                    symbol >= allowed_modes) return 0;
                int mode = directional_mode_from_signal(
                    modes, first_block, 0, blocks_x, symbol,
                    allowed_modes);
                if (mode < 0 || mode >= allowed_modes) return 0;
                (*decoded_symbols)++;
                for (int cell = 0; cell < 4; cell++) {
                    int block_x = first_x + (cell & 1);
                    int block_y = first_y + (cell >> 1);
                    if (block_x >= blocks_x || block_y >= blocks_y)
                        return 0;
                    long block = (long) block_y * blocks_x + block_x;
                    if (splits[block]) return 0;
                    for (int part = 0;
                         part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                        modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part] =
                            (uint8_t) mode;
                }
                continue;
            }
            for (int cell_y = 0; cell_y < 2; cell_y++) {
                int block_y = first_y + cell_y;
                if (block_y >= blocks_y) continue;
                for (int cell_x = 0; cell_x < 2; cell_x++) {
                    int block_x = first_x + cell_x;
                    if (block_x >= blocks_x) continue;
                    long block = (long) block_y * blocks_x + block_x;
                    int parts = splits[block] ?
                        N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                    for (int part = 0; part < parts; part++) {
                        int model = directional_prediction_mode_model(
                            coefficient_model_count, 0, modes, block, part,
                            blocks_x);
                        uint8_t symbol;
                        if (*decoded_symbols >= symbol_count ||
                            !decode_rans_payload_symbol(
                                decoder, &models[model], &symbol) ||
                            symbol >= allowed_modes) return 0;
                        int mode = directional_mode_from_signal(
                            modes, block, part, blocks_x, symbol,
                            allowed_modes);
                        if (mode < 0 || mode >= allowed_modes ||
                            (splits[block] &&
                             n148_intra_reference_index(mode) != 0))
                            return 0;
                        modes[block * N148_INTRA_MODE_SLOTS_PER_BLOCK + part] =
                            (uint8_t) mode;
                        (*decoded_symbols)++;
                    }
                    if (!splits[block]) {
                        uint8_t mode = modes[block *
                            N148_INTRA_MODE_SLOTS_PER_BLOCK];
                        for (int part = 1;
                             part < N148_INTRA_MODE_SLOTS_PER_BLOCK; part++)
                            modes[block *
                                N148_INTRA_MODE_SLOTS_PER_BLOCK + part] = mode;
                    }
                }
            }
        }
    }
    return 1;
}

static int decode_partition_map(
    N148RansDecoder *decoder, const N148RansModel *models, int model,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    N148PartitionMap *map) {
    if (!decoder || !models || model < 0 || !decoded_symbols || !map ||
        !map->split || map->count == 0) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t delta;
        uint8_t prediction = n148_partition_map_predict(map, index);
        if (*decoded_symbols >= symbol_count || prediction > 1 ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &delta) || delta > 1) return 0;
        map->split[index] = delta ^ prediction;
        (*decoded_symbols)++;
    }
    return n148_partition_map_validate(map);
}

static int decode_variable_luma_partition_map(
    N148RansDecoder *decoder, const N148RansModel *models, int model,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    const N148TransformMap *transform_map, N148PartitionMap *map,
    int dct32) {
    if (!decoder || !models || model < 0 || !decoded_symbols || !map ||
        !map->split || !transform_map ||
        !n148_luma_transform_map_validate_extended(transform_map, dct32) ||
        map->blocks_x[0] != transform_map->blocks_x[0] ||
        map->blocks_y[0] != transform_map->blocks_y[0]) return 0;
    for (int plane = 0; plane < 3; plane++) {
        int blocks_x = map->blocks_x[plane];
        int blocks_y = map->blocks_y[plane];
        size_t index = map->plane_offsets[plane];
        for (int row = 0; row < blocks_y; row++) {
            for (int column = 0; column < blocks_x; column++, index++) {
                if (plane == 0) {
                    int macro_x = column >> 1;
                    int macro_y = row >> 1;
                    size_t transform_index = (size_t) macro_y *
                        (size_t) transform_map->columns[0] +
                        (size_t) macro_x;
                    if ((dct32 && n148_luma_transform_dct32_member(
                            transform_map, macro_x, macro_y)) ||
                        transform_map->strategies[transform_index] ==
                            N148_TRANSFORM_DCT16) {
                        map->split[index] = 0;
                        continue;
                    }
                }
                uint8_t delta;
                uint8_t prediction = column ? map->split[index - 1] :
                    row ? map->split[index - (size_t) blocks_x] : 0;
                if (*decoded_symbols >= symbol_count || prediction > 1 ||
                    !decode_rans_payload_symbol(
                        decoder, &models[model], &delta) || delta > 1)
                    return 0;
                map->split[index] = delta ^ prediction;
                (*decoded_symbols)++;
            }
        }
    }
    return n148_partition_map_validate(map);
}

static int decode_segmentation_map(
    N148RansDecoder *decoder, const N148RansModel *models, int model,
    int width, int height, uint32_t symbol_count,
    uint32_t *decoded_symbols, N148SegmentationMap *map) {
    if (!decoder || !models || model < 0 || !decoded_symbols || !map ||
        !n148_segmentation_map_allocate(width, height, map)) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t delta;
        uint8_t prediction = n148_segmentation_map_predict(map, index);
        if (*decoded_symbols >= symbol_count ||
            prediction >= N148_SEGMENT_COUNT ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &delta) ||
            delta >= N148_SEGMENT_COUNT) return 0;
        map->levels[index] = (uint8_t)(
            (prediction + delta) & (N148_SEGMENT_COUNT - 1));
        (*decoded_symbols)++;
    }
    return n148_segmentation_map_validate(map);
}

static int decode_adaptive_map(N148RansDecoder *decoder,
                               const N148RansModel *models, int model,
                               int width, int height, uint32_t symbol_count,
                               uint32_t *decoded_symbols,
                               N148AdaptiveMap *map) {
    if (!decoder || !models || !decoded_symbols || !map || model < 0 ||
        !n148_adaptive_map_allocate(width, height, map)) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t delta;
        uint8_t prediction = n148_adaptive_map_predict(map, index);
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &delta) ||
            delta >= N148_AQ_LEVEL_COUNT ||
            prediction >= N148_AQ_LEVEL_COUNT) return 0;
        map->levels[index] = (uint8_t)(
            (prediction + delta) & (N148_AQ_LEVEL_COUNT - 1));
        (*decoded_symbols)++;
    }
    return 1;
}

static int decode_transform_map(N148RansDecoder *decoder,
                                const N148RansModel *models, int model,
                                uint32_t symbol_count,
                                uint32_t *decoded_symbols,
                                N148TransformMap *map) {
    if (!decoder || !models || !decoded_symbols || !map || model < 0 ||
        !map->strategies || map->count == 0) return 0;
    for (size_t index = 0; index < map->count; index++) {
        uint8_t delta;
        uint8_t prediction = n148_transform_map_predict(map, index);
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &delta) ||
            delta >= N148_TRANSFORM_STRATEGY_COUNT ||
            prediction >= N148_TRANSFORM_STRATEGY_COUNT) return 0;
        map->strategies[index] = (uint8_t)(
            (prediction + delta) % N148_TRANSFORM_STRATEGY_COUNT);
        (*decoded_symbols)++;
    }
    return n148_transform_map_validate(map);
}

static int decode_luma_transform_map(
    N148RansDecoder *decoder, const N148RansModel *models, int model,
    uint32_t symbol_count, uint32_t *decoded_symbols,
    int dct32, N148TransformMap *map) {
    if (!decoder || !models || !decoded_symbols || !map || model < 0 ||
        !map->strategies || map->plane_offsets[1] == 0) return 0;
    for (size_t index = 0; index < map->plane_offsets[1]; index++) {
        uint8_t delta;
        uint8_t prediction = n148_transform_map_predict(map, index);
        if (*decoded_symbols >= symbol_count ||
            !decode_rans_payload_symbol(
                decoder, &models[model], &delta) ||
            delta >= N148_TRANSFORM_STRATEGY_COUNT ||
            prediction >= N148_TRANSFORM_STRATEGY_COUNT) return 0;
        map->strategies[index] = (uint8_t)(
            (prediction + delta) % N148_TRANSFORM_STRATEGY_COUNT);
        (*decoded_symbols)++;
    }
    return n148_luma_transform_map_validate_extended(map, dct32);
}

int n148_decode_format_2_coefficients(const uint8_t *metadata, size_t metadata_size,
                                const uint8_t *payload_data, size_t payload_size,
                                int width, int height, int chroma,
                                uint32_t features,
                                N148CoeffPlane coefficients[3],
                                N148ContextTrace *trace,
                                N148PredictionModes *prediction,
                                N148AdaptiveMap *adaptive_map,
                                N148TransformMap *transform_map) {
    uint8_t *decoded_modes = NULL;
    N148AdaptiveMap decoded_adaptive = {0};
    N148TransformMap decoded_transform = {0};
    if (!metadata || !payload_data || !coefficients || width <= 0 ||
        height <= 0 || !(features & N148_FEATURE_RANS) ||
        chroma < CHROMA_444 || chroma > CHROMA_420 ||
        (features & ~N148_FORMAT_2_SUPPORTED_FEATURES) ||
        ((features & (N148_FEATURE_CONTEXT | N148_FEATURE_INTRA |
                      N148_FEATURE_PERCEPTUAL_COLOR |
                      N148_FEATURE_ADAPTIVE_QUANT |
                      N148_FEATURE_VARIABLE_TRANSFORM |
                      N148_FEATURE_RDO |
                      N148_FEATURE_LOOP_FILTER)) &&
         !(features & N148_FEATURE_RANS)) ||
        ((features & N148_FEATURE_ADAPTIVE_QUANT) &&
         !(features & N148_FEATURE_INTRA)) ||
        ((features & N148_FEATURE_VARIABLE_TRANSFORM) &&
         !(features & N148_FEATURE_INTRA)) ||
        ((features & N148_FEATURE_RDO) &&
         !(features & N148_FEATURE_INTRA)) ||
        ((features & N148_FEATURE_LOOP_FILTER) &&
         !(features & N148_FEATURE_INTRA)))
        return 0;
    memset(coefficients, 0, 3 * sizeof(*coefficients));
    if (prediction) memset(prediction, 0, sizeof(*prediction));
    if (adaptive_map) memset(adaptive_map, 0, sizeof(*adaptive_map));
    if (transform_map) memset(transform_map, 0, sizeof(*transform_map));
    context_trace_init(trace);
    int use_context = (features & N148_FEATURE_CONTEXT) != 0;
    int use_intra = (features & N148_FEATURE_INTRA) != 0;
    int use_adaptive =
        (features & N148_FEATURE_ADAPTIVE_QUANT) != 0;
    int use_transform =
        (features & N148_FEATURE_VARIABLE_TRANSFORM) != 0;
    int coefficient_model_count = use_context ?
        CONTEXT_MODEL_COUNT : BASE_COEFFICIENT_MODEL_COUNT;
    int model_count = coefficient_model_count +
        (use_intra ? BLOCK_INTRA_MODELS : 0) +
        (use_adaptive ? ADAPTIVE_MAP_MODEL_COUNT : 0) +
        (use_transform ? TRANSFORM_MAP_MODEL_COUNT : 0);
    N148RansModel models[MAX_MODEL_COUNT];
    size_t metadata_consumed = 0;
    if (!parse_rans_models(metadata, metadata_size, models, model_count,
                           &metadata_consumed) ||
        metadata_consumed != metadata_size)
        return 0;

    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    if (!allocate_coefficients(width, height, &coefficients[0]) ||
        !allocate_coefficients(chroma_width, chroma_height, &coefficients[1]) ||
        !allocate_coefficients(chroma_width, chroma_height, &coefficients[2]))
        goto failure;

    ByteReader payload;
    reader_init(&payload, payload_data, payload_size);
    uint8_t payload_version = reader_u8(&payload);
    uint8_t plane_count = reader_u8(&payload);
    uint8_t mode_coder = reader_u8(&payload);
    uint8_t map_coders = reader_u8(&payload);
    uint8_t expected_payload_version = use_transform ?
        PAYLOAD_REVISION_VARIABLE_TRANSFORM : (use_adaptive ?
        PAYLOAD_REVISION_ADAPTIVE_QUANT :
        (use_intra ? PAYLOAD_REVISION_INTRA : PAYLOAD_REVISION_ENTROPY));
    if (payload_version != expected_payload_version ||
        plane_count != PLANE_COUNT ||
        mode_coder != (use_intra ? INTRA_MODE_CODER_RANS : 0) ||
        map_coders != ((use_adaptive ? ADAPTIVE_MAP_CODER_RANS : 0) |
                       (use_transform ? TRANSFORM_MAP_CODER_RANS : 0)))
        goto failure;
    uint32_t symbol_count = reader_u32(&payload);
    uint32_t rans_size = reader_u32(&payload);
    uint32_t amplitude_bits = reader_u32(&payload);
    uint32_t prediction_mode_count = use_intra ? reader_u32(&payload) : 0;
    uint32_t adaptive_map_count = use_adaptive ? reader_u32(&payload) : 0;
    uint32_t transform_map_count = use_transform ? reader_u32(&payload) : 0;
    if (amplitude_bits > UINT32_MAX - 7u) goto failure;
    uint32_t amplitude_size = (amplitude_bits + 7u) / 8u;
    const uint8_t *rans_stream = reader_bytes(&payload, rans_size);
    const uint8_t *amplitudes = reader_bytes(&payload, amplitude_size);
    size_t expected_modes = 0;
    if (use_intra) {
        if (!n148_intra_expected_modes(width, height, chroma,
                                        &expected_modes) ||
            prediction_mode_count != expected_modes) goto failure;
        decoded_modes = (uint8_t *) calloc(expected_modes, 1);
        if (!decoded_modes) goto failure;
    }
    if (use_adaptive) {
        size_t map_columns = (size_t)(width / N148_AQ_REGION_SIZE +
            (width % N148_AQ_REGION_SIZE != 0));
        size_t map_rows = (size_t)(height / N148_AQ_REGION_SIZE +
            (height % N148_AQ_REGION_SIZE != 0));
        if (map_rows == 0 || map_columns > SIZE_MAX / map_rows ||
            map_columns * map_rows != adaptive_map_count) goto failure;
    }
    if (use_transform &&
        (!n148_transform_map_allocate(width, height, chroma,
                                      &decoded_transform) ||
         decoded_transform.count != transform_map_count)) goto failure;
    if (!reader_finished(&payload)) goto failure;

    N148RansDecoder decoder;
    BitReader bits;
    if (!n148_rans_decoder_init_mixed(&decoder, rans_stream, rans_size))
        goto failure;
    bit_reader_init(&bits, amplitudes, amplitude_size, amplitude_bits);
    uint32_t decoded_symbols = 0;
    if (!bits.valid ||
        !decode_rans_plane(&decoder, &bits, models,
                           width / 8 + (width % 8 != 0), 0, use_context,
                           use_intra,
                           symbol_count, &decoded_symbols, &coefficients[0],
                           trace) ||
        !decode_rans_plane(&decoder, &bits, models,
                           chroma_width / 8 + (chroma_width % 8 != 0),
                           1, use_context, use_intra, symbol_count,
                           &decoded_symbols,
                           &coefficients[1], trace) ||
        !decode_rans_plane(&decoder, &bits, models,
                           chroma_width / 8 + (chroma_width % 8 != 0),
                           1, use_context, use_intra, symbol_count,
                           &decoded_symbols,
                           &coefficients[2], trace) ||
        bits.bits_remaining != 0) goto failure;
    if (use_intra) {
        long y_count = coefficients[0].count;
        long cb_count = coefficients[1].count;
        if (!decode_prediction_plane(
                &decoder, models, decoded_modes, y_count,
                width / 8 + (width % 8 != 0), 0,
                coefficient_model_count, symbol_count, &decoded_symbols) ||
            !decode_prediction_plane(
                &decoder, models, decoded_modes + y_count, cb_count,
                chroma_width / 8 + (chroma_width % 8 != 0), 1,
                coefficient_model_count, symbol_count, &decoded_symbols) ||
            !decode_prediction_plane(
                &decoder, models, decoded_modes + y_count + cb_count,
                coefficients[2].count,
                chroma_width / 8 + (chroma_width % 8 != 0), 1,
                coefficient_model_count, symbol_count, &decoded_symbols))
            goto failure;
    }
    if (use_adaptive && !decode_adaptive_map(
            &decoder, models, coefficient_model_count + BLOCK_INTRA_MODELS,
            width, height, symbol_count, &decoded_symbols,
            &decoded_adaptive)) goto failure;
    if (use_transform && !decode_transform_map(
            &decoder, models,
            coefficient_model_count + BLOCK_INTRA_MODELS +
                (use_adaptive ? ADAPTIVE_MAP_MODEL_COUNT : 0),
            symbol_count, &decoded_symbols, &decoded_transform)) goto failure;
    if (decoded_symbols != symbol_count ||
        !n148_rans_decoder_finished(&decoder)) goto failure;
    if (amplitude_bits & 7u) {
        uint8_t padding_mask =
            (uint8_t)((1u << (8u - (amplitude_bits & 7u))) - 1u);
        if (!amplitude_size || amplitudes[amplitude_size - 1] & padding_mask)
            goto failure;
    }
    if (prediction) {
        prediction->values = decoded_modes;
        prediction->count = expected_modes;
        decoded_modes = NULL;
    }
    if (adaptive_map) {
        *adaptive_map = decoded_adaptive;
        memset(&decoded_adaptive, 0, sizeof(decoded_adaptive));
    }
    if (transform_map) {
        *transform_map = decoded_transform;
        memset(&decoded_transform, 0, sizeof(decoded_transform));
    }
    n148_adaptive_map_release(&decoded_adaptive);
    n148_transform_map_release(&decoded_transform);
    free(decoded_modes);
    return 1;

failure:
    free(decoded_modes);
    n148_adaptive_map_release(&decoded_adaptive);
    n148_transform_map_release(&decoded_transform);
    for (int plane = 0; plane < 3; plane++)
        n148_free_coeff_plane(&coefficients[plane]);
    return 0;
}

static int decode_modern_coefficients(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload_data, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    int segmented, int adaptive_filter, int variable_luma, int dct32,
    uint32_t entropy_tools,
    N148CoeffPlane coefficients[3], N148PredictionModes *prediction,
    N148PartitionMap *partition_map,
    N148SegmentationMap *segmentation_map,
    N148TransformMap *transform_map) {
    uint8_t *decoded_modes = NULL;
    N148PartitionMap decoded_partition = {0};
    N148SegmentationMap decoded_segmentation = {0};
    N148TransformMap decoded_transform = {0};
    if ((entropy_tools & N148_ENTROPY_RICH_CONTEXT) && metadata_size > 0 &&
        metadata && metadata[0] == ENTROPY_METADATA_REVISION_BASE)
        entropy_tools &= N148_ENTROPY_MULTIPLE_REFERENCES;
    if (!metadata || !payload_data || !coefficients || !prediction ||
        !partition_map || !segmentation_map || !transform_map ||
        width <= 0 || height <= 0 ||
        chroma < CHROMA_444 || chroma > CHROMA_420 ||
        (joint_chroma != 0 && joint_chroma != 1) ||
        (segmented != 0 && segmented != 1) ||
        (adaptive_filter != 0 && adaptive_filter != 1) ||
        (variable_luma != 0 && variable_luma != 1) ||
        (dct32 != 0 && dct32 != 1) ||
        (entropy_tools & ~(N148_ENTROPY_ADAPTIVE |
                           N148_ENTROPY_RICH_CONTEXT |
                           N148_ENTROPY_MULTIPLE_REFERENCES)) ||
        ((entropy_tools & N148_ENTROPY_ADAPTIVE) &&
         !(features & N148_FEATURE_RANS)) ||
        (dct32 && !variable_luma) ||
        (adaptive_filter && !segmented) ||
        !n148_format_3_features_valid(features) ||
        (joint_chroma &&
         !(features & N148_FEATURE_CONTEXTUAL_RDO))) return 0;
    memset(coefficients, 0, 3 * sizeof(*coefficients));
    memset(prediction, 0, sizeof(*prediction));
    memset(partition_map, 0, sizeof(*partition_map));
    memset(segmentation_map, 0, sizeof(*segmentation_map));
    memset(transform_map, 0, sizeof(*transform_map));
    int use_context = (features & N148_FEATURE_CONTEXT) != 0;
    int use_directional =
        (features & N148_FEATURE_DIRECTIONAL_INTRA) != 0;
    int use_split = (features & N148_FEATURE_INTRA_4X4) != 0;
    int rich_context =
        (entropy_tools & N148_ENTROPY_RICH_CONTEXT) != 0;
    int multiple_references =
        (entropy_tools & N148_ENTROPY_MULTIPLE_REFERENCES) != 0;
    if (rich_context &&
        (!use_context || !(entropy_tools & N148_ENTROPY_ADAPTIVE))) return 0;
    if (multiple_references && (!use_directional ||
                                !(features & N148_FEATURE_CONTEXTUAL_RDO)))
        return 0;
    int coefficient_model_count = rich_context ?
        RICH_CONTEXT_MODEL_COUNT : use_context ?
        CONTEXT_MODEL_COUNT : BASE_COEFFICIENT_MODEL_COUNT;
    int model_count = coefficient_model_count + DIRECTIONAL_MODELS +
        (use_split ? PARTITION_MODEL_COUNT : 0) +
        (segmented ? SEGMENTATION_MODEL_COUNT : 0) +
        (variable_luma ? LUMA_TRANSFORM_MODEL_COUNT : 0);
    N148RansModel models[MAX_MODEL_COUNT];
    uint8_t adaptive_mask[RICH_ADAPTIVE_MASK_SIZE] = {0};
    size_t metadata_consumed = 0;
    if (!(rich_context ? parse_rich_models(
              metadata, metadata_size, models, model_count,
              adaptive_mask,
              &metadata_consumed) :
          parse_rans_models(metadata, metadata_size, models, model_count,
                            &metadata_consumed)) ||
        metadata_consumed != metadata_size) return 0;

    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
    if (!allocate_coefficients(width, height, &coefficients[0]) ||
        !allocate_coefficients(chroma_width, chroma_height, &coefficients[1]) ||
        !allocate_coefficients(chroma_width, chroma_height, &coefficients[2]) ||
        !n148_partition_map_allocate(width, height, chroma,
                                        &decoded_partition) ||
        (variable_luma && !n148_transform_map_allocate(
            width, height, chroma, &decoded_transform))) goto failure;

    ByteReader payload;
    reader_init(&payload, payload_data, payload_size);
    uint8_t payload_version = reader_u8(&payload);
    uint8_t plane_count = reader_u8(&payload);
    uint8_t mode_coder = reader_u8(&payload);
    uint8_t map_coders = reader_u8(&payload);
    uint8_t expected_payload_version = rich_context ?
        PAYLOAD_REVISION_RICH_CONTEXT : variable_luma ?
        PAYLOAD_REVISION_VARIABLE_LUMA : adaptive_filter ?
        (joint_chroma ? PAYLOAD_REVISION_JOINT_ADAPTIVE_FILTER :
                        PAYLOAD_REVISION_ADAPTIVE_FILTER) :
        segmented ?
        (joint_chroma ? PAYLOAD_REVISION_JOINT_SEGMENTATION :
                        PAYLOAD_REVISION_SEGMENTATION) :
        (joint_chroma ? PAYLOAD_REVISION_JOINT_CHROMA :
                        PAYLOAD_REVISION_DIRECTIONAL_INTRA);
    uint8_t expected_map_coders = (uint8_t)(
        (use_split ? PARTITION_MAP_CODER_RANS : 0) |
        (segmented ? SEGMENT_MAP_CODER_RANS : 0) |
        (variable_luma ? TRANSFORM_MAP_CODER_RANS : 0));
    if (payload_version != expected_payload_version ||
        plane_count != PLANE_COUNT ||
        mode_coder != INTRA_MODE_CODER_RANS ||
        map_coders != expected_map_coders)
        goto failure;
    uint32_t symbol_count = reader_u32(&payload);
    uint32_t rans_size = reader_u32(&payload);
    uint32_t amplitude_bits = reader_u32(&payload);
    uint32_t prediction_mode_count = reader_u32(&payload);
    uint32_t partition_count = use_split ? reader_u32(&payload) : 0;
    uint32_t segmentation_count = segmented ? reader_u32(&payload) : 0;
    uint32_t transform_count = variable_luma ? reader_u32(&payload) : 0;
    uint8_t segmentation_scales[N148_SEGMENT_COUNT] = {100, 100, 100, 100};
    if (segmented) {
        for (int segment = 0; segment < N148_SEGMENT_COUNT; segment++) {
            segmentation_scales[segment] = reader_u8(&payload);
            if (segmentation_scales[segment] < N148_SEGMENT_MIN_SCALE ||
                segmentation_scales[segment] > N148_SEGMENT_MAX_SCALE)
                goto failure;
        }
    }
    if (amplitude_bits > UINT32_MAX - 7u) goto failure;
    uint32_t amplitude_size = (amplitude_bits + 7u) / 8u;
    const uint8_t *rans_stream = reader_bytes(&payload, rans_size);
    const uint8_t *amplitudes = reader_bytes(&payload, amplitude_size);
    size_t expected_modes;
    long y_blocks = coefficients[0].count;
    long cb_blocks = coefficients[1].count;
    int y_blocks_x = width / 8 + (width % 8 != 0);
    int y_blocks_y = height / 8 + (height % 8 != 0);
    size_t y_modes = (size_t) y_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK;
    size_t cb_modes = (size_t) cb_blocks * N148_INTRA_MODE_SLOTS_PER_BLOCK;
    size_t expected_segmentation_count =
        (size_t)(width / N148_SEGMENT_REGION_SIZE +
                 (width % N148_SEGMENT_REGION_SIZE != 0)) *
        (size_t)(height / N148_SEGMENT_REGION_SIZE +
                 (height % N148_SEGMENT_REGION_SIZE != 0));
    if (!n148_directional_intra_expected_modes(width, height, chroma,
                                       &expected_modes) ||
        (joint_chroma && y_modes > SIZE_MAX - cb_modes) ||
        (joint_chroma ? prediction_mode_count != y_modes + cb_modes :
                        prediction_mode_count != expected_modes) ||
        (use_split && partition_count != decoded_partition.count) ||
        (segmented && segmentation_count != expected_segmentation_count) ||
        (variable_luma &&
         transform_count != decoded_transform.plane_offsets[1]) ||
        !reader_finished(&payload)) goto failure;
    decoded_modes = (uint8_t *) calloc(expected_modes, 1);
    if (!decoded_modes) goto failure;

    N148RansDecoder decoder;
    N148RansAdaptiveModel adaptive_models[MAX_MODEL_COUNT];
    BitReader bits;
    int use_adaptive_decoder = (entropy_tools & N148_ENTROPY_ADAPTIVE) != 0;
    if (rich_context) {
        use_adaptive_decoder = 0;
        for (size_t index = 0; index < RICH_ADAPTIVE_MASK_SIZE; index++)
            use_adaptive_decoder |= adaptive_mask[index] != 0;
    }
    if (use_adaptive_decoder) {
        if (!n148_rans_decoder_init_mixed_adaptive(
                &decoder, rans_stream, rans_size, models, adaptive_models,
                (size_t) model_count,
                rich_context ?
                    RICH_LUMA_DC_MODELS + RICH_LUMA_AC_MODELS :
                    (size_t) model_count,
                rich_context ? adaptive_mask : NULL,
                rich_context ? RICH_ADAPTIVE_MODEL_COUNT : 0,
                N148_ENTROPY_ADAPT_RATE_SHIFT))
            goto failure;
    } else if (!n148_rans_decoder_init_mixed(
                   &decoder, rans_stream, rans_size)) goto failure;
    bit_reader_init(&bits, amplitudes, amplitude_size, amplitude_bits);
    uint32_t decoded_symbols = 0;
    int segmentation_model = coefficient_model_count + DIRECTIONAL_MODELS +
        (use_split ? PARTITION_MODEL_COUNT : 0);
    int transform_model = segmentation_model +
        (segmented ? SEGMENTATION_MODEL_COUNT : 0);
    int allowed_modes = multiple_references ?
        N148_PACKED_INTRA_MODE_COUNT :
        (use_directional ? N148_DIRECTIONAL_INTRA_MODE_COUNT : 4);
    int chroma_allowed_modes = use_directional ?
        N148_DIRECTIONAL_INTRA_MODE_COUNT : 4;
    if (!bits.valid ||
        (variable_luma && !decode_luma_transform_map(
              &decoder, models, transform_model, symbol_count,
              &decoded_symbols, dct32, &decoded_transform)) ||
        (use_split && !(variable_luma ? decode_variable_luma_partition_map(
              &decoder, models, coefficient_model_count + DIRECTIONAL_MODELS,
              symbol_count, &decoded_symbols, &decoded_transform,
              &decoded_partition, dct32) :
          decode_partition_map(
              &decoder, models, coefficient_model_count + DIRECTIONAL_MODELS,
              symbol_count, &decoded_symbols, &decoded_partition))) ||
        (segmented && !decode_segmentation_map(
            &decoder, models, segmentation_model, width, height,
            symbol_count, &decoded_symbols, &decoded_segmentation)))
        goto failure;

#define DECODE_MODERN_MODES() \
    ((variable_luma ? decode_variable_luma_prediction_plane( \
          &decoder, models, decoded_modes, y_blocks, y_blocks_x, y_blocks_y, \
          coefficient_model_count, allowed_modes, \
          decoded_partition.split + decoded_partition.plane_offsets[0], \
          symbol_count, &decoded_symbols, &decoded_transform, dct32) : \
      decode_directional_prediction_plane( \
          &decoder, models, decoded_modes, y_blocks, y_blocks_x, 0, \
          coefficient_model_count, allowed_modes, \
          decoded_partition.split + decoded_partition.plane_offsets[0], \
          symbol_count, &decoded_symbols)) && \
     decode_directional_prediction_plane( \
          &decoder, models, decoded_modes + y_modes, cb_blocks, \
          chroma_width / 8 + (chroma_width % 8 != 0), 1, \
          coefficient_model_count, chroma_allowed_modes, \
          decoded_partition.split + decoded_partition.plane_offsets[1], \
          symbol_count, &decoded_symbols) && \
     (joint_chroma || decode_directional_prediction_plane( \
          &decoder, models, decoded_modes + y_modes + cb_modes, \
          coefficients[2].count, \
          chroma_width / 8 + (chroma_width % 8 != 0), 1, \
          coefficient_model_count, chroma_allowed_modes, \
          decoded_partition.split + decoded_partition.plane_offsets[2], \
          symbol_count, &decoded_symbols)))

    int syntax_ok;
    if (rich_context) {
        syntax_ok = DECODE_MODERN_MODES();
        if (syntax_ok && joint_chroma) {
            if (coefficients[2].count != cb_blocks) goto failure;
            memcpy(decoded_modes + y_modes + cb_modes,
                   decoded_modes + y_modes, cb_modes);
        }
        syntax_ok = syntax_ok &&
            (variable_luma ? decode_rich_luma_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[0],
                decoded_modes, y_blocks_x, y_blocks_y, symbol_count,
                &decoded_symbols, &decoded_transform, &coefficients[0]) :
             decode_rich_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[0],
                decoded_modes, y_blocks_x, 0, symbol_count,
                &decoded_symbols, &coefficients[0])) &&
            decode_rich_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[1],
                decoded_modes + y_modes,
                chroma_width / 8 + (chroma_width % 8 != 0), 1,
                symbol_count, &decoded_symbols, &coefficients[1]) &&
            decode_rich_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[2],
                decoded_modes + y_modes + cb_modes,
                chroma_width / 8 + (chroma_width % 8 != 0), 1,
                symbol_count, &decoded_symbols, &coefficients[2]);
    } else {
        FixedRansDecoder fixed_decoder;
        FixedRansDecoder *fixed = NULL;
        if (!decoder.adaptive_models) {
            if (decoded_symbols > symbol_count) goto failure;
            fixed_rans_begin(
                &decoder, symbol_count - decoded_symbols, &fixed_decoder);
            fixed = &fixed_decoder;
        }
        syntax_ok =
            (variable_luma ? decode_variable_luma_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[0],
                y_blocks_x, y_blocks_y, use_context, symbol_count,
                &decoded_symbols, &decoded_transform, &coefficients[0], fixed,
                dct32) :
             decode_directional_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[0],
                y_blocks_x, 0, use_context, symbol_count,
                &decoded_symbols, &coefficients[0], fixed)) &&
            decode_directional_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[1],
                chroma_width / 8 + (chroma_width % 8 != 0), 1, use_context,
                symbol_count, &decoded_symbols, &coefficients[1], fixed) &&
            decode_directional_coefficient_plane(
                &decoder, &bits, models,
                decoded_partition.split + decoded_partition.plane_offsets[2],
                chroma_width / 8 + (chroma_width % 8 != 0), 1, use_context,
                symbol_count, &decoded_symbols, &coefficients[2], fixed);
        if (syntax_ok && fixed) {
            decoded_symbols = symbol_count - fixed->symbols_remaining;
            fixed_rans_commit(fixed, &decoder);
        }
        syntax_ok = syntax_ok && DECODE_MODERN_MODES();
    }
#undef DECODE_MODERN_MODES
    if (!syntax_ok || bits.bits_remaining != 0 ||
        decoded_symbols != symbol_count ||
        !n148_rans_decoder_finished(&decoder)) goto failure;
    if (segmented) {
        memcpy(decoded_segmentation.quant_scale, segmentation_scales,
               sizeof(segmentation_scales));
        if (!n148_segmentation_map_validate(&decoded_segmentation))
            goto failure;
    }
    if (joint_chroma) {
        if (coefficients[2].count != cb_blocks) goto failure;
        memcpy(decoded_modes + y_modes + cb_modes,
               decoded_modes + y_modes, cb_modes);
    }
    if (amplitude_bits & 7u) {
        uint8_t padding_mask =
            (uint8_t)((1u << (8u - (amplitude_bits & 7u))) - 1u);
        if (!amplitude_size || amplitudes[amplitude_size - 1] & padding_mask)
            goto failure;
    }
    prediction->values = decoded_modes;
    prediction->count = expected_modes;
    decoded_modes = NULL;
    *partition_map = decoded_partition;
    memset(&decoded_partition, 0, sizeof(decoded_partition));
    if (segmented) {
        *segmentation_map = decoded_segmentation;
        memset(&decoded_segmentation, 0, sizeof(decoded_segmentation));
    }
    if (variable_luma) {
        *transform_map = decoded_transform;
        memset(&decoded_transform, 0, sizeof(decoded_transform));
    }
    return 1;

failure:
    free(decoded_modes);
    n148_partition_map_release(&decoded_partition);
    n148_segmentation_map_release(&decoded_segmentation);
    n148_transform_map_release(&decoded_transform);
    for (int plane = 0; plane < 3; plane++)
        n148_free_coeff_plane(&coefficients[plane]);
    return 0;
}

static int inspect_modern_syntax_entropy(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    int segmented, int adaptive_filter, int variable_luma, int dct32,
    uint32_t entropy_tools,
    N148SyntaxInspection *inspection) {
    if (!inspection) return 0;
    memset(inspection, 0, sizeof(*inspection));
    N148CoeffPlane coefficients[3] = {{0}};
    N148PredictionModes prediction = {0};
    N148PartitionMap partition_map = {0};
    N148SegmentationMap segmentation_map = {0};
    N148TransformMap transform_map = {0};
    /* The extended transform-map validator is shared with the codec and is
       selected through the internal fidelity context.  Inspection is also a
       standalone entry point, so establish the syntax context explicitly
       instead of depending on whichever encode/decode happened previously. */
    uint32_t inspection_tools = dct32 ? N148_FIDELITY_DCT32 : 0;
    if (entropy_tools & N148_ENTROPY_MULTIPLE_REFERENCES)
        inspection_tools |= N148_FIDELITY_MULTIPLE_REFERENCES;
    if (!n148_fidelity_tools_set(inspection_tools)) return 0;
    int success = decode_modern_coefficients(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, joint_chroma, segmented,
        adaptive_filter, variable_luma, dct32, entropy_tools,
        coefficients, &prediction,
        &partition_map, &segmentation_map, &transform_map);
    n148_fidelity_tools_set(0);
    if (success) {
        size_t mode_offset = 0;
        for (int plane = 0; plane < 3 && success; plane++) {
            for (long block = 0; block < coefficients[plane].count; block++) {
                size_t map_index = partition_map.plane_offsets[plane] +
                    (size_t) block;
                int split = partition_map.split[map_index];
                int parts = split ? N148_INTRA_MODE_SLOTS_PER_BLOCK : 1;
                inspection->total_blocks++;
                inspection->split_blocks += (uint64_t) split;
                inspection->plane_total_blocks[plane]++;
                inspection->plane_split_blocks[plane] += (uint64_t) split;
                for (int part = 0; part < parts; part++) {
                    uint8_t mode = prediction.values[
                        mode_offset + (size_t) block *
                            N148_INTRA_MODE_SLOTS_PER_BLOCK + (size_t) part];
                    int reference = n148_intra_reference_index(mode);
                    int base_mode = n148_intra_base_mode(mode);
                    if (base_mode >= N148_DIRECTIONAL_INTRA_MODE_COUNT ||
                        reference >= N148_INTRA_REFERENCE_COUNT ||
                        (plane != 0 && reference != 0) ||
                        (split && reference != 0)) {
                        success = 0;
                        break;
                    }
                    inspection->mode_histogram[base_mode]++;
                    inspection->plane_mode_histogram[plane][base_mode]++;
                    if (plane == 0)
                        inspection->luma_reference_histogram[reference]++;
                }
            }
            mode_offset += (size_t) coefficients[plane].count *
                N148_INTRA_MODE_SLOTS_PER_BLOCK;
        }
    }
    for (int plane = 0; plane < 3; plane++)
        n148_free_coeff_plane(&coefficients[plane]);
    n148_prediction_modes_release(&prediction);
    if (success && segmented) {
        inspection->segment_count = segmentation_map.count;
        memcpy(inspection->segment_quant_scale,
               segmentation_map.quant_scale,
               sizeof(inspection->segment_quant_scale));
        for (size_t index = 0; index < segmentation_map.count; index++)
            inspection->segment_histogram[segmentation_map.levels[index]]++;
    }
    n148_segmentation_map_release(&segmentation_map);
    if (success && variable_luma) {
        inspection->luma_transform_regions =
            transform_map.plane_offsets[1];
        for (int macro_y = 0; macro_y < transform_map.rows[0]; macro_y++) {
            for (int macro_x = 0; macro_x < transform_map.columns[0];
                 macro_x++) {
                size_t index = (size_t) macro_y *
                    (size_t) transform_map.columns[0] + (size_t) macro_x;
                if (n148_luma_transform_dct32_member(
                        &transform_map, macro_x, macro_y)) {
                    if (n148_luma_transform_dct32_origin(
                            &transform_map, macro_x, macro_y))
                        inspection->luma_transform_32x32++;
                    continue;
                }
                if (transform_map.strategies[index] ==
                    N148_TRANSFORM_DCT16) {
                    inspection->luma_transform_16x16++;
                    continue;
                }
                for (int cell_y = 0; cell_y < 2; cell_y++) {
                    int block_y = macro_y * 2 + cell_y;
                    if (block_y >= transform_map.blocks_y[0]) continue;
                    for (int cell_x = 0; cell_x < 2; cell_x++) {
                        int block_x = macro_x * 2 + cell_x;
                        if (block_x >= transform_map.blocks_x[0]) continue;
                        size_t block = (size_t) block_y *
                            (size_t) transform_map.blocks_x[0] +
                            (size_t) block_x;
                        if (partition_map.split[block])
                            inspection->luma_transform_4x4 += 4;
                        else
                            inspection->luma_transform_8x8++;
                    }
                }
            }
        }
        for (int macro_y = 0; macro_y + 1 < transform_map.rows[0];
             macro_y += 2) {
            for (int macro_x = 0; macro_x + 1 < transform_map.columns[0];
                 macro_x += 2) {
                int all_dct16 = 1;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        size_t index = (size_t)(macro_y + dy) *
                            (size_t) transform_map.columns[0] +
                            (size_t)(macro_x + dx);
                        if (transform_map.strategies[index] !=
                            N148_TRANSFORM_DCT16) all_dct16 = 0;
                    }
                }
                inspection->luma_transform_32x32_opportunities +=
                    (uint64_t) all_dct16;
            }
        }
    }
    n148_transform_map_release(&transform_map);
    n148_partition_map_release(&partition_map);
    if (!success) memset(inspection, 0, sizeof(*inspection));
    return success;
}

static int inspect_modern_syntax(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    int segmented, int adaptive_filter, int variable_luma, int dct32,
    N148SyntaxInspection *inspection) {
    return inspect_modern_syntax_entropy(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, joint_chroma, segmented,
        adaptive_filter, variable_luma, dct32, 0, inspection);
}

int n148_entropy_profile_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    int segmented, int adaptive_filter, int variable_luma, int dct32,
    uint32_t entropy_tools, N148SyntaxInspection *inspection) {
    return inspect_modern_syntax_entropy(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, joint_chroma, segmented,
        adaptive_filter, variable_luma, dct32, entropy_tools, inspection);
}

int n148_directional_intra_inspect(const uint8_t *metadata, size_t metadata_size,
                    const uint8_t *payload, size_t payload_size,
                    int width, int height, int chroma, uint32_t features,
                    N148SyntaxInspection *inspection) {
    return inspect_modern_syntax(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, 0, 0, 0, 0, 0, inspection);
}

int n148_joint_chroma_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features,
    N148SyntaxInspection *inspection) {
    return inspect_modern_syntax(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, 1, 0, 0, 0, 0, inspection);
}

int n148_segmented_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    N148SyntaxInspection *inspection) {
    return inspect_modern_syntax(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, joint_chroma, 1, 0, 0, 0,
        inspection);
}

int n148_adaptive_filter_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    N148SyntaxInspection *inspection) {
    return inspect_modern_syntax(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, joint_chroma, 1, 1, 0, 0,
        inspection);
}

int n148_predictive_inspect(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int chroma, uint32_t features, int joint_chroma,
    int segmented, int adaptive_filter, int variable_luma, int dct32,
    N148SyntaxInspection *inspection) {
    return inspect_modern_syntax(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, joint_chroma, segmented,
        adaptive_filter, variable_luma, dct32, inspection);
}

static int read_huffman_specs(const uint8_t *metadata, size_t metadata_size,
                              HuffSpec specs[HUFFMAN_TABLE_COUNT]) {
    ByteReader reader;
    reader_init(&reader, metadata, metadata_size);
    if (reader_u8(&reader) != ENTROPY_METADATA_REVISION_BASE ||
        reader_u8(&reader) != ENTROPY_CODER_HUFFMAN ||
        reader_u8(&reader) != HUFFMAN_TABLE_COUNT ||
        reader_u8(&reader) != 0) return 0;
    memset(specs, 0, HUFFMAN_TABLE_COUNT * sizeof(*specs));
    for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
        for (int length = 1; length <= 16; length++) {
            specs[table].bits[length] = reader_u8(&reader);
            specs[table].value_count += specs[table].bits[length];
            if (!reader.valid ||
                specs[table].value_count > HUFFMAN_MAX_SYMBOLS) return 0;
        }
        const uint8_t *values = reader_bytes(
            &reader, (size_t) specs[table].value_count);
        if (!values) return 0;
        memcpy(specs[table].values, values,
               (size_t) specs[table].value_count);
        if (!huffman_spec_is_valid(&specs[table])) return 0;
    }
    return reader_finished(&reader);
}

int n148_decode_format_2(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma,
                   uint32_t features,
                   Plane *y, Plane *cb, Plane *cr) {
    if (!y || !cb || !cr) return 0;
    *y = (Plane){0};
    *cb = (Plane){0};
    *cr = (Plane){0};
    if (features & N148_FEATURE_RANS) {
        N148CoeffPlane coefficients[3] = {{0}};
        N148PredictionModes prediction = {0};
        N148AdaptiveMap adaptive_map = {0};
        N148TransformMap transform_map = {0};
        int chroma_width, chroma_height;
        chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);
        int success = n148_decode_format_2_coefficients(
            metadata, metadata_size, payload, payload_size,
            width, height, chroma, features, coefficients, NULL,
            &prediction, &adaptive_map, &transform_map);
        int use_perceptual =
            (features & N148_FEATURE_PERCEPTUAL_COLOR) != 0;
        if (success && (features & N148_FEATURE_INTRA)) {
            success = n148_intra_reconstruct_planes(
                coefficients, prediction.values, prediction.count,
                width, height, quality, chroma, use_perceptual,
                (features & N148_FEATURE_ADAPTIVE_QUANT) ?
                    &adaptive_map : NULL,
                (features & N148_FEATURE_VARIABLE_TRANSFORM) ?
                    &transform_map : NULL,
                (features & N148_FEATURE_LOOP_FILTER) != 0,
                y, cb, cr);
        } else {
            if (success) success = n148_reconstruct_coeff_plane_ex(
                coefficients[0].coefficients, coefficients[0].count,
                width, height, quality, 0, use_perceptual, y);
            if (success) success = n148_reconstruct_coeff_plane_ex(
                coefficients[1].coefficients, coefficients[1].count,
                chroma_width, chroma_height, quality, 1, use_perceptual, cb);
            if (success) success = n148_reconstruct_coeff_plane_ex(
                coefficients[2].coefficients, coefficients[2].count,
                chroma_width, chroma_height, quality, 1, use_perceptual, cr);
        }
        for (int plane = 0; plane < 3; plane++)
            n148_free_coeff_plane(&coefficients[plane]);
        n148_prediction_modes_release(&prediction);
        n148_adaptive_map_release(&adaptive_map);
        n148_transform_map_release(&transform_map);
        if (!success) {
            free_plane(y); free_plane(cb); free_plane(cr);
        }
        return success;
    }

    HuffSpec specs[HUFFMAN_TABLE_COUNT];
    DecodeStats stats;
    if (!read_huffman_specs(metadata, metadata_size, specs) ||
        payload_size > LONG_MAX ||
        !decode_image((unsigned char *) payload, (long) payload_size,
                      width, height, quality, chroma, specs,
                      y, cb, cr, &stats) ||
        stats.bytes_consumed != (long) payload_size) {
        free_plane(y); free_plane(cb); free_plane(cr);
        return 0;
    }
    return 1;
}

int n148_predictive_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma, int calibrated_chroma, int segmented,
    int adaptive_filter, int calibrated_luma, int variable_luma,
    int quant_adaptive_filter, int dct32, uint32_t entropy_tools,
    Plane *y, Plane *cb, Plane *cr) {
    if (!y || !cb || !cr || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 ||
        (joint_chroma != 0 && joint_chroma != 1) ||
        (calibrated_chroma != 0 && calibrated_chroma != 1) ||
        (calibrated_luma != 0 && calibrated_luma != 1) ||
        (variable_luma != 0 && variable_luma != 1) ||
        (quant_adaptive_filter != 0 && quant_adaptive_filter != 1) ||
        (dct32 != 0 && dct32 != 1) ||
        (dct32 && !variable_luma) ||
        (quant_adaptive_filter &&
         (!(features & N148_FEATURE_LOOP_FILTER) || !calibrated_luma)) ||
        (variable_luma &&
         (!(features & N148_FEATURE_CONTEXTUAL_RDO) ||
          !(features & N148_FEATURE_INTRA_4X4))) ||
        (segmented != 0 && segmented != 1) ||
        (adaptive_filter != 0 && adaptive_filter != 1) ||
        (adaptive_filter && (!segmented ||
         !(features & N148_FEATURE_LOOP_FILTER))) ||
        !n148_format_3_features_valid(features) ||
        (joint_chroma &&
         !(features & N148_FEATURE_CONTEXTUAL_RDO))) return 0;
    *y = (Plane){0};
    *cb = (Plane){0};
    *cr = (Plane){0};
    N148CoeffPlane coefficients[3] = {{0}};
    N148PredictionModes prediction = {0};
    N148PartitionMap partition_map = {0};
    N148SegmentationMap segmentation_map = {0};
    N148TransformMap transform_map = {0};
    int success = decode_modern_coefficients(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, features, joint_chroma, segmented,
        adaptive_filter, variable_luma, dct32, entropy_tools,
        coefficients, &prediction,
        &partition_map, &segmentation_map, &transform_map);
    if (success && segmented)
        segmentation_map.adaptive_filter = (uint8_t) adaptive_filter;
    if (success) {
        int loop_filter_policy =
            (features & N148_FEATURE_LOOP_FILTER) ?
            (quant_adaptive_filter ? N148_LOOP_FILTER_QUANT_ADAPTIVE_LUMA :
                                     N148_LOOP_FILTER_LEGACY) :
            N148_LOOP_FILTER_OFF;
        success = n148_directional_intra_reconstruct_planes(
            coefficients, prediction.values, prediction.count,
            width, height, quality, chroma_quality, calibrated_chroma,
            calibrated_luma, chroma,
            (features & N148_FEATURE_DIRECTIONAL_INTRA) != 0,
            (features & N148_FEATURE_INTRA_4X4) != 0,
            loop_filter_policy,
            &partition_map, segmented ? &segmentation_map : NULL,
            variable_luma ? &transform_map : NULL,
            y, cb, cr);
    }
    for (int plane = 0; plane < 3; plane++)
        n148_free_coeff_plane(&coefficients[plane]);
    n148_prediction_modes_release(&prediction);
    n148_partition_map_release(&partition_map);
    n148_segmentation_map_release(&segmentation_map);
    n148_transform_map_release(&transform_map);
    if (!success) {
        free_plane(y);
        free_plane(cb);
        free_plane(cr);
    }
    return success;
}

int n148_decode_format_3(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma,
                   uint32_t features,
                   Plane *y, Plane *cb, Plane *cr) {
    return n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, quality, chroma, features,
        0, 0, 0, 0, 0, 0, 0, 0, 0,
        y, cb, cr);
}

int n148_joint_chroma_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma, uint32_t features,
    Plane *y, Plane *cb, Plane *cr) {
    return n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, quality, chroma, features,
        1, 0, 0, 0, 0, 0, 0, 0, 0,
        y, cb, cr);
}

int n148_calibrated_chroma_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma,
    Plane *y, Plane *cb, Plane *cr) {
    return n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, chroma_quality, chroma, features,
        joint_chroma, 1, 0, 0, 0, 0, 0, 0, 0, y, cb, cr);
}

int n148_segmented_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma,
    Plane *y, Plane *cb, Plane *cr) {
    return n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, chroma_quality, chroma, features,
        joint_chroma, 1, 1, 0, 0, 0, 0, 0, 0, y, cb, cr);
}

int n148_adaptive_filter_decode(
    const uint8_t *metadata, size_t metadata_size,
    const uint8_t *payload, size_t payload_size,
    int width, int height, int quality, int chroma_quality, int chroma,
    uint32_t features, int joint_chroma,
    Plane *y, Plane *cb, Plane *cr) {
    return n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, chroma_quality, chroma, features,
        joint_chroma, 1, 1, 1, 0, 0, 0, 0, 0, y, cb, cr);
}

void n148_encoded_payload_release(N148EncodedPayload *encoded) {
    if (!encoded) return;
    free(encoded->metadata);
    free(encoded->payload);
    memset(encoded, 0, sizeof(*encoded));
}

void n148_prediction_modes_release(N148PredictionModes *prediction) {
    if (!prediction) return;
    free(prediction->values);
    memset(prediction, 0, sizeof(*prediction));
}

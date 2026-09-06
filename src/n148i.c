/*
 * N.148i public memory-to-memory API
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include "n148i.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "decoder.h"
#include "encoder.h"
#include "header.h"
#include "huffman.h"
#include "parallel.h"
#include "ppm.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t payload_size;
    size_t payload_offset;
    uint8_t version;
    uint8_t quality;
    uint8_t chroma;
    uint8_t optimized;
    HuffSpec specs[HUFFMAN_TABLE_COUNT];
} ParsedHeader;

static void memory_write_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t) (value & 0xffu);
    destination[1] = (uint8_t) ((value >> 8) & 0xffu);
    destination[2] = (uint8_t) ((value >> 16) & 0xffu);
    destination[3] = (uint8_t) ((value >> 24) & 0xffu);
}

static uint32_t memory_read_u32(const uint8_t *source) {
    return (uint32_t) source[0] |
        ((uint32_t) source[1] << 8) |
        ((uint32_t) source[2] << 16) |
        ((uint32_t) source[3] << 24);
}

static int valid_chroma(int chroma) {
    return chroma == CHROMA_444 || chroma == CHROMA_422 ||
        chroma == CHROMA_420;
}

static int multiplication_overflows(size_t first, size_t second) {
    return second != 0 && first > SIZE_MAX / second;
}

static n148i_result_t parse_header(const uint8_t *encoded,
                                   size_t encoded_size,
                                   ParsedHeader *parsed) {
    if (!encoded || !parsed) return N148I_ERROR_INVALID_ARGUMENT;
    if (encoded_size < N148I_HEADER_SIZE) return N148I_ERROR_TRUNCATED_DATA;
    if (memcmp(encoded, N148I_MAGIC, N148I_MAGIC_LEN) != 0)
        return N148I_ERROR_INVALID_FORMAT;

    memset(parsed, 0, sizeof(*parsed));
    parsed->version = encoded[5];
    if (parsed->version != N148I_VERSION)
        return N148I_ERROR_UNSUPPORTED_VERSION;

    parsed->width = memory_read_u32(encoded + 6);
    parsed->height = memory_read_u32(encoded + 10);
    parsed->quality = encoded[14];
    parsed->chroma = encoded[15];
    parsed->optimized = encoded[16];
    parsed->payload_size = memory_read_u32(encoded + 17);

    if (parsed->width == 0 || parsed->width > INT_MAX ||
        parsed->height == 0 || parsed->height > INT_MAX ||
        parsed->quality < 1 || parsed->quality > 100 ||
        !valid_chroma(parsed->chroma) || parsed->optimized > 1 ||
        parsed->payload_size == 0) {
        return N148I_ERROR_INVALID_FORMAT;
    }
#if LONG_MAX < UINT32_MAX
    if (parsed->payload_size > (uint32_t) LONG_MAX)
        return N148I_ERROR_SIZE_OVERFLOW;
#endif

    size_t offset = N148I_HEADER_SIZE;
    if (parsed->optimized) {
        for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
            HuffSpec *spec = &parsed->specs[table];
            if (encoded_size - offset < 16)
                return N148I_ERROR_TRUNCATED_DATA;
            for (int length = 1; length <= 16; length++) {
                int count = encoded[offset++];
                spec->bits[length] = count;
                spec->value_count += count;
                if (spec->value_count > HUFFMAN_MAX_SYMBOLS)
                    return N148I_ERROR_CORRUPT_DATA;
            }
            if (spec->value_count == 0)
                return N148I_ERROR_CORRUPT_DATA;
            if ((size_t) spec->value_count > encoded_size - offset)
                return N148I_ERROR_TRUNCATED_DATA;
            memcpy(spec->values, encoded + offset,
                   (size_t) spec->value_count);
            offset += (size_t) spec->value_count;
            if (!huffman_spec_is_valid(spec))
                return N148I_ERROR_CORRUPT_DATA;
        }
    } else {
        huffman_default_specs(parsed->specs);
    }

    if ((size_t) parsed->payload_size > encoded_size - offset)
        return N148I_ERROR_TRUNCATED_DATA;
    if ((size_t) parsed->payload_size != encoded_size - offset)
        return N148I_ERROR_CORRUPT_DATA;
    parsed->payload_offset = offset;
    return N148I_OK;
}

static n148i_result_t normalize_options(
    const n148i_encode_options_t *input,
    n148i_encode_options_t *output) {
    n148i_encode_options_init(output);
    if (!input) return N148I_OK;

    if (input->struct_size <
        offsetof(n148i_encode_options_t, quality) + sizeof(input->quality)) {
        return N148I_ERROR_INVALID_ARGUMENT;
    }
    size_t copy_size = input->struct_size;
    if (copy_size > sizeof(*output)) copy_size = sizeof(*output);
    memcpy(output, input, copy_size);
    output->struct_size = sizeof(*output);

    if (output->quality < 1 || output->quality > 100 ||
        !valid_chroma(output->chroma) ||
        (output->optimize_huffman != 0 && output->optimize_huffman != 1) ||
        output->thread_count < 0 || output->thread_count > N148I_MAX_THREADS) {
        return N148I_ERROR_INVALID_ARGUMENT;
    }
    return N148I_OK;
}

void n148i_encode_options_init(n148i_encode_options_t *options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->quality = 50;
    options->chroma = N148I_CHROMA_420;
    options->optimize_huffman = 1;
    options->thread_count = 0;
}

n148i_result_t n148i_encode_memory(const n148i_image_t *image,
                                   const n148i_encode_options_t *options,
                                   uint8_t **encoded,
                                   size_t *encoded_size) {
    if (!image || !image->pixels || !encoded || !encoded_size ||
        image->width == 0 || image->width > INT_MAX ||
        image->height == 0 || image->height > INT_MAX) {
        return N148I_ERROR_INVALID_ARGUMENT;
    }
    *encoded = NULL;
    *encoded_size = 0;

    n148i_encode_options_t settings;
    n148i_result_t result = normalize_options(options, &settings);
    if (result != N148I_OK) return result;

    size_t width = image->width;
    size_t height = image->height;
    if (multiplication_overflows(width, 3))
        return N148I_ERROR_SIZE_OVERFLOW;
    size_t tight_stride = width * 3;
    size_t source_stride = image->stride ? image->stride : tight_stride;
    if (source_stride < tight_stride ||
        (height > 1 &&
         source_stride > (SIZE_MAX - tight_stride) / (height - 1)) ||
        multiplication_overflows(tight_stride, height) ||
        multiplication_overflows(width, height)) {
        return N148I_ERROR_SIZE_OVERFLOW;
    }
    size_t pixel_count = width * height;
#if LONG_MAX < SIZE_MAX
    if (pixel_count > (size_t) LONG_MAX)
        return N148I_ERROR_SIZE_OVERFLOW;
#endif

    uint8_t *compact = NULL;
    Image internal_image;
    internal_image.width = (int) image->width;
    internal_image.height = (int) image->height;
    internal_image.pixels = image->pixels;
    if (source_stride != tight_stride) {
        compact = (uint8_t *) malloc(tight_stride * height);
        if (!compact) return N148I_ERROR_OUT_OF_MEMORY;
        for (size_t row = 0; row < height; row++) {
            memcpy(compact + row * tight_stride,
                   image->pixels + row * source_stride, tight_stride);
        }
        internal_image.pixels = compact;
    }

    if (settings.thread_count > 0)
        n148_set_thread_count(settings.thread_count);

    Plane y = {0}, cb = {0}, cr = {0};
    HuffSpec specs[HUFFMAN_TABLE_COUNT];
    EncodeStats stats;
    uint8_t *payload = NULL;
    int ok = split_channels(&internal_image, &y, &cb, &cr, settings.chroma);
    free(compact);
    if (ok) {
        ok = encode_image(&y, &cb, &cr, settings.quality,
                          settings.optimize_huffman, specs, &payload, &stats);
    }
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    if (!ok) {
        free(payload);
        return N148I_ERROR_ENCODE_FAILED;
    }

    if (stats.data_size <= 0 || stats.data_size > UINT32_MAX ||
        stats.table_size < 0) {
        free(payload);
        return N148I_ERROR_SIZE_OVERFLOW;
    }
#if LONG_MAX > UINT32_MAX
    if ((unsigned long) stats.data_size > UINT32_MAX) {
        free(payload);
        return N148I_ERROR_SIZE_OVERFLOW;
    }
#endif
    size_t table_size = (size_t) stats.table_size;
    size_t payload_size = (size_t) stats.data_size;
    if (table_size > SIZE_MAX - N148I_HEADER_SIZE ||
        payload_size > SIZE_MAX - N148I_HEADER_SIZE - table_size) {
        free(payload);
        return N148I_ERROR_SIZE_OVERFLOW;
    }
    size_t complete_size = N148I_HEADER_SIZE + table_size + payload_size;
    uint8_t *complete = (uint8_t *) malloc(complete_size);
    if (!complete) {
        free(payload);
        return N148I_ERROR_OUT_OF_MEMORY;
    }

    memcpy(complete, N148I_MAGIC, N148I_MAGIC_LEN);
    complete[5] = N148I_VERSION;
    memory_write_u32(complete + 6, image->width);
    memory_write_u32(complete + 10, image->height);
    complete[14] = (uint8_t) settings.quality;
    complete[15] = (uint8_t) settings.chroma;
    complete[16] = (uint8_t) settings.optimize_huffman;
    memory_write_u32(complete + 17, (uint32_t) stats.data_size);

    size_t offset = N148I_HEADER_SIZE;
    if (settings.optimize_huffman) {
        for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
            if (!huffman_spec_is_valid(&specs[table])) {
                free(payload);
                free(complete);
                return N148I_ERROR_ENCODE_FAILED;
            }
            for (int length = 1; length <= 16; length++)
                complete[offset++] = (uint8_t) specs[table].bits[length];
            memcpy(complete + offset, specs[table].values,
                   (size_t) specs[table].value_count);
            offset += (size_t) specs[table].value_count;
        }
    }
    if (offset != N148I_HEADER_SIZE + table_size) {
        free(payload);
        free(complete);
        return N148I_ERROR_ENCODE_FAILED;
    }
    memcpy(complete + offset, payload, payload_size);
    free(payload);

    *encoded = complete;
    *encoded_size = complete_size;
    return N148I_OK;
}

n148i_result_t n148i_decode_memory(const uint8_t *encoded,
                                   size_t encoded_size,
                                   n148i_image_t *image) {
    if (!image) return N148I_ERROR_INVALID_ARGUMENT;
    ParsedHeader parsed;
    n148i_result_t result = parse_header(encoded, encoded_size, &parsed);
    if (result != N148I_OK) return result;

    size_t width = parsed.width;
    size_t height = parsed.height;
    if (multiplication_overflows(width, 3) ||
        multiplication_overflows(width * 3, height) ||
        multiplication_overflows(width, height)) {
        return N148I_ERROR_SIZE_OVERFLOW;
    }
    size_t pixel_count = width * height;
#if LONG_MAX < SIZE_MAX
    if (pixel_count > (size_t) LONG_MAX)
        return N148I_ERROR_SIZE_OVERFLOW;
#endif

    Plane y = {0}, cb = {0}, cr = {0};
    DecodeStats stats;
    Image decoded = {0};
    int ok = decode_image((uint8_t *) encoded + parsed.payload_offset,
                          (long) parsed.payload_size,
                          (int) parsed.width, (int) parsed.height,
                          parsed.quality, parsed.chroma, parsed.specs,
                          &y, &cb, &cr, &stats);
    if (ok && stats.bytes_consumed != (long) parsed.payload_size)
        ok = 0;
    if (ok) ok = merge_channels(&y, &cb, &cr, 1, &decoded);
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    if (!ok) {
        free_image(&decoded);
        return N148I_ERROR_DECODE_FAILED;
    }
    if (decoded.width != (int) parsed.width ||
        decoded.height != (int) parsed.height || !decoded.pixels) {
        free_image(&decoded);
        return N148I_ERROR_CORRUPT_DATA;
    }

    image->pixels = decoded.pixels;
    image->width = parsed.width;
    image->height = parsed.height;
    image->stride = width * 3;
    return N148I_OK;
}

n148i_result_t n148i_read_header(const uint8_t *encoded,
                                 size_t encoded_size,
                                 n148i_image_info_t *info) {
    if (!info) return N148I_ERROR_INVALID_ARGUMENT;
    ParsedHeader parsed;
    n148i_result_t result = parse_header(encoded, encoded_size, &parsed);
    if (result != N148I_OK) return result;

    info->width = parsed.width;
    info->height = parsed.height;
    info->payload_size = parsed.payload_size;
    info->encoded_header_size = parsed.payload_offset;
    info->format_version = parsed.version;
    info->quality = parsed.quality;
    info->chroma = (n148i_chroma_t) parsed.chroma;
    info->optimized_huffman = parsed.optimized;
    return N148I_OK;
}

void n148i_free_buffer(void *buffer) {
    free(buffer);
}

void n148i_free_image(n148i_image_t *image) {
    if (!image) return;
    free(image->pixels);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;
    image->stride = 0;
}

const char *n148i_result_string(n148i_result_t result) {
    switch (result) {
        case N148I_OK: return "success";
        case N148I_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case N148I_ERROR_OUT_OF_MEMORY: return "out of memory";
        case N148I_ERROR_INVALID_FORMAT: return "invalid N.148i format";
        case N148I_ERROR_UNSUPPORTED_VERSION: return "unsupported format version";
        case N148I_ERROR_TRUNCATED_DATA: return "truncated data";
        case N148I_ERROR_CORRUPT_DATA: return "corrupt data";
        case N148I_ERROR_SIZE_OVERFLOW: return "image or buffer is too large";
        case N148I_ERROR_ENCODE_FAILED: return "encoding failed";
        case N148I_ERROR_DECODE_FAILED: return "decoding failed";
        case N148I_ERROR_UNSUPPORTED_SIMD: return "SIMD level is not supported";
        default: return "unknown N.148i error";
    }
}

const char *n148i_library_version(void) {
    return N148I_VERSION_STRING;
}

uint32_t n148i_format_version(void) {
    return N148I_FORMAT_VERSION;
}

n148i_simd_level_t n148i_simd_level(void) {
    return (n148i_simd_level_t) n148_cpu_level();
}

const char *n148i_simd_name(n148i_simd_level_t level) {
    switch (level) {
        case N148I_SIMD_AUTO: return "automatic";
        case N148I_SIMD_SCALAR: return "scalar";
        case N148I_SIMD_SSE2: return "SSE2";
        case N148I_SIMD_AVX2: return "AVX2";
        case N148I_SIMD_AVX2_FMA: return "AVX2+FMA";
        default: return "unknown";
    }
}

n148i_result_t n148i_simd_force(n148i_simd_level_t level) {
    if (level == N148I_SIMD_AUTO) {
        n148_cpu_force(-1);
        return N148I_OK;
    }
    if (level < N148I_SIMD_SCALAR || level > N148I_SIMD_AVX2_FMA)
        return N148I_ERROR_INVALID_ARGUMENT;
    if ((int) level > n148_cpu_detected_level())
        return N148I_ERROR_UNSUPPORTED_SIMD;
    n148_cpu_force((int) level);
    return N148I_OK;
}

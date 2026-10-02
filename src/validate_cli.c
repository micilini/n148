/*
 * N.148 codec validation driver.
 *
 * Copyright (c) Micilini Roll. Licensed under the MIT License.
 *
 * This executable verifies bitstream determinism across CPU paths and thread
 * counts, partial 8x8 blocks, and exact encoder/decoder byte consumption.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n148i.h"
#include "decoder.h"
#include "encoder.h"
#include "header.h"
#include "huffman.h"
#include "intra.h"
#include "loop_filter.h"
#include "ppm.h"
#include "cpu.h"
#include "parallel.h"
#include "perceptual.h"
#include "rans.h"
#include "rdo.h"
#include "tables.h"
#include "variable_transform.h"
#include "entropy_payload.h"
#include "predictive_codec.h"
#include "directional_intra.h"
#include "chroma_profile.h"
#include "entropy_profile.h"

#define EXPECTED_EXAMPLE_SIZE 2290L
#define ODD_WIDTH 317
#define ODD_HEIGHT 241

typedef struct {
    unsigned char *stream;
    long stream_size;
    long payload_size;
    long bytes_consumed;
    double psnr;
    int width;
    int height;
    int cpu_level;
    int threads;
    int chroma;
    char cpu_name[16];
    Image decoded;
} CycleResult;

static void release_cycle(CycleResult *result) {
    free(result->stream);
    result->stream = NULL;
    free_image(&result->decoded);
}

static double image_psnr(const Image *original, const Image *decoded) {
    long count = (long) original->width * original->height * 3;
    double squared_error = 0.0;

    for (long index = 0; index < count; index++) {
        double difference =
            (double) original->pixels[index] - decoded->pixels[index];
        squared_error += difference * difference;
    }

    if (squared_error == 0.0) return 999.0;
    return 10.0 * log10(255.0 * 255.0 /
                        (squared_error / (double) count));
}

static int run_cycle(const Image *original, int quality, int chroma,
                     int cpu_level, int threads, CycleResult *result) {
    Plane y = {0}, cb = {0}, cr = {0};
    Plane decoded_y = {0}, decoded_cb = {0}, decoded_cr = {0};
    HuffSpec encode_specs[HUFFMAN_TABLE_COUNT];
    HuffSpec decode_specs[HUFFMAN_TABLE_COUNT];
    EncodeStats encode_stats;
    DecodeStats decode_stats;
    unsigned char *payload = NULL;
    unsigned char *loaded_payload = NULL;
    FILE *file = NULL;
    int success = 0;

    memset(result, 0, sizeof(*result));
    n148_cpu_force(cpu_level);
    n148_set_thread_count(threads);
    result->cpu_level = n148_cpu_level();
    result->threads = n148_thread_count();
    result->chroma = chroma;
    snprintf(result->cpu_name, sizeof(result->cpu_name), "%s", n148_cpu_name());

    if (!split_channels((Image *) original, &y, &cb, &cr, chroma)) {
        fprintf(stderr, "split_channels failed for chroma %d\n", chroma);
        goto cleanup;
    }
    if (!encode_image(&y, &cb, &cr, quality, 1, encode_specs,
                      &payload, &encode_stats)) {
        fprintf(stderr, "encode_image failed for chroma %d\n", chroma);
        goto cleanup;
    }

    N148iHeader header;
    header.version = N148_FORMAT_VERSION_1;
    header.width = (uint32_t) original->width;
    header.height = (uint32_t) original->height;
    header.quality = (uint8_t) quality;
    header.chroma = (uint8_t) chroma;
    header.optimized = 1;
    header.data_size = (uint32_t) encode_stats.data_size;

    file = tmpfile();
    if (!file ||
        !write_header(file, &header) ||
        !write_huffman_tables(file, encode_specs) ||
        fwrite(payload, 1, (size_t) encode_stats.data_size, file) !=
            (size_t) encode_stats.data_size ||
        fflush(file) != 0 ||
        fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "could not serialize validation stream\n");
        goto cleanup;
    }

    result->stream_size = ftell(file);
    result->payload_size = encode_stats.data_size;
    long expected_size =
        N148_FORMAT_1_HEADER_SIZE + huffman_tables_size(encode_specs) +
        encode_stats.data_size;
    if (result->stream_size != expected_size || result->stream_size <= 0) {
        fprintf(stderr, "serialized size mismatch: %ld vs %ld\n",
                result->stream_size, expected_size);
        goto cleanup;
    }

    result->stream = (unsigned char *) malloc((size_t) result->stream_size);
    if (!result->stream ||
        fseek(file, 0, SEEK_SET) != 0 ||
        fread(result->stream, 1, (size_t) result->stream_size, file) !=
            (size_t) result->stream_size ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "could not read serialized validation stream\n");
        goto cleanup;
    }

    N148iHeader loaded;
    if (!read_header(file, &loaded) ||
        loaded.version != header.version ||
        loaded.width != header.width ||
        loaded.height != header.height ||
        loaded.quality != header.quality ||
        loaded.chroma != header.chroma ||
        loaded.optimized != header.optimized ||
        loaded.data_size != header.data_size ||
        !read_huffman_tables(file, decode_specs)) {
        fprintf(stderr, "serialized header or Huffman tables differ\n");
        goto cleanup;
    }

    loaded_payload = (unsigned char *) malloc(loaded.data_size);
    if (!loaded_payload ||
        fread(loaded_payload, 1, loaded.data_size, file) != loaded.data_size ||
        fgetc(file) != EOF) {
        fprintf(stderr, "serialized payload is truncated or has trailing bytes\n");
        goto cleanup;
    }

    if (!decode_image(loaded_payload, loaded.data_size,
                      original->width, original->height, quality, chroma,
                      decode_specs, &decoded_y, &decoded_cb, &decoded_cr,
                      &decode_stats) ||
        !merge_channels(&decoded_y, &decoded_cb, &decoded_cr, 1,
                        &result->decoded)) {
        fprintf(stderr, "decode cycle failed for chroma %d\n", chroma);
        goto cleanup;
    }

    result->bytes_consumed = decode_stats.bytes_consumed;
    result->width = result->decoded.width;
    result->height = result->decoded.height;
    result->psnr = image_psnr(original, &result->decoded);
    if (result->bytes_consumed != result->payload_size) {
        fprintf(stderr, "decoder consumed %ld of %ld bytes for chroma %d\n",
                result->bytes_consumed, result->payload_size, chroma);
        goto cleanup;
    }
    if (result->width != original->width ||
        result->height != original->height) {
        fprintf(stderr, "decoded dimensions changed for chroma %d\n", chroma);
        goto cleanup;
    }

    success = 1;

cleanup:
    if (file) fclose(file);
    free(payload);
    free(loaded_payload);
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    free_plane(&decoded_y);
    free_plane(&decoded_cb);
    free_plane(&decoded_cr);
    if (!success) release_cycle(result);
    return success;
}

static int cycles_identical(const CycleResult *left,
                            const CycleResult *right) {
    long pixel_count = (long) left->width * left->height * 3;
    return left->stream_size == right->stream_size &&
           memcmp(left->stream, right->stream,
                  (size_t) left->stream_size) == 0 &&
           left->width == right->width &&
           left->height == right->height &&
           memcmp(left->decoded.pixels, right->decoded.pixels,
                  (size_t) pixel_count) == 0 &&
           left->psnr == right->psnr;
}

static void print_cycle(const char *name, const CycleResult *result) {
    printf("cycle,%s,%s,%d,%d,%d,%d,%ld,%ld,%ld,%.12f,"
           "roundtrip_exact,PASS\n",
           name, result->cpu_name, result->threads, result->chroma,
           result->width, result->height, result->stream_size,
           result->payload_size, result->bytes_consumed, result->psnr);
}

static void fill_odd_image(Image *image) {
    image->width = ODD_WIDTH;
    image->height = ODD_HEIGHT;
    image->pixels = (unsigned char *) malloc(
        (size_t) image->width * image->height * 3);
    if (!image->pixels) return;

    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            long offset = ((long) y * image->width + x) * 3;
            image->pixels[offset + 0] =
                (unsigned char) ((13 * x + 3 * y + (x * y) % 251) & 255);
            image->pixels[offset + 1] =
                (unsigned char) ((5 * x + 17 * y + (x ^ y)) & 255);
            image->pixels[offset + 2] =
                (unsigned char) ((19 * x + 7 * y + (x * y) % 97) & 255);
        }
    }
}

/* Equal-luma red/blue-cyan checks isolate one-pixel chroma detail. A useful
   automatic selector must retain 4:4:4 for this case at high quality. */
static void fill_chroma_detail_image(Image *image) {
    image->width = 64;
    image->height = 64;
    image->pixels = (unsigned char *) malloc(
        (size_t) image->width * image->height * 3u);
    if (!image->pixels) return;

    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            size_t offset = ((size_t) y * image->width + x) * 3u;
            if ((x + y) & 1) {
                image->pixels[offset + 0] = 255;
                image->pixels[offset + 1] = 0;
                image->pixels[offset + 2] = 0;
            } else {
                image->pixels[offset + 0] = 0;
                image->pixels[offset + 1] = 82;
                image->pixels[offset + 2] = 255;
            }
        }
    }
}

static int validate_rans_core(void) {
    enum { SYMBOL_COUNT = 8192 };
    uint8_t *symbols = (uint8_t *) malloc(SYMBOL_COUNT);
    uint32_t counts[N148_RANS_ALPHABET_SIZE] = {0};
    N148RansModel model;
    N148RansDecoder decoder;
    uint8_t *stream = NULL;
    size_t stream_size = 0;
    int success = 0;
    if (!symbols) goto cleanup;

    /* Uneven and sparse on purpose: this exercises minimum-one frequency
       preservation as well as repeated renormalization. */
    for (int index = 0; index < SYMBOL_COUNT; index++) {
        uint8_t symbol;
        if (index % 997 == 0) symbol = 255;
        else if (index % 31 == 0) symbol = (uint8_t)(32 + index % 97);
        else if (index % 5 == 0) symbol = 1;
        else symbol = 0;
        symbols[index] = symbol;
        counts[symbol]++;
    }
    if (!n148_rans_build_model(counts, &model) ||
        !n148_rans_encode(symbols, SYMBOL_COUNT, &model,
                          &stream, &stream_size) ||
        !n148_rans_decoder_init(&decoder, &model, stream, stream_size)) {
        goto cleanup;
    }
    for (int index = 0; index < SYMBOL_COUNT; index++) {
        uint8_t decoded = 0;
        if (!n148_rans_decode_symbol(&decoder, &decoded) ||
            decoded != symbols[index]) {
            goto cleanup;
        }
    }
    success = n148_rans_decoder_finished(&decoder);

cleanup:
    printf("comparison,rans_core,scalar,1,0,0,0,%zu,%d,%d,0.0,"
           "normalized_roundtrip,%s\n",
           stream_size, SYMBOL_COUNT, SYMBOL_COUNT,
           success ? "PASS" : "FAIL");
    free(stream);
    free(symbols);
    return success;
}

/* Format 7 snapshots adaptive probabilities in forward symbol order before
   the rANS state is emitted in reverse.  Exercise three independent streams
   (standing in for the three planes) and compare the complete final model
   state on the encoder and decoder sides. */
static int validate_adaptive_rans_core(void) {
    enum { MODEL_COUNT = 3, PLANE_COUNT = 3, SYMBOLS_PER_PLANE = 2048 };
    uint8_t symbols[PLANE_COUNT][SYMBOLS_PER_PLANE];
    uint8_t indexes[PLANE_COUNT][SYMBOLS_PER_PLANE];
    uint32_t counts[MODEL_COUNT][N148_RANS_ALPHABET_SIZE] = {{0}};
    N148RansModel models[MODEL_COUNT];
    const uint8_t adaptive_flags[1] = {0x05u};
    size_t total_stream_size = 0;
    int success = 0;

    for (int plane = 0; plane < PLANE_COUNT; plane++) {
        for (int index = 0; index < SYMBOLS_PER_PLANE; index++) {
            uint8_t model = (uint8_t)((index + 2 * plane) % MODEL_COUNT);
            uint8_t symbol = (uint8_t)(
                ((index / (7 + plane)) + 3 * plane + model) & 7);
            indexes[plane][index] = model;
            symbols[plane][index] = symbol;
            counts[model][symbol]++;
        }
    }
    for (int model = 0; model < MODEL_COUNT; model++)
        if (!n148_rans_build_model(counts[model], &models[model]))
            goto cleanup;

    for (int plane = 0; plane < PLANE_COUNT; plane++) {
        uint8_t *stream = NULL;
        size_t stream_size = 0;
        uint64_t encoder_hash = 0;
        N148RansAdaptiveModel decoder_models[MODEL_COUNT];
        N148RansDecoder decoder;
        int plane_ok = n148_rans_encode_mixed_adaptive(
            symbols[plane], indexes[plane], SYMBOLS_PER_PLANE,
            models, MODEL_COUNT, MODEL_COUNT, adaptive_flags, MODEL_COUNT,
            N148_ENTROPY_ADAPT_RATE_SHIFT, &stream, &stream_size,
            &encoder_hash) &&
            n148_rans_decoder_init_mixed_adaptive(
                &decoder, stream, stream_size, models, decoder_models,
                MODEL_COUNT, MODEL_COUNT, adaptive_flags, MODEL_COUNT,
                N148_ENTROPY_ADAPT_RATE_SHIFT);
        for (int index = 0; index < SYMBOLS_PER_PLANE && plane_ok; index++) {
            uint8_t decoded = 0;
            plane_ok = n148_rans_decode_symbol_with_model(
                &decoder, &models[indexes[plane][index]], &decoded) &&
                decoded == symbols[plane][index];
        }
        uint64_t decoder_hash = plane_ok ?
            n148_rans_adaptive_models_hash(decoder_models, MODEL_COUNT) : 0;
        plane_ok = plane_ok && n148_rans_decoder_finished(&decoder) &&
            encoder_hash != 0 && encoder_hash == decoder_hash;
        total_stream_size += stream_size;
        free(stream);
        if (!plane_ok) goto cleanup;
    }
    success = 1;

cleanup:
    printf("comparison,rans_adaptive_state,scalar,1,0,0,0,%zu,%d,%d,0.0,"
           "three_plane_integer_model_state_match,%s\n",
           total_stream_size, PLANE_COUNT * SYMBOLS_PER_PLANE,
           PLANE_COUNT * SYMBOLS_PER_PLANE, success ? "PASS" : "FAIL");
    return success;
}

static int validate_public_api(const Image *original,
                               const CycleResult *reference) {
    n148i_image_t input = {
        original->pixels,
        (uint32_t) original->width,
        (uint32_t) original->height,
        (size_t) original->width * 3
    };
    n148i_encode_options_t options;
    uint8_t *encoded = NULL;
    uint8_t *strided_encoded = NULL;
    uint8_t *strided_pixels = NULL;
    size_t encoded_size = 0;
    size_t strided_size = 0;
    n148i_image_t decoded = {0};
    n148i_image_info_t info = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.optimize_huffman = 1;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_1;
    options.feature_flags = 0;

    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&input, &options, &encoded, &encoded_size) !=
            N148I_OK ||
        encoded_size != (size_t) reference->stream_size ||
        memcmp(encoded, reference->stream, encoded_size) != 0 ||
        n148i_read_header(encoded, encoded_size, &info) != N148I_OK ||
        info.width != (uint32_t) original->width ||
        info.height != (uint32_t) original->height ||
        info.format_version != N148I_FORMAT_VERSION_1 ||
        info.quality != 50 || info.chroma != N148I_CHROMA_420 ||
        !info.optimized_huffman ||
        info.encoded_header_size + info.payload_size != encoded_size ||
        n148i_decode_memory(encoded, encoded_size, &decoded) != N148I_OK ||
        decoded.width != (uint32_t) reference->width ||
        decoded.height != (uint32_t) reference->height ||
        decoded.stride != (size_t) reference->width * 3 ||
        memcmp(decoded.pixels, reference->decoded.pixels,
               decoded.stride * decoded.height) != 0) {
        goto cleanup;
    }

    size_t tight_stride = (size_t) original->width * 3;
    size_t padded_stride = tight_stride + 7;
    strided_pixels = (uint8_t *) malloc(
        padded_stride * (size_t) original->height);
    if (!strided_pixels) goto cleanup;
    memset(strided_pixels, 0xa5, padded_stride * (size_t) original->height);
    for (int row = 0; row < original->height; row++) {
        memcpy(strided_pixels + (size_t) row * padded_stride,
               original->pixels + (size_t) row * tight_stride,
               tight_stride);
    }
    input.pixels = strided_pixels;
    input.stride = padded_stride;
    if (n148i_encode_memory(&input, &options, &strided_encoded,
                            &strided_size) != N148I_OK ||
        strided_size != encoded_size ||
        memcmp(strided_encoded, encoded, encoded_size) != 0 ||
        n148i_read_header(encoded, N148_FORMAT_1_HEADER_SIZE - 1, &info) !=
            N148I_ERROR_TRUNCATED_DATA) {
        goto cleanup;
    }
    success = 1;

cleanup:
    printf("comparison,public_api_vs_internal,scalar,1,2,%d,%d,%zu,%u,%u,"
           "%.12f,memory_stride_and_bitstream_identical,%s\n",
           original->width, original->height, encoded_size,
           info.payload_size, info.payload_size, reference->psnr,
           success ? "PASS" : "FAIL");
    free(strided_pixels);
    n148i_free_buffer(strided_encoded);
    n148i_free_image(&decoded);
    n148i_free_buffer(encoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    return success;
}

static uint32_t validation_u32(const uint8_t *data) {
    return (uint32_t) data[0] | ((uint32_t) data[1] << 8) |
        ((uint32_t) data[2] << 16) | ((uint32_t) data[3] << 24);
}

static void validation_put_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t) value;
    data[1] = (uint8_t) (value >> 8);
    data[2] = (uint8_t) (value >> 16);
    data[3] = (uint8_t) (value >> 24);
}

static int validate_rans_entropy(const Image *original,
                               const CycleResult *format_1_reference) {
    n148i_image_t source = {
        original->pixels,
        (uint32_t) original->width,
        (uint32_t) original->height,
        (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *scalar = NULL, *vector = NULL, *parallel = NULL, *huffman = NULL;
    size_t scalar_size = 0, vector_size = 0, parallel_size = 0, huffman_size = 0;
    n148i_image_t decoded = {0}, huffman_decoded = {0};
    n148i_image_info_t info = {0};
    Plane y = {0}, cb = {0}, cr = {0};
    N148CoeffPlane expected[3] = {{0}}, recovered[3] = {{0}};
    int success = 0;

    n148i_encode_options_init(&options);
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_2;
    options.feature_flags = N148I_FEATURE_RANS;

    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &scalar, &scalar_size) != N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_2 ||
        info.feature_flags != N148I_FEATURE_RANS || info.effort != 3 ||
        info.encoded_header_size !=
            N148_EXTENDED_HEADER_SIZE + info.metadata_size ||
        info.encoded_header_size + info.payload_size != scalar_size ||
        scalar_size >= (size_t) format_1_reference->stream_size ||
        n148i_decode_memory(scalar, scalar_size, &decoded) != N148I_OK ||
        decoded.width != (uint32_t) format_1_reference->width ||
        decoded.height != (uint32_t) format_1_reference->height ||
        memcmp(decoded.pixels, format_1_reference->decoded.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) != N148I_OK ||
        vector_size != scalar_size || memcmp(vector, scalar, scalar_size) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) != N148I_OK ||
        parallel_size != scalar_size || memcmp(parallel, scalar, scalar_size) != 0)
        goto cleanup;

    n148_cpu_force(N148_CPU_BASELINE);
    n148_set_thread_count(1);
    if (!split_channels((Image *) original, &y, &cb, &cr, CHROMA_420) ||
        !n148_quantize_planes(&y, &cb, &cr, 50, expected)) goto cleanup;
    uint16_t header_size = (uint16_t)(scalar[6] | ((uint16_t) scalar[7] << 8));
    uint32_t metadata_size = validation_u32(scalar + 28);
    uint32_t payload_size = validation_u32(scalar + 16);
    if (header_size != N148_EXTENDED_HEADER_SIZE + metadata_size ||
        !n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            scalar + header_size, payload_size,
            original->width, original->height, CHROMA_420,
            N148I_FEATURE_RANS, recovered, NULL, NULL, NULL, NULL))
        goto cleanup;
    for (int plane = 0; plane < 3; plane++) {
        if (expected[plane].count != recovered[plane].count ||
            memcmp(expected[plane].coefficients, recovered[plane].coefficients,
                   (size_t) expected[plane].count * 64 * sizeof(short)) != 0)
            goto cleanup;
    }

    options.feature_flags = 0;
    if (n148i_encode_memory(&source, &options, &huffman, &huffman_size) != N148I_OK ||
        n148i_read_header(huffman, huffman_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_2 ||
        info.feature_flags != 0 ||
        n148i_decode_memory(huffman, huffman_size, &huffman_decoded) != N148I_OK ||
        memcmp(huffman_decoded.pixels, format_1_reference->decoded.pixels,
               huffman_decoded.stride * huffman_decoded.height) != 0 ||
        n148i_decode_memory(scalar, scalar_size - 1, &huffman_decoded) !=
            N148I_ERROR_TRUNCATED_DATA) goto cleanup;

    success = 1;

cleanup:
    printf("comparison,rans_vs_format_1_coefficients,scalar+AVX2,1+4,2,%d,%d,"
           "%zu,%u,%u,%.12f,coefficients_pixels_determinism_and_v1_decode,%s\n",
           original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, format_1_reference->psnr,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(scalar);
    n148i_free_buffer(vector);
    n148i_free_buffer(parallel);
    n148i_free_buffer(huffman);
    n148i_free_image(&decoded);
    n148i_free_image(&huffman_decoded);
    free_plane(&y); free_plane(&cb); free_plane(&cr);
    for (int plane = 0; plane < 3; plane++) {
        n148_free_coeff_plane(&expected[plane]);
        n148_free_coeff_plane(&recovered[plane]);
    }
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_context_modeling(const Image *original,
                               const CycleResult *format_1_reference) {
    n148i_image_t source = {
        original->pixels,
        (uint32_t) original->width,
        (uint32_t) original->height,
        (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *rans_only = NULL, *scalar = NULL, *vector = NULL, *parallel = NULL;
    size_t rans_only_size = 0, scalar_size = 0, vector_size = 0, parallel_size = 0;
    n148i_image_t decoded = {0};
    n148i_image_info_t info = {0};
    Plane y = {0}, cb = {0}, cr = {0};
    N148CoeffPlane expected[3] = {{0}}, recovered[3] = {{0}};
    N148ContextTrace encoder_trace = {0}, decoder_trace = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_2;
    options.feature_flags = N148I_FEATURE_RANS;

    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &rans_only, &rans_only_size) !=
            N148I_OK) goto cleanup;
    options.feature_flags |= N148I_FEATURE_CONTEXT;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK || scalar_size >= rans_only_size ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags !=
            (N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT) ||
        n148i_decode_memory(scalar, scalar_size, &decoded) != N148I_OK ||
        memcmp(decoded.pixels, format_1_reference->decoded.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;

    n148_cpu_force(N148_CPU_BASELINE);
    n148_set_thread_count(1);
    if (!split_channels((Image *) original, &y, &cb, &cr, CHROMA_420) ||
        !n148_quantize_planes(&y, &cb, &cr, 50, expected) ||
        !n148_trace_base_contexts(expected, original->width, original->height,
                                CHROMA_420,
                                N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT,
                                &encoder_trace)) goto cleanup;
    uint16_t header_size = (uint16_t)(scalar[6] | ((uint16_t) scalar[7] << 8));
    uint32_t metadata_size = validation_u32(scalar + 28);
    uint32_t payload_size = validation_u32(scalar + 16);
    if (!n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            scalar + header_size, payload_size,
            original->width, original->height, CHROMA_420,
            N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT,
            recovered, &decoder_trace, NULL, NULL, NULL) ||
        encoder_trace.symbol_count != decoder_trace.symbol_count ||
        encoder_trace.hash != decoder_trace.hash) goto cleanup;
    for (int plane = 0; plane < 3; plane++) {
        if (expected[plane].count != recovered[plane].count ||
            memcmp(expected[plane].coefficients, recovered[plane].coefficients,
                   (size_t) expected[plane].count * 64 * sizeof(short)) != 0)
            goto cleanup;
    }

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0) goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,context_model_trace,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,"
           "%.12f,encoder_decoder_model_hash_and_coefficients,%s\n",
           original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, format_1_reference->psnr,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(rans_only);
    n148i_free_buffer(scalar);
    n148i_free_buffer(vector);
    n148i_free_buffer(parallel);
    n148i_free_image(&decoded);
    free_plane(&y); free_plane(&cb); free_plane(&cr);
    for (int plane = 0; plane < 3; plane++) {
        n148_free_coeff_plane(&expected[plane]);
        n148_free_coeff_plane(&recovered[plane]);
    }
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int uniform_plane(const Plane *plane) {
    if (!plane || !plane->data || plane->width <= 0 || plane->height <= 0)
        return 0;
    size_t count = (size_t) plane->width * (size_t) plane->height;
    for (size_t index = 1; index < count; index++)
        if (plane->data[index] != plane->data[0]) return 0;
    return 1;
}

static int validate_intra_uniform_drift(void) {
    enum { WIDTH = 257, HEIGHT = 193 };
    Plane source_y = create_plane(WIDTH, HEIGHT);
    Plane source_cb = create_plane(WIDTH, HEIGHT);
    Plane source_cr = create_plane(WIDTH, HEIGHT);
    Plane decoded_y = {0}, decoded_cb = {0}, decoded_cr = {0};
    N148CoeffPlane coefficients[3] = {{0}};
    uint8_t *modes = NULL;
    size_t mode_count = 0;
    int success = 0;
    if (!source_y.data || !source_cb.data || !source_cr.data) goto cleanup;
    memset(source_y.data, 173, (size_t) WIDTH * HEIGHT);
    memset(source_cb.data, 128, (size_t) WIDTH * HEIGHT);
    memset(source_cr.data, 128, (size_t) WIDTH * HEIGHT);
    if (!n148_intra_quantize_planes(&source_y, &source_cb, &source_cr, 30, 0,
                                    NULL, NULL, 0, 0, coefficients, &modes,
                                    &mode_count) ||
        !n148_intra_reconstruct_planes(coefficients, modes, mode_count,
                                       WIDTH, HEIGHT, 30, CHROMA_444, 0, NULL,
                                       NULL, 0,
                                       &decoded_y, &decoded_cb, &decoded_cr) ||
        !uniform_plane(&decoded_y) || !uniform_plane(&decoded_cb) ||
        !uniform_plane(&decoded_cr)) goto cleanup;
    success = 1;

cleanup:
    free_plane(&source_y); free_plane(&source_cb); free_plane(&source_cr);
    free_plane(&decoded_y); free_plane(&decoded_cb); free_plane(&decoded_cr);
    for (int plane = 0; plane < 3; plane++)
        n148_free_coeff_plane(&coefficients[plane]);
    free(modes);
    return success;
}

static int validate_reconstructed_intra(const Image *original) {
    n148i_image_t source = {
        original->pixels,
        (uint32_t) original->width,
        (uint32_t) original->height,
        (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *context_only = NULL, *scalar = NULL, *vector = NULL, *parallel = NULL;
    size_t context_only_size = 0, scalar_size = 0, vector_size = 0, parallel_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0};
    n148i_image_info_t info = {0};
    Plane source_y = {0}, source_cb = {0}, source_cr = {0};
    Plane expected_y = {0}, expected_cb = {0}, expected_cr = {0};
    Image expected_rgb = {0};
    N148CoeffPlane expected[3] = {{0}}, recovered[3] = {{0}};
    N148ContextTrace encoder_trace = {0}, decoder_trace = {0};
    N148PredictionModes decoded_modes = {0};
    uint8_t *expected_modes = NULL;
    size_t expected_mode_count = 0;
    size_t mode_histogram[N148_INTRA_MODE_COUNT] = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_2;
    options.feature_flags = N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &context_only, &context_only_size) !=
            N148I_OK) goto cleanup;
    options.feature_flags |= N148I_FEATURE_INTRA;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK || scalar_size >= context_only_size ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != (N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT |
                               N148I_FEATURE_INTRA) ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK)
        goto cleanup;

    n148_cpu_force(N148_CPU_BASELINE);
    n148_set_thread_count(1);
    if (!split_channels((Image *) original, &source_y, &source_cb, &source_cr,
                        CHROMA_420) ||
        !n148_intra_quantize_planes(&source_y, &source_cb, &source_cr, 50, 0,
                                    NULL, NULL, 0, 0, expected,
                                    &expected_modes,
                                    &expected_mode_count) ||
        !n148_trace_base_contexts(expected, original->width, original->height,
                                CHROMA_420,
                                N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT |
                                    N148I_FEATURE_INTRA,
                                &encoder_trace)) goto cleanup;
    uint16_t header_size = (uint16_t)(scalar[6] | ((uint16_t) scalar[7] << 8));
    uint32_t metadata_size = validation_u32(scalar + 28);
    uint32_t payload_size = validation_u32(scalar + 16);
    if (!n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            scalar + header_size, payload_size,
            original->width, original->height, CHROMA_420,
            N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT | N148I_FEATURE_INTRA,
            recovered, &decoder_trace, &decoded_modes, NULL, NULL) ||
        encoder_trace.symbol_count != decoder_trace.symbol_count ||
        encoder_trace.hash != decoder_trace.hash ||
        decoded_modes.count != expected_mode_count ||
        memcmp(decoded_modes.values, expected_modes, expected_mode_count) != 0)
        goto cleanup;
    for (int plane = 0; plane < 3; plane++) {
        if (expected[plane].count != recovered[plane].count ||
            memcmp(expected[plane].coefficients, recovered[plane].coefficients,
                   (size_t) expected[plane].count * 64 * sizeof(short)) != 0)
            goto cleanup;
    }
    for (size_t index = 0; index < expected_mode_count; index++) {
        if (expected_modes[index] >= N148_INTRA_MODE_COUNT) goto cleanup;
        mode_histogram[expected_modes[index]]++;
    }
    for (int mode = 0; mode < N148_INTRA_MODE_COUNT; mode++)
        if (mode_histogram[mode] == 0) goto cleanup;
    if (!n148_intra_reconstruct_planes(
            recovered, decoded_modes.values, decoded_modes.count,
            original->width, original->height, 50, CHROMA_420, 0, NULL, NULL,
            0, &expected_y, &expected_cb, &expected_cr) ||
        !merge_channels(&expected_y, &expected_cb, &expected_cr, 1,
                        &expected_rgb) ||
        scalar_decoded.width != (uint32_t) expected_rgb.width ||
        scalar_decoded.height != (uint32_t) expected_rgb.height ||
        memcmp(scalar_decoded.pixels, expected_rgb.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0 ||
        !validate_intra_uniform_drift()) goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    success = 1;

cleanup:
    printf("comparison,reconstructed_intra_prediction,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,"
           "0.0,modes_%zu_%zu_%zu_%zu_coefficients_and_uniform_drift,%s\n",
           original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           mode_histogram[0], mode_histogram[1], mode_histogram[2],
           mode_histogram[3], success ? "PASS" : "FAIL");
    n148i_free_buffer(context_only); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded);
    free_plane(&source_y); free_plane(&source_cb); free_plane(&source_cr);
    free_plane(&expected_y); free_plane(&expected_cb); free_plane(&expected_cr);
    free_image(&expected_rgb);
    for (int plane = 0; plane < 3; plane++) {
        n148_free_coeff_plane(&expected[plane]);
        n148_free_coeff_plane(&recovered[plane]);
    }
    free(expected_modes);
    n148_prediction_modes_release(&decoded_modes);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_perceptual_transform(void) {
    enum { LEVELS = 16, SAMPLE_COUNT = LEVELS * LEVELS * LEVELS };
    uint8_t *rgb = (uint8_t *) malloc(SAMPLE_COUNT * 3u);
    uint8_t *intensity = (uint8_t *) malloc(SAMPLE_COUNT);
    uint8_t *red_green = (uint8_t *) malloc(SAMPLE_COUNT);
    uint8_t *blue_yellow = (uint8_t *) malloc(SAMPLE_COUNT);
    int max_error = 0;
    uint8_t max_source[3] = {0}, max_reconstructed[3] = {0};
    double squared_error = 0.0;
    int success = 0;
    if (!rgb || !intensity || !red_green || !blue_yellow) goto cleanup;

    int sample = 0;
    for (int red = 0; red < LEVELS; red++) {
        for (int green = 0; green < LEVELS; green++) {
            for (int blue = 0; blue < LEVELS; blue++) {
                rgb[sample * 3] = (uint8_t)(red * 17);
                rgb[sample * 3 + 1] = (uint8_t)(green * 17);
                rgb[sample * 3 + 2] = (uint8_t)(blue * 17);
                sample++;
            }
        }
    }
    n148_perceptual_init();
    n148_rgb_to_perceptual(rgb, SAMPLE_COUNT, intensity, red_green,
                           blue_yellow);
    for (int index = 0; index < SAMPLE_COUNT; index++) {
        uint8_t reconstructed[3];
        n148_perceptual_to_rgb(
            intensity[index], (uint16_t) red_green[index] << 4,
            (uint16_t) blue_yellow[index] << 4, reconstructed);
        for (int channel = 0; channel < 3; channel++) {
            int error = (int) rgb[index * 3 + channel] -
                reconstructed[channel];
            int magnitude = error < 0 ? -error : error;
            if (magnitude > max_error) {
                max_error = magnitude;
                memcpy(max_source, rgb + index * 3, sizeof(max_source));
                memcpy(max_reconstructed, reconstructed,
                       sizeof(max_reconstructed));
            }
            squared_error += (double) error * error;
        }
    }
    double mse = squared_error / (SAMPLE_COUNT * 3.0);
    double psnr = mse == 0.0 ? 999.0 :
        10.0 * log10(255.0 * 255.0 / mse);
    /* The transform is lossy at its intentional 8-bit plane boundary. These
       limits freeze the measured conversion floor instead of pretending the
       opponent mapping is mathematically lossless. */
    success = max_error <= 16 && psnr >= 45.0;
    printf("comparison,perceptual_color_transform,scalar,1,0,16,16,0,0,0,"
           "%.12f,max_channel_error_%d_at_%u_%u_%u_to_%u_%u_%u_over_%d_"
           "rgb_samples,%s\n",
           psnr, max_error, max_source[0], max_source[1], max_source[2],
           max_reconstructed[0], max_reconstructed[1], max_reconstructed[2],
           SAMPLE_COUNT, success ? "PASS" : "FAIL");

cleanup:
    free(rgb);
    free(intensity);
    free(red_green);
    free(blue_yellow);
    return success;
}

static int validate_perceptual_color(const Image *original) {
    n148i_image_t source = {
        original->pixels,
        (uint32_t) original->width,
        (uint32_t) original->height,
        (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *scalar = NULL, *vector = NULL, *parallel = NULL;
    size_t scalar_size = 0, vector_size = 0, parallel_size = 0;
    n148i_image_t decoded = {0};
    n148i_image_info_t info = {0};
    size_t reported_size = 0;
    uint32_t features = N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT |
        N148I_FEATURE_INTRA | N148I_FEATURE_PERCEPTUAL_COLOR;
    int success = 0;
    if (!validate_perceptual_transform()) goto cleanup;

    n148i_encode_options_init(&options);
    if (options.format_version == N148I_FORMAT_VERSION_2 &&
        (options.feature_flags & N148I_FEATURE_PERCEPTUAL_COLOR)) goto cleanup;
    options.quality = 50;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_2;
    options.feature_flags = N148I_FEATURE_PERCEPTUAL_COLOR;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    options.feature_flags = features;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420;
         chroma++) {
        options.chroma = (n148i_chroma_t) chroma;
        options.thread_count = 1;
        if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
            n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
                N148I_OK ||
            n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
            info.feature_flags != features || (int) info.chroma != chroma ||
            n148i_decode_memory(scalar, scalar_size, &decoded) != N148I_OK ||
            decoded.width != (uint32_t) original->width ||
            decoded.height != (uint32_t) original->height) goto cleanup;
        n148i_free_image(&decoded);

        if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
            n148i_encode_memory(&source, &options, &vector, &vector_size) !=
                N148I_OK || vector_size != scalar_size ||
            memcmp(vector, scalar, scalar_size) != 0) goto cleanup;

        options.thread_count = 4;
        if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
            n148i_encode_memory(&source, &options, &parallel,
                                &parallel_size) != N148I_OK ||
            parallel_size != scalar_size ||
            memcmp(parallel, scalar, scalar_size) != 0) goto cleanup;
        if (chroma == N148I_CHROMA_420) reported_size = scalar_size;
        n148i_free_buffer(scalar); scalar = NULL;
        n148i_free_buffer(vector); vector = NULL;
        n148i_free_buffer(parallel); parallel = NULL;
    }

    const uint32_t independent_masks[] = {
        N148I_FEATURE_RANS | N148I_FEATURE_PERCEPTUAL_COLOR,
        N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT |
            N148I_FEATURE_PERCEPTUAL_COLOR,
        N148I_FEATURE_RANS | N148I_FEATURE_INTRA |
            N148I_FEATURE_PERCEPTUAL_COLOR,
    };
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    for (size_t index = 0;
         index < sizeof(independent_masks) / sizeof(independent_masks[0]);
         index++) {
        options.feature_flags = independent_masks[index];
        if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
            n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
                N148I_OK ||
            n148i_decode_memory(scalar, scalar_size, &decoded) != N148I_OK)
            goto cleanup;
        n148i_free_buffer(scalar); scalar = NULL;
        n148i_free_image(&decoded);
    }
    success = 1;

cleanup:
    printf("comparison,perceptual_color,scalar+AVX2,1+4,0+1+2,%d,%d,%zu,"
           "%u,%u,0.0,all_feature_combinations_rgb_roundtrip_and_"
           "determinism,%s\n",
           original->width, original->height, reported_size,
           info.payload_size, info.payload_size, success ? "PASS" : "FAIL");
    n148i_free_buffer(scalar);
    n148i_free_buffer(vector);
    n148i_free_buffer(parallel);
    n148i_free_image(&decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    return success;
}

static int validate_adaptive_chroma(const Image *original, int chroma,
                                    uint32_t features,
                                    size_t *reported_size,
                                    size_t level_histogram[4]) {
    int perceptual_color =
        (features & N148I_FEATURE_PERCEPTUAL_COLOR) != 0;
    n148i_image_t source = {
        original->pixels,
        (uint32_t) original->width,
        (uint32_t) original->height,
        (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *scalar = NULL, *vector = NULL, *parallel = NULL;
    uint8_t *corrupt_payload = NULL;
    size_t scalar_size = 0, vector_size = 0, parallel_size = 0;
    n148i_image_t decoded = {0}, vector_decoded = {0}, parallel_decoded = {0};
    n148i_image_info_t info = {0};
    Plane source_y = {0}, source_cb = {0}, source_cr = {0};
    Plane expected_y = {0}, expected_cb = {0}, expected_cr = {0};
    Image expected_rgb = {0};
    N148CoeffPlane expected[3] = {{0}}, recovered[3] = {{0}};
    N148CoeffPlane rejected[3] = {{0}};
    N148PredictionModes decoded_modes = {0};
    N148AdaptiveMap expected_map = {0}, decoded_map = {0};
    uint8_t *expected_modes = NULL;
    size_t expected_mode_count = 0;
    int success = 0;

    n148i_encode_options_init(&options);
    options.quality = 50;
    options.chroma = (n148i_chroma_t) chroma;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_2;
    options.feature_flags = features;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != features || (int) info.chroma != chroma ||
        n148i_decode_memory(scalar, scalar_size, &decoded) != N148I_OK)
        goto cleanup;

    n148_cpu_force(N148_CPU_BASELINE);
    n148_set_thread_count(1);
    if (!n148_split_channels_ex(
            (Image *) original, &source_y, &source_cb, &source_cr, chroma,
            perceptual_color) ||
        !n148_adaptive_map_build(&source_y, &expected_map) ||
        !n148_intra_quantize_planes(
            &source_y, &source_cb, &source_cr, 50, perceptual_color,
            &expected_map, NULL, 0, 0,
            expected, &expected_modes, &expected_mode_count)) goto cleanup;

    uint16_t header_size =
        (uint16_t)(scalar[6] | ((uint16_t) scalar[7] << 8));
    uint32_t metadata_size = validation_u32(scalar + 28);
    uint32_t payload_size = validation_u32(scalar + 16);
    const uint8_t *payload = scalar + header_size;
    if (payload_size < 24 || payload[0] != 3 || payload[1] != 3 ||
        payload[2] != 1 || payload[3] != 1 ||
        validation_u32(payload + 20) != expected_map.count ||
        !n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            payload, payload_size, original->width, original->height, chroma,
            features, recovered, NULL, &decoded_modes, &decoded_map, NULL) ||
        decoded_modes.count != expected_mode_count ||
        memcmp(decoded_modes.values, expected_modes,
               expected_mode_count) != 0 ||
        decoded_map.count != expected_map.count ||
        decoded_map.columns != expected_map.columns ||
        decoded_map.rows != expected_map.rows ||
        memcmp(decoded_map.levels, expected_map.levels,
               expected_map.count) != 0) goto cleanup;

    for (int plane = 0; plane < 3; plane++) {
        if (expected[plane].count != recovered[plane].count ||
            memcmp(expected[plane].coefficients, recovered[plane].coefficients,
                   (size_t) expected[plane].count * 64 * sizeof(short)) != 0)
            goto cleanup;
    }
    if (!n148_intra_reconstruct_planes(
            recovered, decoded_modes.values, decoded_modes.count,
            original->width, original->height, 50, chroma, perceptual_color,
            &decoded_map, NULL, 0,
            &expected_y, &expected_cb, &expected_cr) ||
        !n148_merge_channels_ex(&expected_y, &expected_cb, &expected_cr, 1,
                                perceptual_color, &expected_rgb) ||
        decoded.width != (uint32_t) expected_rgb.width ||
        decoded.height != (uint32_t) expected_rgb.height ||
        memcmp(decoded.pixels, expected_rgb.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;

    for (size_t index = 0; index < expected_map.count; index++) {
        if (expected_map.levels[index] >= N148_AQ_LEVEL_COUNT) goto cleanup;
        level_histogram[expected_map.levels[index]]++;
    }

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, decoded.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, decoded.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;

    corrupt_payload = (uint8_t *) malloc(payload_size);
    if (!corrupt_payload) goto cleanup;
    memcpy(corrupt_payload, payload, payload_size);
    corrupt_payload[20] ^= 1u;
    if (n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            corrupt_payload, payload_size, original->width, original->height,
            chroma, features, rejected, NULL, NULL, NULL, NULL)) goto cleanup;

    if (reported_size) *reported_size = scalar_size;
    success = 1;

cleanup:
    n148i_free_buffer(scalar); n148i_free_buffer(vector);
    n148i_free_buffer(parallel);
    n148i_free_image(&decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded);
    free(corrupt_payload);
    free_plane(&source_y); free_plane(&source_cb); free_plane(&source_cr);
    free_plane(&expected_y); free_plane(&expected_cb); free_plane(&expected_cr);
    free_image(&expected_rgb);
    free(expected_modes);
    n148_prediction_modes_release(&decoded_modes);
    n148_adaptive_map_release(&expected_map);
    n148_adaptive_map_release(&decoded_map);
    for (int plane = 0; plane < 3; plane++) {
        n148_free_coeff_plane(&expected[plane]);
        n148_free_coeff_plane(&recovered[plane]);
        n148_free_coeff_plane(&rejected[plane]);
    }
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_adaptive_quantization(const Image *original) {
    const uint32_t features = N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT |
        N148I_FEATURE_INTRA | N148I_FEATURE_ADAPTIVE_QUANT;
    n148i_image_t source = {
        original->pixels,
        (uint32_t) original->width,
        (uint32_t) original->height,
        (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    size_t reported_size = 0;
    size_t level_histogram[N148_AQ_LEVEL_COUNT] = {0};
    size_t combined_histogram[N148_AQ_LEVEL_COUNT] = {0};
    Image level_fixture = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_2;
    options.feature_flags = N148I_FEATURE_ADAPTIVE_QUANT;
    if (n148i_encode_memory(&source, &options, &encoded, &encoded_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_FEATURE_RANS | N148I_FEATURE_ADAPTIVE_QUANT;
    if (n148i_encode_memory(&source, &options, &encoded, &encoded_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420;
         chroma++) {
        size_t size = 0;
        if (!validate_adaptive_chroma(original, chroma, features, &size,
                                      level_histogram)) goto cleanup;
        if (chroma == N148I_CHROMA_420) reported_size = size;
    }
    level_fixture.width = 64;
    level_fixture.height = 16;
    level_fixture.pixels = (uint8_t *) malloc(64u * 16u * 3u);
    if (!level_fixture.pixels) goto cleanup;
    const int amplitudes[N148_AQ_LEVEL_COUNT] = {0, 1, 3, 10};
    for (int y = 0; y < level_fixture.height; y++) {
        for (int x = 0; x < level_fixture.width; x++) {
            int amplitude = amplitudes[x / N148_AQ_REGION_SIZE];
            int value = 128 + (((x + y) & 1) ? amplitude : -amplitude);
            size_t pixel = ((size_t) y * 64u + (size_t) x) * 3u;
            level_fixture.pixels[pixel] = (uint8_t) value;
            level_fixture.pixels[pixel + 1] = (uint8_t) value;
            level_fixture.pixels[pixel + 2] = (uint8_t) value;
        }
    }
    if (!validate_adaptive_chroma(&level_fixture, N148I_CHROMA_420, features,
                                  NULL, level_histogram)) goto cleanup;
    for (int level = 0; level < N148_AQ_LEVEL_COUNT; level++)
        if (level_histogram[level] == 0) goto cleanup;
    const uint32_t combined_features =
        features | N148I_FEATURE_PERCEPTUAL_COLOR;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420;
         chroma++) {
        if (!validate_adaptive_chroma(original, chroma, combined_features,
                                      NULL, combined_histogram)) goto cleanup;
    }
    success = 1;

cleanup:
    printf("comparison,adaptive_quantization,scalar+AVX2,1+4,0+1+2,%d,%d,%zu,"
           "0,0,0.0,levels_%zu_%zu_%zu_%zu_map_coefficients_corruption_and_"
           "perceptual_combination_determinism,%s\n",
           original->width, original->height, reported_size,
           level_histogram[0], level_histogram[1], level_histogram[2],
           level_histogram[3], success ? "PASS" : "FAIL");
    n148i_free_buffer(encoded);
    free_image(&level_fixture);
    n148i_simd_force(N148I_SIMD_AUTO);
    return success;
}

static int validate_transform4_dispatch(void) {
    if (n148_cpu_detected_level() < N148_CPU_AVX2) return 1;
    const int32_t limits[] = {255, 8191, 8192, 32767, 8388607, INT32_MAX};
    uint32_t random = 1;
    int previous_cpu = n148_cpu_level();
    int success = 1;
    for (unsigned range = 0; range < sizeof(limits) / sizeof(limits[0]) && success;
         range++) {
        for (int trial = 0; trial < 128 && success; trial++) {
            int32_t input[16], scalar[16], vector[16], aliased[16];
            for (int i = 0; i < 16; i++) {
                random = random * 1664525u + 1013904223u;
                int32_t value = (int32_t)(random % (uint32_t)limits[range]);
                if (trial < 16) value = i == trial ? limits[range] : 0;
                else if (trial == 16) value = limits[range];
                else if (trial == 17) value = -limits[range];
                else if (trial == 18) value = (i & 1) ? limits[range] : -limits[range];
                else if (random & 0x80000000u) value = -value;
                if (limits[range] == INT32_MAX) {
                    if (trial == 19) value = INT32_MIN;
                    else if (trial == 20)
                        value = (i & 1) ? INT32_MAX : INT32_MIN;
                    else if (trial >= 21 && trial < 37)
                        value = i == trial - 21 ? INT32_MIN : 0;
                }
                input[i] = value;
            }
            for (int inverse = 0; inverse < 2 && success; inverse++) {
                n148_cpu_force(N148_CPU_BASELINE);
                int scalar_ok = inverse ? n148_variable_inverse(input, 4, scalar) :
                    n148_variable_forward(input, 4, scalar);
                n148_cpu_force(N148_CPU_AVX2);
                int vector_ok = inverse ? n148_variable_inverse(input, 4, vector) :
                    n148_variable_forward(input, 4, vector);
                memcpy(aliased, input, sizeof(input));
                int alias_ok = inverse ? n148_variable_inverse(aliased, 4, aliased) :
                    n148_variable_forward(aliased, 4, aliased);
                success = scalar_ok == vector_ok && scalar_ok == alias_ok &&
                    (!scalar_ok || (!memcmp(scalar, vector, sizeof(scalar)) &&
                                    !memcmp(scalar, aliased, sizeof(scalar))));
            }
        }
    }
    n148_cpu_force(previous_cpu);
    return success;
}

static int validate_large_forward_dispatch(void) {
    if (n148_cpu_detected_level() < N148_CPU_AVX2) return 1;
    const int sizes[] = {16, 32};
    const int limits[] = {255, 8191, 32767, 32768, 65535, 8388607};
    int previous_cpu = n148_cpu_level();
    uint32_t random = 0x1487115u;
    int success = 1;
    for (int ns = 0; ns < 2 && success; ns++) {
        int size = sizes[ns], count = size * size;
        for (unsigned range = 0; range < sizeof(limits)/sizeof(limits[0]) && success; range++) {
            for (int trial = 0; trial < 128 && success; trial++) {
                int32_t input[1024], scalar[1024], vector[1024], alias[1024];
                for (int i = 0; i < count; i++) {
                    random = random * 1664525u + 1013904223u;
                    int value = (int)(random % (uint32_t)(limits[range] + 1));
                    if (random & 0x80000000u) value = -value;
                    if (trial < 64) value = i == trial ? limits[range] : 0;
                    else if (trial == 64) value = limits[range];
                    else if (trial == 65) value = -limits[range];
                    else if (trial == 66) value = (i & 1) ? limits[range] : -limits[range];
                    input[i] = value;
                }
                n148_cpu_force(N148_CPU_BASELINE);
                int a = n148_variable_forward(input, size, scalar);
                n148_cpu_force(N148_CPU_AVX2);
                int b = n148_variable_forward(input, size, vector);
                memcpy(alias, input, (size_t)count * sizeof(*input));
                int c = n148_variable_forward(alias, size, alias);
                success = a == b && a == c && (!a ||
                    (!memcmp(scalar,vector,(size_t)count*sizeof(*scalar)) &&
                     !memcmp(scalar,alias,(size_t)count*sizeof(*scalar))));
            }
        }
    }
    n148_cpu_force(previous_cpu);
    return success;
}

static int validate_variable_transform_core(void) {
    if (!validate_transform4_dispatch() || !validate_large_forward_dispatch()) return 0;
    static const uint64_t expected_matrix_hash = 0xb17cb6d69247a7e9ull;
    uint8_t seen8[64] = {0};
    for (int position = 0; position < 64; position++) {
        int natural = ZIGZAG[position];
        if (natural < 0 || natural >= 64 || seen8[natural]) return 0;
        seen8[natural] = 1;
    }
    for (int size_index = 0; size_index < 2; size_index++) {
        int size = size_index == 0 ? 4 : 16;
        uint8_t seen[16 * 16] = {0};
        int32_t input[16 * 16], coefficients[16 * 16];
        int32_t reconstructed[16 * 16];
        for (int position = 0; position < size * size; position++) {
            int natural = n148_variable_zigzag(size, position);
            if (natural < 0 || natural >= size * size || seen[natural])
                return 0;
            seen[natural] = 1;
            input[position] = (int32_t)(((position * 37 +
                (position / size) * 13) % 511) - 255);
        }
        if (!n148_variable_forward(input, size, coefficients) ||
            !n148_variable_inverse(coefficients, size, reconstructed))
            return 0;
        for (int position = 0; position < size * size; position++) {
            int difference = reconstructed[position] - input[position];
            if (difference < -1 || difference > 1) return 0;
        }
    }
    return n148_variable_zigzag(8, 0) == -1 &&
        n148_variable_transform_matrix_hash() == expected_matrix_hash;
}

static uint32_t sparse16_random(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static int validate_sparse16_inverse(void) {
    uint32_t random_state = 1;
    int success = 1;
    int detected = n148_cpu_detected_level();
    for (int cpu = 0; cpu < 2 && success; cpu++) {
        if (cpu && detected < N148_CPU_AVX2) break;
        n148_cpu_force(cpu ? N148_CPU_AVX2 : N148_CPU_BASELINE);
        for (int trial = 0; trial < 500 && success; trial++) {
            int32_t input[16 * 16] = {0};
            int32_t full[16 * 16], sparse[16 * 16];
            uint32_t rows = 0, columns = 0;
            for (int sample = 0; sample < trial % 65; sample++) {
                int y = (int) (sparse16_random(&random_state) % 16u);
                int x = (int) (sparse16_random(&random_state) % 16u);
                input[y * 16 + x] =
                    (int32_t)(sparse16_random(&random_state) % 200001u) -
                    100000;
            }
            for (int y = 0; y < 16; y++) {
                for (int x = 0; x < 16; x++) {
                    if (input[y * 16 + x]) {
                        rows |= 1u << y;
                        columns |= 1u << x;
                    }
                }
            }
            int full_ok = n148_variable_inverse(input, 16, full);
            int sparse_ok = n148_variable_inverse_sparse16(
                input, rows, columns, sparse);
            if (full_ok != sparse_ok ||
                (full_ok && memcmp(full, sparse, sizeof(full)))) success = 0;
        }
    }
    n148_cpu_force(N148_CPU_BASELINE);
    return success;
}

static int validate_forced_transform_strategy(uint8_t strategy) {
    enum { WIDTH = 32, HEIGHT = 32 };
    Plane source[3] = {{0}}, decoded[3] = {{0}};
    N148CoeffPlane coefficients[3] = {{0}};
    N148TransformMap map = {0};
    uint8_t *modes = NULL;
    size_t mode_count = 0;
    int success = 0;
    for (int plane = 0; plane < 3; plane++) {
        source[plane] = create_plane(WIDTH, HEIGHT);
        if (!source[plane].data) goto cleanup;
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                int value = plane == 0 ?
                    (x * 7 + y * 11 + ((x ^ y) & 7) * 9) & 255 :
                    128 + (((x + y + plane) & 3) - 1);
                source[plane].data[y * WIDTH + x] = (uint8_t) value;
            }
        }
    }
    if (!n148_transform_map_allocate(WIDTH, HEIGHT, CHROMA_444, &map))
        goto cleanup;
    memset(map.strategies, strategy, map.count);
    if (!n148_transform_map_validate(&map) ||
        !n148_intra_quantize_planes(
            &source[0], &source[1], &source[2], 50, 0, NULL, &map,
            0, 0, coefficients, &modes, &mode_count)) goto cleanup;
    if (strategy == N148_TRANSFORM_DCT16) {
        int blocks_x = map.blocks_x[0];
        if (modes[0] != modes[1] || modes[0] != modes[blocks_x] ||
            modes[0] != modes[blocks_x + 1]) goto cleanup;
        modes[1] = (uint8_t)((modes[1] + 1) % N148_INTRA_MODE_COUNT);
        if (n148_intra_reconstruct_planes(
                coefficients, modes, mode_count, WIDTH, HEIGHT, 50,
                CHROMA_444, 0, NULL, &map, 0,
                &decoded[0], &decoded[1], &decoded[2])) goto cleanup;
        modes[1] = modes[0];
    }
    if (!n148_intra_reconstruct_planes(
            coefficients, modes, mode_count, WIDTH, HEIGHT, 50,
            CHROMA_444, 0, NULL, &map, 0,
            &decoded[0], &decoded[1], &decoded[2])) goto cleanup;
    for (int plane = 0; plane < 3; plane++) {
        if (!decoded[plane].data || decoded[plane].width != WIDTH ||
            decoded[plane].height != HEIGHT) goto cleanup;
    }
    success = 1;

cleanup:
    for (int plane = 0; plane < 3; plane++) {
        free_plane(&source[plane]);
        free_plane(&decoded[plane]);
        n148_free_coeff_plane(&coefficients[plane]);
    }
    free(modes);
    n148_transform_map_release(&map);
    return success;
}

static int validate_transform_edge_rejection(void) {
    N148TransformMap map = {0};
    if (!n148_transform_map_allocate(17, 17, CHROMA_444, &map)) return 0;
    size_t edge = (size_t)(map.rows[0] - 1) * (size_t) map.columns[0] +
        (size_t)(map.columns[0] - 1);
    map.strategies[edge] = N148_TRANSFORM_DCT16;
    int rejected = !n148_transform_map_validate(&map);
    n148_transform_map_release(&map);
    return rejected;
}

static int transform_maps_equal(const N148TransformMap *first,
                                const N148TransformMap *second) {
    return first && second && first->count == second->count &&
        memcmp(first->plane_offsets, second->plane_offsets,
               sizeof(first->plane_offsets)) == 0 &&
        memcmp(first->columns, second->columns, sizeof(first->columns)) == 0 &&
        memcmp(first->rows, second->rows, sizeof(first->rows)) == 0 &&
        memcmp(first->blocks_x, second->blocks_x,
               sizeof(first->blocks_x)) == 0 &&
        memcmp(first->blocks_y, second->blocks_y,
               sizeof(first->blocks_y)) == 0 &&
        memcmp(first->strategies, second->strategies, first->count) == 0;
}

static int validate_variable_transforms(const Image *original) {
    const uint32_t features = N148I_FEATURE_RANS | N148I_FEATURE_CONTEXT |
        N148I_FEATURE_INTRA | N148I_FEATURE_VARIABLE_TRANSFORM;
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *scalar = NULL, *vector = NULL, *parallel = NULL;
    uint8_t *corrupt_payload = NULL, *combined = NULL;
    size_t scalar_size = 0, vector_size = 0, parallel_size = 0;
    size_t combined_size = 0;
    n148i_image_t decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, combined_decoded = {0};
    n148i_image_info_t info = {0};
    Plane source_y = {0}, source_cb = {0}, source_cr = {0};
    Plane expected_y = {0}, expected_cb = {0}, expected_cr = {0};
    Image expected_rgb = {0};
    N148CoeffPlane expected[3] = {{0}}, recovered[3] = {{0}};
    N148CoeffPlane rejected[3] = {{0}};
    N148PredictionModes decoded_modes = {0};
    N148TransformMap expected_map = {0}, decoded_map = {0};
    uint8_t *expected_modes = NULL;
    size_t expected_mode_count = 0;
    size_t histogram[N148_TRANSFORM_STRATEGY_COUNT] = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_2;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.feature_flags = N148I_FEATURE_VARIABLE_TRANSFORM;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_FEATURE_RANS |
        N148I_FEATURE_VARIABLE_TRANSFORM;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = features;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != features || info.format_version != 2 ||
        n148i_decode_memory(scalar, scalar_size, &decoded) != N148I_OK)
        goto cleanup;

    n148_cpu_force(N148_CPU_BASELINE);
    n148_set_thread_count(1);
    if (!split_channels((Image *) original, &source_y, &source_cb, &source_cr,
                        CHROMA_420) ||
        !n148_transform_map_build(
            &source_y, &source_cb, &source_cr, &expected_map) ||
        !n148_intra_quantize_planes(
            &source_y, &source_cb, &source_cr, 50, 0, NULL, &expected_map,
            0, 0, expected, &expected_modes, &expected_mode_count))
        goto cleanup;

    uint16_t header_size =
        (uint16_t)(scalar[6] | ((uint16_t) scalar[7] << 8));
    uint32_t metadata_size = validation_u32(scalar + 28);
    uint32_t payload_size = validation_u32(scalar + 16);
    const uint8_t *payload = scalar + header_size;
    if (payload_size < 24 || payload[0] != 4 || payload[1] != 3 ||
        payload[2] != 1 || payload[3] != 2 ||
        validation_u32(payload + 20) != expected_map.count ||
        !n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            payload, payload_size, original->width, original->height,
            CHROMA_420, features, recovered, NULL, &decoded_modes, NULL,
            &decoded_map) ||
        decoded_modes.count != expected_mode_count ||
        memcmp(decoded_modes.values, expected_modes,
               expected_mode_count) != 0 ||
        !transform_maps_equal(&expected_map, &decoded_map)) goto cleanup;
    for (int plane = 0; plane < 3; plane++) {
        if (expected[plane].count != recovered[plane].count ||
            memcmp(expected[plane].coefficients, recovered[plane].coefficients,
                   (size_t) expected[plane].count * 64 * sizeof(short)) != 0)
            goto cleanup;
    }
    if (!n148_intra_reconstruct_planes(
            recovered, decoded_modes.values, decoded_modes.count,
            original->width, original->height, 50, CHROMA_420, 0, NULL,
            &decoded_map, 0, &expected_y, &expected_cb, &expected_cr) ||
        !merge_channels(&expected_y, &expected_cb, &expected_cr, 1,
                        &expected_rgb) ||
        decoded.width != (uint32_t) expected_rgb.width ||
        decoded.height != (uint32_t) expected_rgb.height ||
        memcmp(decoded.pixels, expected_rgb.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;
    for (size_t index = 0; index < expected_map.count; index++) {
        if (expected_map.strategies[index] >=
            N148_TRANSFORM_STRATEGY_COUNT) goto cleanup;
        histogram[expected_map.strategies[index]]++;
    }

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, decoded.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, decoded.pixels,
               decoded.stride * decoded.height) != 0) goto cleanup;

    corrupt_payload = (uint8_t *) malloc(payload_size);
    if (!corrupt_payload) goto cleanup;
    memcpy(corrupt_payload, payload, payload_size);
    corrupt_payload[20] ^= 1u;
    if (n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            corrupt_payload, payload_size, original->width, original->height,
            CHROMA_420, features, rejected, NULL, NULL, NULL, NULL))
        goto cleanup;

    const uint32_t combinations[] = {
        features | N148I_FEATURE_ADAPTIVE_QUANT,
        features | N148I_FEATURE_PERCEPTUAL_COLOR,
        features | N148I_FEATURE_ADAPTIVE_QUANT |
            N148I_FEATURE_PERCEPTUAL_COLOR,
    };
    options.thread_count = 1;
    for (size_t index = 0;
         index < sizeof(combinations) / sizeof(combinations[0]); index++) {
        options.feature_flags = combinations[index];
        if (n148i_encode_memory(&source, &options, &combined,
                                &combined_size) != N148I_OK ||
            n148i_decode_memory(combined, combined_size,
                                &combined_decoded) != N148I_OK) goto cleanup;
        n148i_free_buffer(combined); combined = NULL;
        n148i_free_image(&combined_decoded);
    }
    if (!validate_variable_transform_core() ||
        !validate_sparse16_inverse() ||
        !validate_forced_transform_strategy(N148_TRANSFORM_DCT4) ||
        !validate_forced_transform_strategy(N148_TRANSFORM_DCT8) ||
        !validate_forced_transform_strategy(N148_TRANSFORM_DCT16) ||
        !validate_transform_edge_rejection()) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,variable_transform,scalar+AVX2,1+4,2,%d,%d,%zu,"
           "0,0,0.0,strategies_%zu_%zu_%zu_q14_hash_%016llx_forced_sizes_"
           "edge_count_and_combination_rejection,%s\n",
           original->width, original->height, scalar_size,
           histogram[0], histogram[1], histogram[2],
           (unsigned long long) n148_variable_transform_matrix_hash(),
           success ? "PASS" : "FAIL");
    n148i_free_buffer(scalar); n148i_free_buffer(vector);
    n148i_free_buffer(parallel); n148i_free_buffer(combined);
    n148i_free_image(&decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded); n148i_free_image(&combined_decoded);
    free(corrupt_payload); free(expected_modes);
    free_plane(&source_y); free_plane(&source_cb); free_plane(&source_cr);
    free_plane(&expected_y); free_plane(&expected_cb); free_plane(&expected_cr);
    free_image(&expected_rgb);
    n148_prediction_modes_release(&decoded_modes);
    n148_transform_map_release(&expected_map);
    n148_transform_map_release(&decoded_map);
    for (int plane = 0; plane < 3; plane++) {
        n148_free_coeff_plane(&expected[plane]);
        n148_free_coeff_plane(&recovered[plane]);
        n148_free_coeff_plane(&rejected[plane]);
    }
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_rdo_core(uint32_t *changed) {
    float ideal_levels[64] = {0.0f};
    double unit_energy[64];
    short coefficients[64] = {0};
    for (int position = 0; position < 64; position++)
        unit_energy[position] = 1.0 + (position & 3) * 0.25;
    ideal_levels[0] = 5.0f;
    ideal_levels[1] = 0.55f;
    ideal_levels[7] = -1.6f;
    ideal_levels[31] = 0.7f;
    coefficients[0] = 5;
    coefficients[1] = 1;
    coefficients[7] = -2;
    coefficients[31] = 1;
    short dc = coefficients[0];
    N148RdoResult result;
    double before = n148_rdo_proxy_cost(
        ideal_levels, unit_energy, coefficients, 4.0);
    if (!n148_rdo_refine_coefficients(
            ideal_levels, unit_energy, 4.0, 5, coefficients, &result) ||
        coefficients[0] != dc || result.changed_coefficients == 0 ||
        result.output_nonzeros > result.input_nonzeros ||
        result.output_cost > result.input_cost + 1e-9 ||
        fabs(result.input_cost - before) > 1e-9 ||
        fabs(result.output_cost - n148_rdo_proxy_cost(
            ideal_levels, unit_energy, coefficients, 4.0)) > 1e-9 ||
        n148_rdo_refine_coefficients(
            ideal_levels, unit_energy, -1.0, 5, coefficients, NULL)) return 0;
    if (changed) *changed = result.changed_coefficients;
    return 1;
}

static int validate_rate_distortion_optimization(const Image *original) {
    const uint32_t base_features = N148I_FEATURE_RANS |
        N148I_FEATURE_CONTEXT | N148I_FEATURE_INTRA;
    const uint32_t rdo_features = base_features | N148I_FEATURE_RDO;
    const uint32_t combined_features = rdo_features |
        N148I_FEATURE_PERCEPTUAL_COLOR | N148I_FEATURE_ADAPTIVE_QUANT |
        N148I_FEATURE_VARIABLE_TRANSFORM;
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *baseline = NULL, *effort0 = NULL, *scalar = NULL;
    uint8_t *vector = NULL, *parallel = NULL, *maximum = NULL;
    uint8_t *combined = NULL;
    size_t baseline_size = 0, effort0_size = 0, scalar_size = 0;
    size_t vector_size = 0, parallel_size = 0, maximum_size = 0;
    size_t combined_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, maximum_decoded = {0};
    n148i_image_t combined_decoded = {0};
    n148i_image_info_t info = {0};
    uint32_t changed = 0;
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.effort != 3)
        goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_2;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 0;
    options.feature_flags = base_features;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &baseline, &baseline_size) !=
            N148I_OK) goto cleanup;
    options.feature_flags = rdo_features;
    if (n148i_encode_memory(&source, &options, &effort0, &effort0_size) !=
            N148I_OK || effort0_size != baseline_size ||
        effort0_size < N148_EXTENDED_HEADER_SIZE ||
        memcmp(effort0 + N148_EXTENDED_HEADER_SIZE,
               baseline + N148_EXTENDED_HEADER_SIZE,
               effort0_size - N148_EXTENDED_HEADER_SIZE) != 0) goto cleanup;

    options.feature_flags = N148I_FEATURE_RDO;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_FEATURE_RANS | N148I_FEATURE_RDO;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    options.feature_flags = rdo_features;
    options.effort = 5;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != rdo_features || info.effort != 5 ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK)
        goto cleanup;
    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    options.thread_count = 1;
    options.effort = 9;
    if (n148i_encode_memory(&source, &options, &maximum, &maximum_size) !=
            N148I_OK ||
        n148i_read_header(maximum, maximum_size, &info) != N148I_OK ||
        info.feature_flags != rdo_features || info.effort != 9 ||
        n148i_decode_memory(maximum, maximum_size, &maximum_decoded) !=
            N148I_OK) goto cleanup;

    options.feature_flags = combined_features;
    options.effort = 5;
    if (n148i_encode_memory(&source, &options, &combined, &combined_size) !=
            N148I_OK ||
        n148i_decode_memory(combined, combined_size, &combined_decoded) !=
            N148I_OK || !validate_rdo_core(&changed)) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,rate_distortion_optimization,scalar+AVX2,1+4,2,%d,%d,%zu,"
           "%zu,%zu,0.0,effort0_%zu_effort5_%zu_effort9_%zu_changed_%u_"
           "trellis_modes_dependencies_and_combination_determinism,%s\n",
           original->width, original->height, scalar_size, maximum_size,
           maximum_size, effort0_size, scalar_size, maximum_size, changed,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(baseline); n148i_free_buffer(effort0);
    n148i_free_buffer(scalar); n148i_free_buffer(vector);
    n148i_free_buffer(parallel); n148i_free_buffer(maximum);
    n148i_free_buffer(combined);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded); n148i_free_image(&maximum_decoded);
    n148i_free_image(&combined_decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_loop_filter_core(N148LoopFilterStats *result) {
    Plane first = create_plane(17, 17);
    Plane second = create_plane(17, 17);
    if (!first.data || !second.data) {
        free_plane(&first);
        free_plane(&second);
        return 0;
    }
    for (int y = 0; y < first.height; y++) {
        for (int x = 0; x < first.width; x++) {
            first.data[(long) y * first.width + x] =
                (uint8_t)(x < 8 ? 100 : 110);
        }
    }
    first.data[4 * first.width + 11] = 140;
    memcpy(second.data, first.data, (size_t) first.width * first.height);
    int before_gap = first.data[5 * first.width + 8] -
        first.data[5 * first.width + 7];
    N148LoopFilterStats first_stats = {0}, second_stats = {0};
    int success = n148_loop_filter_block(
        &first, 8, 0, 8, 30, 0, N148_AQ_INITIAL_LEVEL, &first_stats) &&
        n148_loop_filter_block(
            &second, 8, 0, 8, 30, 0, N148_AQ_INITIAL_LEVEL,
            &second_stats);
    int after_gap = first.data[5 * first.width + 8] -
        first.data[5 * first.width + 7];
    if (!success || first_stats.deblocked_samples == 0 ||
        first_stats.deringed_samples == 0 || after_gap >= before_gap ||
        first.data[4 * first.width + 11] >= 140 ||
        memcmp(first.data, second.data,
               (size_t) first.width * first.height) != 0 ||
        memcmp(&first_stats, &second_stats, sizeof(first_stats)) != 0 ||
        n148_loop_filter_strength(30, 0, 3) <=
            n148_loop_filter_strength(30, 0, 0) ||
        n148_loop_filter_strength(30, 0, 1) <=
            n148_loop_filter_strength(90, 0, 1) ||
        n148_loop_filter_strength(100, 0, 1) != 0 ||
        n148_loop_filter_strength(0, 0, 1) != -1 ||
        n148_loop_filter_block(NULL, 0, 0, 8, 30, 0, 1, NULL)) {
        success = 0;
    }
    if (success && result) *result = first_stats;
    free_plane(&first);
    free_plane(&second);
    return success;
}

static size_t different_rgb_samples(const n148i_image_t *first,
                                    const Image *second) {
    if (!first || !first->pixels || !second || !second->pixels ||
        first->width != (uint32_t) second->width ||
        first->height != (uint32_t) second->height) return 0;
    size_t count = (size_t) second->width * (size_t) second->height * 3u;
    size_t different = 0;
    for (size_t index = 0; index < count; index++)
        different += first->pixels[index] != second->pixels[index];
    return different;
}

static int validate_causal_loop_filter(const Image *original) {
    const uint32_t base_features = N148I_FEATURE_RANS |
        N148I_FEATURE_CONTEXT | N148I_FEATURE_INTRA | N148I_FEATURE_RDO;
    const uint32_t filter_features =
        base_features | N148I_FEATURE_LOOP_FILTER;
    const uint32_t combined_features = N148I_FEATURE_ALL;
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *baseline = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *combined = NULL;
    size_t baseline_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, combined_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, combined_decoded = {0};
    N148CoeffPlane recovered[3] = {{0}};
    N148PredictionModes decoded_modes = {0};
    Plane filtered_y = {0}, filtered_cb = {0}, filtered_cr = {0};
    Plane plain_y = {0}, plain_cb = {0}, plain_cr = {0};
    Image filtered_rgb = {0}, plain_rgb = {0};
    n148i_image_info_t info = {0};
    N148LoopFilterStats filter_stats = {0};
    size_t changed_samples = 0;
    uint32_t payload_size = 0;
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.effort != 3)
        goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_2;
    options.quality = 30;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 5;
    options.feature_flags = base_features;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &baseline, &baseline_size) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_FEATURE_LOOP_FILTER;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_FEATURE_RANS | N148I_FEATURE_LOOP_FILTER;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    options.feature_flags = filter_features;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != filter_features || info.effort != 5 ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK)
        goto cleanup;

    uint16_t header_size =
        (uint16_t)(scalar[6] | ((uint16_t) scalar[7] << 8));
    uint32_t metadata_size = validation_u32(scalar + 28);
    payload_size = validation_u32(scalar + 16);
    if (!n148_decode_format_2_coefficients(
            scalar + N148_EXTENDED_HEADER_SIZE, metadata_size,
            scalar + header_size, payload_size,
            original->width, original->height, CHROMA_420,
            filter_features, recovered, NULL, &decoded_modes, NULL, NULL) ||
        !n148_intra_reconstruct_planes(
            recovered, decoded_modes.values, decoded_modes.count,
            original->width, original->height, 30, CHROMA_420, 0,
            NULL, NULL, 1, &filtered_y, &filtered_cb, &filtered_cr) ||
        !merge_channels(&filtered_y, &filtered_cb, &filtered_cr, 1,
                        &filtered_rgb) ||
        memcmp(scalar_decoded.pixels, filtered_rgb.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0 ||
        !n148_intra_reconstruct_planes(
            recovered, decoded_modes.values, decoded_modes.count,
            original->width, original->height, 30, CHROMA_420, 0,
            NULL, NULL, 0, &plain_y, &plain_cb, &plain_cr) ||
        !merge_channels(&plain_y, &plain_cb, &plain_cr, 1, &plain_rgb))
        goto cleanup;
    changed_samples = different_rgb_samples(&scalar_decoded, &plain_rgb);
    if (changed_samples == 0 || !validate_loop_filter_core(&filter_stats))
        goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420;
         chroma++) {
        uint8_t *chroma_stream = NULL;
        size_t chroma_size = 0;
        n148i_image_t chroma_decoded = {0};
        options.thread_count = 1;
        options.chroma = (n148i_chroma_t) chroma;
        n148i_result_t encode_result = n148i_encode_memory(
            &source, &options, &chroma_stream, &chroma_size);
        n148i_result_t decode_result = encode_result == N148I_OK ?
            n148i_decode_memory(chroma_stream, chroma_size,
                                &chroma_decoded) : encode_result;
        n148i_free_buffer(chroma_stream);
        n148i_free_image(&chroma_decoded);
        if (encode_result != N148I_OK || decode_result != N148I_OK)
            goto cleanup;
    }

    options.thread_count = 1;
    options.chroma = N148I_CHROMA_420;
    options.quality = 50;
    options.feature_flags = combined_features;
    if (n148i_encode_memory(&source, &options, &combined, &combined_size) !=
            N148I_OK ||
        n148i_decode_memory(combined, combined_size, &combined_decoded) !=
            N148I_OK) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,causal_loop_filter,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,"
           "0.0,base_%zu_filter_%zu_changed_%zu_deblock_%llu_dering_%llu_"
           "causal_reference_dependencies_and_combination_determinism,%s\n",
           original->width, original->height, scalar_size, payload_size,
           payload_size, baseline_size, scalar_size, changed_samples,
           (unsigned long long) filter_stats.deblocked_samples,
           (unsigned long long) filter_stats.deringed_samples,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(baseline); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(combined);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded); n148i_free_image(&combined_decoded);
    for (int plane = 0; plane < 3; plane++)
        n148_free_coeff_plane(&recovered[plane]);
    n148_prediction_modes_release(&decoded_modes);
    free_plane(&filtered_y); free_plane(&filtered_cb); free_plane(&filtered_cr);
    free_plane(&plain_y); free_plane(&plain_cb); free_plane(&plain_cr);
    free_image(&filtered_rgb); free_image(&plain_rgb);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int inspect_directional_stream(const uint8_t *stream, size_t stream_size,
                             const Image *source, int chroma,
                             uint32_t features,
                             N148SyntaxInspection *inspection) {
    n148i_image_info_t info = {0};
    return stream && source && inspection &&
        n148i_read_header(stream, stream_size, &info) == N148I_OK &&
        info.format_version == N148I_FORMAT_VERSION_3 &&
        info.feature_flags == features &&
        (int) info.chroma == chroma &&
        info.encoded_header_size <= stream_size &&
        info.metadata_size <= info.encoded_header_size -
            N148_EXTENDED_HEADER_SIZE &&
        info.payload_size == stream_size - info.encoded_header_size &&
        n148_directional_intra_inspect(
            stream + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            stream + info.encoded_header_size, info.payload_size,
            source->width, source->height, chroma, features, inspection);
}

static int inspect_variable_luma_stream(const uint8_t *stream, size_t stream_size,
                             const Image *source, int chroma,
                             uint32_t features,
                             N148SyntaxInspection *inspection) {
    n148i_image_info_t info = {0};
    int joint_chroma =
        (features & N148I_FEATURE_JOINT_CHROMA_INTRA) != 0;
    int segmented = (features & N148I_FEATURE_SEGMENTATION) != 0;
    int adaptive_filter =
        (features & N148I_FEATURE_ADAPTIVE_LOOP_FILTER) != 0;
    int variable_luma =
        (features & N148I_FEATURE_LUMA_TRANSFORM_16X16) != 0;
    return stream && source && inspection &&
        n148i_read_header(stream, stream_size, &info) == N148I_OK &&
        info.format_version == N148I_FORMAT_VERSION_5 &&
        info.feature_flags == features && (int) info.chroma == chroma &&
        info.encoded_header_size <= stream_size &&
        info.metadata_size <= info.encoded_header_size -
            N148_EXTENDED_HEADER_SIZE &&
        info.payload_size == stream_size - info.encoded_header_size &&
        n148_predictive_inspect(
            stream + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            stream + info.encoded_header_size, info.payload_size,
            source->width, source->height, chroma,
            features & N148_FORMAT_3_SUPPORTED_FEATURES, joint_chroma,
            segmented, adaptive_filter, variable_luma, 0, inspection);
}

static int validate_directional_intra(const Image *original) {
    const uint32_t profile_features[3] = {
        N148I_PROFILE_DIRECTIONAL_INTRA,
        N148I_PROFILE_CONTEXTUAL_RDO,
        N148I_PROFILE_PERCEPTUAL_TRELLIS,
    };
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *profile_streams[3] = {0};
    size_t profile_sizes[3] = {0};
    n148i_image_t profile_decoded[3] = {{0}};
    N148SyntaxInspection profile_inspection[3] = {0};
    uint8_t *vector = NULL, *parallel = NULL, *corrupt = NULL;
    size_t vector_size = 0, parallel_size = 0;
    n148i_image_t vector_decoded = {0}, parallel_decoded = {0};
    uint64_t mode_mask = 0;
    uint64_t total_splits = 0;
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.feature_flags != N148I_DEFAULT_FEATURES ||
        options.effort != 3) goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_3;
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 5;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK) goto cleanup;
    for (int profile = 0; profile < 3; profile++) {
        options.feature_flags = profile_features[profile];
        if (n148i_encode_memory(
                &source, &options, &profile_streams[profile],
                &profile_sizes[profile]) != N148I_OK ||
            n148i_decode_memory(
                profile_streams[profile], profile_sizes[profile],
                &profile_decoded[profile]) != N148I_OK ||
            !inspect_directional_stream(
                profile_streams[profile], profile_sizes[profile], original,
                options.chroma, profile_features[profile],
                &profile_inspection[profile]) ||
            profile_decoded[profile].width != source.width ||
            profile_decoded[profile].height != source.height ||
            profile_inspection[profile].split_blocks == 0 ||
            profile_inspection[profile].plane_split_blocks[1] != 0 ||
            profile_inspection[profile].plane_split_blocks[2] != 0 ||
            profile_inspection[profile].plane_total_blocks[1] == 0 ||
            profile_inspection[profile].plane_total_blocks[2] == 0)
            goto cleanup;
        total_splits += profile_inspection[profile].split_blocks;
        for (int mode = 0; mode < N148_DIRECTIONAL_INTRA_MODE_COUNT; mode++) {
            if (profile_inspection[profile].mode_histogram[mode] != 0)
                mode_mask |= 1ull << mode;
        }
    }
    if (mode_mask != (1ull << N148_DIRECTIONAL_INTRA_MODE_COUNT) - 1 ||
        profile_sizes[0] == profile_sizes[1] ||
        profile_sizes[1] == profile_sizes[2]) goto cleanup;

    options.feature_flags = N148I_PROFILE_PERCEPTUAL_TRELLIS;
    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != profile_sizes[2] ||
        memcmp(vector, profile_streams[2], vector_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, profile_decoded[2].pixels,
               profile_decoded[2].stride * profile_decoded[2].height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != profile_sizes[2] ||
        memcmp(parallel, profile_streams[2], parallel_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, profile_decoded[2].pixels,
               profile_decoded[2].stride * profile_decoded[2].height) != 0)
        goto cleanup;

    options.thread_count = 1;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_422;
         chroma++) {
        uint8_t *stream = NULL;
        size_t stream_size = 0;
        n148i_image_t decoded = {0};
        N148SyntaxInspection inspection = {0};
        options.chroma = (n148i_chroma_t) chroma;
        n148i_result_t encoded_result = n148i_encode_memory(
            &source, &options, &stream, &stream_size);
        int chroma_ok = encoded_result == N148I_OK &&
            n148i_decode_memory(stream, stream_size, &decoded) == N148I_OK &&
            inspect_directional_stream(stream, stream_size, original, chroma,
                              options.feature_flags, &inspection) &&
            inspection.plane_total_blocks[1] != 0 &&
            inspection.plane_total_blocks[2] != 0 &&
            inspection.plane_split_blocks[1] == 0 &&
            inspection.plane_split_blocks[2] == 0;
        n148i_free_buffer(stream);
        n148i_free_image(&decoded);
        if (!chroma_ok) goto cleanup;
    }

    options.chroma = N148I_CHROMA_420;
    options.feature_flags = N148I_PROFILE_PERCEPTUAL_TRELLIS &
        ~N148I_FEATURE_CONTEXTUAL_RDO;
    if (n148i_encode_memory(&source, &options, &corrupt, &parallel_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    if (n148i_decode_memory(profile_streams[2], profile_sizes[2] - 1,
                            &parallel_decoded) == N148I_OK) goto cleanup;
    corrupt = (uint8_t *) malloc(profile_sizes[2]);
    if (!corrupt) goto cleanup;
    memcpy(corrupt, profile_streams[2], profile_sizes[2]);
    corrupt[21] &= (uint8_t) ~0x04u;
    if (n148i_decode_memory(corrupt, profile_sizes[2], &parallel_decoded) ==
            N148I_OK) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,directional_intra,scalar+AVX2,1+4,0+1+2,%d,%d,%zu,0,0,"
           "0.0,profiles_%zu_%zu_%zu_splits_%llu_modes_0x%llx_"
           "directional_4x4_contextual_rdo_trellis_compatibility_and_"
           "determinism,%s\n",
           original->width, original->height, profile_sizes[2],
           profile_sizes[0], profile_sizes[1], profile_sizes[2],
           (unsigned long long) total_splits,
           (unsigned long long) mode_mask,
           success ? "PASS" : "FAIL");
    for (int profile = 0; profile < 3; profile++) {
        n148i_free_buffer(profile_streams[profile]);
        n148i_free_image(&profile_decoded[profile]);
    }
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    free(corrupt);
    n148i_free_image(&vector_decoded); n148i_free_image(&parallel_decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_directional_intra_odd_dimensions(const Image *original) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *single = NULL, *parallel = NULL;
    size_t single_size = 0, parallel_size = 0;
    n148i_image_t decoded = {0};
    N148SyntaxInspection inspection = {0};
    int success = 0;
    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_3;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_PERCEPTUAL_TRELLIS;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &single, &single_size) !=
            N148I_OK ||
        n148i_decode_memory(single, single_size, &decoded) != N148I_OK ||
        decoded.width != source.width || decoded.height != source.height ||
        !inspect_directional_stream(single, single_size, original, options.chroma,
                           options.feature_flags, &inspection))
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != single_size ||
        memcmp(parallel, single, single_size) != 0) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,directional_intra_odd_dimensions,scalar+auto,1+4,2,%d,%d,%zu,0,0,"
           "0.0,split_%llu_partial_4x4_roundtrip_and_determinism,%s\n",
           original->width, original->height, single_size,
           (unsigned long long) inspection.split_blocks,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(single); n148i_free_buffer(parallel);
    n148i_free_image(&decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static uint64_t rgb_squared_error(const Image *source,
                                  const n148i_image_t *decoded) {
    if (!source || !source->pixels || !decoded || !decoded->pixels ||
        decoded->width != (uint32_t) source->width ||
        decoded->height != (uint32_t) source->height) return UINT64_MAX;
    size_t count = (size_t) source->width * (size_t) source->height * 3u;
    uint64_t error = 0;
    for (size_t index = 0; index < count; index++) {
        int difference = source->pixels[index] - decoded->pixels[index];
        error += (uint64_t)(difference * difference);
    }
    return error;
}

static int same_stream_except_version(const uint8_t *first,
                                      const uint8_t *second, size_t size) {
    return first && second && size > N148I_MAGIC_LEN &&
        memcmp(first, second, N148I_MAGIC_LEN) == 0 &&
        memcmp(first + N148I_MAGIC_LEN + 1,
               second + N148I_MAGIC_LEN + 1,
               size - N148I_MAGIC_LEN - 1) == 0;
}

static uint32_t validation_default_features(int format_version) {
    if (format_version == N148I_FORMAT_VERSION_2)
        return N148I_FORMAT_2_DEFAULT_FEATURES;
    if (format_version == N148I_FORMAT_VERSION_3)
        return N148I_FORMAT_3_DEFAULT_FEATURES;
    if (format_version == N148I_FORMAT_VERSION_4)
        return N148I_FORMAT_4_DEFAULT_FEATURES;
    if (format_version == N148I_FORMAT_VERSION_5)
        return N148I_FORMAT_5_DEFAULT_FEATURES;
    if (format_version == N148I_FORMAT_VERSION_6)
        return N148I_FORMAT_6_DEFAULT_FEATURES;
    if (format_version == N148I_FORMAT_VERSION_7)
        return N148I_FORMAT_7_DEFAULT_FEATURES;
    return 0;
}

/* Exercise the public format/chroma cross-product, not just the historical
   format-1 layout sweep. Each point must parse and decode exactly, and scalar
   single-thread output must match AVX2 four-thread output byte for byte. */
static int validate_chroma_format_matrix(const Image *original,
                                         const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    int completed = 0;
    int success = 0;

    for (int format = N148I_FORMAT_VERSION_1;
         format <= N148I_FORMAT_VERSION_7; format++) {
        for (int chroma = N148I_CHROMA_444;
             chroma <= N148I_CHROMA_420; chroma++) {
            n148i_encode_options_t options;
            uint8_t *scalar = NULL, *parallel = NULL;
            size_t scalar_size = 0, parallel_size = 0;
            n148i_image_t scalar_decoded = {0}, parallel_decoded = {0};
            n148i_image_info_t info = {0};
            int point_ok = 0;

            n148i_encode_options_init(&options);
            options.quality = 50;
            options.chroma = (n148i_chroma_t) chroma;
            options.format_version = (uint32_t) format;
            options.feature_flags = validation_default_features(format);
            options.thread_count = 1;
            if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
                n148i_encode_memory(&source, &options,
                                    &scalar, &scalar_size) != N148I_OK ||
                n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
                info.format_version != (uint8_t) format ||
                info.chroma != (n148i_chroma_t) chroma ||
                info.width != source.width || info.height != source.height ||
                info.encoded_header_size + info.payload_size != scalar_size ||
                n148i_decode_memory(scalar, scalar_size,
                                    &scalar_decoded) != N148I_OK ||
                scalar_decoded.width != source.width ||
                scalar_decoded.height != source.height)
                goto point_cleanup;

            uint32_t expected_features = validation_default_features(format);
            if (format >= N148I_FORMAT_VERSION_4 &&
                chroma == N148I_CHROMA_444)
                expected_features &=
                    ~N148I_FEATURE_RECONSTRUCTION_AWARE_CHROMA;
            if (info.feature_flags != expected_features)
                goto point_cleanup;

            options.thread_count = 4;
            if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
                n148i_encode_memory(&source, &options,
                                    &parallel, &parallel_size) != N148I_OK ||
                parallel_size != scalar_size ||
                memcmp(parallel, scalar, scalar_size) != 0 ||
                n148i_decode_memory(parallel, parallel_size,
                                    &parallel_decoded) != N148I_OK ||
                parallel_decoded.stride != scalar_decoded.stride ||
                memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
                       scalar_decoded.stride * scalar_decoded.height) != 0)
                goto point_cleanup;
            point_ok = 1;

point_cleanup:
            n148i_free_buffer(scalar);
            n148i_free_buffer(parallel);
            n148i_free_image(&scalar_decoded);
            n148i_free_image(&parallel_decoded);
            if (!point_ok) goto cleanup;
            completed++;
        }
    }
    success = completed == 21;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,0+1+2,%d,%d,0,0,0,0.0,"
           "formats_1_to_7_layout_matrix_%d_of_21_exact_roundtrips_"
           "and_determinism,%s\n",
           name, original->width, original->height, completed,
           success ? "PASS" : "FAIL");
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_reconstruction_aware_chroma(const Image *original,
                                     const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *format_3_stream = NULL, *control = NULL, *scalar = NULL;
    uint8_t *vector = NULL, *parallel = NULL;
    uint8_t *layout_control = NULL, *layout_aware = NULL;
    uint8_t *layout_444 = NULL;
    size_t format_3_size = 0, control_size = 0, scalar_size = 0;
    size_t vector_size = 0, parallel_size = 0;
    size_t layout_control_size = 0, layout_aware_size = 0;
    size_t layout_444_size = 0;
    n148i_image_t control_decoded = {0}, scalar_decoded = {0};
    n148i_image_t vector_decoded = {0}, parallel_decoded = {0};
    n148i_image_t layout_control_decoded = {0};
    n148i_image_t layout_aware_decoded = {0}, layout_444_decoded = {0};
    n148i_image_info_t info = {0}, layout_info = {0};
    uint64_t control_error = UINT64_MAX, aware_error = UINT64_MAX;
    uint64_t control_422_error = UINT64_MAX, aware_422_error = UINT64_MAX;
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.feature_flags != N148I_DEFAULT_FEATURES ||
        strcmp(n148i_library_version(), "V2") != 0 ||
        n148i_format_version() != N148I_FORMAT_VERSION_7) goto cleanup;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 5;

    options.format_version = N148I_FORMAT_VERSION_3;
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &format_3_stream,
                            &format_3_size) != N148I_OK)
        goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_4;
    if (n148i_encode_memory(&source, &options, &control, &control_size) !=
            N148I_OK || control_size != format_3_size ||
        !same_stream_except_version(format_3_stream, control, format_3_size) ||
        n148i_decode_memory(control, control_size, &control_decoded) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK || (scalar_size == control_size &&
            memcmp(scalar, control, scalar_size) == 0) ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_4 ||
        info.feature_flags != N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA ||
        info.encoded_header_size + info.payload_size != scalar_size ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK)
        goto cleanup;
    control_error = rgb_squared_error(original, &control_decoded);
    aware_error = rgb_squared_error(original, &scalar_decoded);
    if (control_error == UINT64_MAX || aware_error >= control_error)
        goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    /* 4:2:2 uses the same reconstruction search with an identity vertical
       axis. This is the path that the old format validation rejected. */
    options.thread_count = 1;
    options.chroma = N148I_CHROMA_422;
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &layout_control,
                            &layout_control_size) != N148I_OK ||
        n148i_decode_memory(layout_control, layout_control_size,
                            &layout_control_decoded) != N148I_OK)
        goto cleanup;
    options.feature_flags = N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA;
    if (n148i_encode_memory(&source, &options, &layout_aware,
                            &layout_aware_size) != N148I_OK ||
        n148i_read_header(layout_aware, layout_aware_size,
                          &layout_info) != N148I_OK ||
        layout_info.chroma != N148I_CHROMA_422 ||
        layout_info.feature_flags != N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA ||
        n148i_decode_memory(layout_aware, layout_aware_size,
                            &layout_aware_decoded) != N148I_OK)
        goto cleanup;
    control_422_error = rgb_squared_error(original, &layout_control_decoded);
    aware_422_error = rgb_squared_error(original, &layout_aware_decoded);
    if (control_422_error == UINT64_MAX ||
        aware_422_error >= control_422_error) goto cleanup;

    /* In 4:4:4 no reduction exists. The encoder accepts the format 4 default
       request but canonicalizes the irrelevant reconstruction-aware bit from the
       stream instead of rejecting the whole layout. */
    options.chroma = N148I_CHROMA_444;
    if (n148i_encode_memory(&source, &options, &layout_444,
                            &layout_444_size) != N148I_OK ||
        n148i_read_header(layout_444, layout_444_size,
                          &layout_info) != N148I_OK ||
        layout_info.chroma != N148I_CHROMA_444 ||
        layout_info.feature_flags != N148I_FORMAT_3_DEFAULT_FEATURES ||
        n148i_decode_memory(layout_444, layout_444_size,
                            &layout_444_decoded) != N148I_OK)
        goto cleanup;
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "format_3_payload_compatibility_reconstruction_error_%llu_to_%llu_"
           "422_reconstruction_error_%llu_to_%llu_444_canonical_"
           "odd_edges_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           (unsigned long long) control_error,
           (unsigned long long) aware_error,
           (unsigned long long) control_422_error,
           (unsigned long long) aware_422_error,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(format_3_stream); n148i_free_buffer(control);
    n148i_free_buffer(scalar); n148i_free_buffer(vector);
    n148i_free_buffer(parallel); n148i_free_buffer(layout_control);
    n148i_free_buffer(layout_aware); n148i_free_buffer(layout_444);
    n148i_free_image(&control_decoded); n148i_free_image(&scalar_decoded);
    n148i_free_image(&vector_decoded); n148i_free_image(&parallel_decoded);
    n148i_free_image(&layout_control_decoded);
    n148i_free_image(&layout_aware_decoded);
    n148i_free_image(&layout_444_decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_joint_chroma_intra(const Image *original,
                                     const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *control = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *corrupt = NULL, *invalid = NULL;
    uint8_t *compatibility = NULL;
    size_t control_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, invalid_size = 0, compatibility_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, rejected = {0};
    n148i_image_t compatibility_decoded = {0};
    n148i_image_info_t info = {0};
    n148i_image_info_t compatibility_info = {0};
    N148SyntaxInspection inspection = {0};
    uint32_t stored_mode_slots = 0;
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_4;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 5;
    options.feature_flags = N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &control, &control_size) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_PROFILE_JOINT_CHROMA_INTRA;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK || (scalar_size == control_size &&
            memcmp(scalar, control, scalar_size) == 0) ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_4 ||
        info.feature_flags != N148I_PROFILE_JOINT_CHROMA_INTRA ||
        info.encoded_header_size + info.payload_size != scalar_size ||
        info.payload_size < 20 || scalar[info.encoded_header_size] != 6 ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK ||
        !n148_joint_chroma_inspect(
            scalar + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            scalar + info.encoded_header_size, info.payload_size,
            original->width, original->height, CHROMA_420,
            N148I_FORMAT_3_DEFAULT_FEATURES, &inspection)) goto cleanup;

    stored_mode_slots = validation_u32(
        scalar + info.encoded_header_size + 16);
    int chroma_width, chroma_height;
    chroma_dimensions(CHROMA_420, original->width, original->height,
                      &chroma_width, &chroma_height);
    uint64_t y_blocks = (uint64_t)(original->width / 8 +
        (original->width % 8 != 0)) *
        (uint64_t)(original->height / 8 + (original->height % 8 != 0));
    uint64_t chroma_blocks = (uint64_t)(chroma_width / 8 +
        (chroma_width % 8 != 0)) *
        (uint64_t)(chroma_height / 8 + (chroma_height % 8 != 0));
    uint64_t expected_slots = (y_blocks + chroma_blocks) *
        N148_INTRA_MODE_SLOTS_PER_BLOCK;
    if (expected_slots > UINT32_MAX || stored_mode_slots != expected_slots)
        goto cleanup;
    for (int mode = 0; mode < N148_DIRECTIONAL_INTRA_MODE_COUNT; mode++) {
        if (inspection.plane_mode_histogram[1][mode] !=
            inspection.plane_mode_histogram[2][mode])
            goto cleanup;
    }

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    corrupt = (uint8_t *) malloc(scalar_size);
    if (!corrupt) goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[21] &= (uint8_t) ~0x20u;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[info.encoded_header_size] = 5;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    validation_put_u32(corrupt + info.encoded_header_size + 16,
                       stored_mode_slots + 1);
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;

    /* Bit 12 is intentionally 4:2:0-only, but the independently signaled
       joint-mode grammar must remain valid with every public chroma layout. */
    options.thread_count = 1;
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES |
        N148I_FEATURE_JOINT_CHROMA_INTRA;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420; chroma++) {
        options.chroma = (n148i_chroma_t) chroma;
        if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
            n148i_encode_memory(&source, &options, &compatibility,
                                &compatibility_size) != N148I_OK ||
            n148i_read_header(compatibility, compatibility_size,
                              &compatibility_info) != N148I_OK ||
            compatibility_info.chroma != (n148i_chroma_t) chroma ||
            compatibility_info.feature_flags != options.feature_flags ||
            n148i_decode_memory(compatibility, compatibility_size,
                                &compatibility_decoded) != N148I_OK)
            goto cleanup;
        n148i_free_buffer(compatibility);
        compatibility = NULL;
        compatibility_size = 0;
        n148i_free_image(&compatibility_decoded);
    }

    options.thread_count = 1;
    options.chroma = N148I_CHROMA_420;
    options.feature_flags = N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA |
        N148I_FEATURE_JOINT_CHROMA_INTRA;
    options.feature_flags &= ~N148I_FEATURE_CONTEXTUAL_RDO;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "payload_revision_6_stored_mode_slots_%u_joint_chroma_"
           "corruption_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, stored_mode_slots,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(control); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(invalid); n148i_free_buffer(compatibility); free(corrupt);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded); n148i_free_image(&rejected);
    n148i_free_image(&compatibility_decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_calibrated_chroma_quantization(const Image *original,
                                     const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *control = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *explicit_stream = NULL;
    uint8_t *compatibility = NULL, *invalid = NULL, *corrupt = NULL;
    size_t control_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, explicit_size = 0;
    size_t compatibility_size = 0, invalid_size = 0;
    uint32_t scalar_payload_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, explicit_decoded = {0};
    n148i_image_t compatibility_decoded = {0}, rejected = {0};
    n148i_image_info_t control_info = {0}, info = {0};
    n148i_image_info_t compatibility_info = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.feature_flags != N148I_DEFAULT_FEATURES ||
        options.chroma_quality != 0) goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_4;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 5;
    options.feature_flags = N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &control, &control_size) !=
            N148I_OK ||
        n148i_read_header(control, control_size, &control_info) != N148I_OK ||
        control_info.chroma_quality != 50 || control[27] != 0)
        goto cleanup;

    options.feature_flags = N148I_PROFILE_CALIBRATED_CHROMA_QUANT;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_4 ||
        info.feature_flags != N148I_PROFILE_CALIBRATED_CHROMA_QUANT ||
        info.quality != 50 || info.chroma_quality != 55 || scalar[27] != 55 ||
        info.encoded_header_size + info.payload_size != scalar_size ||
        info.payload_size < 20 || scalar[info.encoded_header_size] != 5 ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK)
        goto cleanup;
    scalar_payload_size = info.payload_size;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    /* The thread comparison above deliberately keeps AVX2 fixed. The FMA
       inverse path may differ by one final clamp unit; stream identity across
       SIMD levels is checked independently before the thread comparison. */
    options.thread_count = 1;
    options.chroma_quality = 63;
    if (n148i_encode_memory(&source, &options, &explicit_stream,
                            &explicit_size) != N148I_OK ||
        n148i_read_header(explicit_stream, explicit_size, &info) != N148I_OK ||
        info.chroma_quality != 63 || explicit_stream[27] != 63 ||
        n148i_decode_memory(explicit_stream, explicit_size,
                            &explicit_decoded) != N148I_OK)
        goto cleanup;

    corrupt = (uint8_t *) malloc(scalar_size);
    if (!corrupt) goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[27] = 0;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[27] = 101;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[21] &= (uint8_t) ~0x40u;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;

    options.chroma_quality = 55;
    options.feature_flags = N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_CALIBRATED_CHROMA_QUANT;
    options.chroma_quality = 101;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_3;
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES;
    options.chroma_quality = 55;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    /* The calibrated table is independent of subsampling syntax. The
       reconstruction-aware path's
       reconstruction-aware reducer remains intentionally 4:2:0-only. */
    options.format_version = N148I_FORMAT_VERSION_4;
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES |
        N148I_FEATURE_CALIBRATED_CHROMA_QUANT;
    options.chroma_quality = 55;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420; chroma++) {
        options.chroma = (n148i_chroma_t) chroma;
        if (n148i_encode_memory(&source, &options, &compatibility,
                                &compatibility_size) != N148I_OK ||
            n148i_read_header(compatibility, compatibility_size,
                              &compatibility_info) != N148I_OK ||
            compatibility_info.chroma != (n148i_chroma_t) chroma ||
            compatibility_info.chroma_quality != 55 ||
            n148i_decode_memory(compatibility, compatibility_size,
                                &compatibility_decoded) != N148I_OK)
            goto cleanup;
        n148i_free_buffer(compatibility);
        compatibility = NULL;
        compatibility_size = 0;
        n148i_free_image(&compatibility_decoded);
    }
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "calibrated_chroma_quality_55_header_byte27_"
           "corruption_all_layouts_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           scalar_payload_size, scalar_payload_size,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(control); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(explicit_stream); n148i_free_buffer(compatibility);
    n148i_free_buffer(invalid); free(corrupt);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded); n148i_free_image(&explicit_decoded);
    n148i_free_image(&compatibility_decoded); n148i_free_image(&rejected);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_frame_segmentation(const Image *original,
                                    const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *control = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *compatibility = NULL;
    uint8_t *invalid = NULL, *corrupt = NULL;
    size_t control_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, compatibility_size = 0, invalid_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, compatibility_decoded = {0};
    n148i_image_t rejected = {0};
    n148i_image_info_t info = {0}, compatibility_info = {0};
    N148SyntaxInspection inspection = {0};
    uint32_t stored_segments = 0;
    uint64_t histogram_total = 0;
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_4;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 5;
    options.feature_flags = N148I_PROFILE_CALIBRATED_CHROMA_QUANT;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &control, &control_size) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_PROFILE_CHROMA_SEGMENTATION;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK || (scalar_size == control_size &&
            memcmp(scalar, control, scalar_size) == 0) ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_4 ||
        info.feature_flags != N148I_PROFILE_CHROMA_SEGMENTATION ||
        info.chroma_quality != 55 ||
        info.encoded_header_size + info.payload_size != scalar_size ||
        info.payload_size < 28 || scalar[info.encoded_header_size] != 7 ||
        scalar[info.encoded_header_size + 3] != 12 ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK ||
        !n148_segmented_inspect(
            scalar + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            scalar + info.encoded_header_size, info.payload_size,
            original->width, original->height, CHROMA_420,
            N148I_FORMAT_3_DEFAULT_FEATURES, 0, &inspection)) goto cleanup;

    stored_segments = validation_u32(
        scalar + info.encoded_header_size + 24);
    uint64_t expected_segments =
        (uint64_t)(original->width / N148_SEGMENT_REGION_SIZE +
            (original->width % N148_SEGMENT_REGION_SIZE != 0)) *
        (uint64_t)(original->height / N148_SEGMENT_REGION_SIZE +
            (original->height % N148_SEGMENT_REGION_SIZE != 0));
    for (int segment = 0; segment < N148_SEGMENT_COUNT; segment++)
        histogram_total += inspection.segment_histogram[segment];
    if (expected_segments > UINT32_MAX ||
        stored_segments != (uint32_t) expected_segments ||
        inspection.segment_count != expected_segments ||
        histogram_total != expected_segments) goto cleanup;
    for (int segment = 0; segment < N148_SEGMENT_COUNT; segment++) {
        if (inspection.segment_quant_scale[segment] <
                N148_SEGMENT_MIN_SCALE ||
            inspection.segment_quant_scale[segment] >
                N148_SEGMENT_MAX_SCALE ||
            scalar[info.encoded_header_size + 28 + segment] !=
                inspection.segment_quant_scale[segment]) goto cleanup;
    }

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    corrupt = (uint8_t *) malloc(scalar_size);
    if (!corrupt) goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[21] &= (uint8_t) ~0x80u;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[info.encoded_header_size] = 5;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[info.encoded_header_size + 3] &= (uint8_t) ~8u;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    validation_put_u32(corrupt + info.encoded_header_size + 24,
                       stored_segments + 1);
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[info.encoded_header_size + 28] = N148_SEGMENT_MIN_SCALE - 1;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;

    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_CHROMA_SEGMENTATION &
        ~N148I_FEATURE_CALIBRATED_CHROMA_QUANT;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_CHROMA_SEGMENTATION &
        ~N148I_FEATURE_CONTEXTUAL_RDO;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    /* Segmentation itself is independent of chroma layout. Excluding
       reconstruction-aware chroma
       permits this syntax check in 4:4:4 and 4:2:2 as well as 4:2:0. */
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES |
        N148I_FEATURE_CALIBRATED_CHROMA_QUANT |
        N148I_FEATURE_SEGMENTATION;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420; chroma++) {
        options.chroma = (n148i_chroma_t) chroma;
        if (n148i_encode_memory(&source, &options, &compatibility,
                                &compatibility_size) != N148I_OK ||
            n148i_read_header(compatibility, compatibility_size,
                              &compatibility_info) != N148I_OK ||
            compatibility_info.chroma != (n148i_chroma_t) chroma ||
            compatibility_info.feature_flags != options.feature_flags ||
            n148i_decode_memory(compatibility, compatibility_size,
                                &compatibility_decoded) != N148I_OK)
            goto cleanup;
        n148i_free_buffer(compatibility);
        compatibility = NULL;
        compatibility_size = 0;
        n148i_free_image(&compatibility_decoded);
    }
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "payload_revision_7_segments_%u_frame_cluster_map_"
           "corruption_all_layouts_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, stored_segments,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(control); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(compatibility); n148i_free_buffer(invalid); free(corrupt);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded);
    n148i_free_image(&compatibility_decoded); n148i_free_image(&rejected);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_segment_adaptive_filter(const Image *original,
                                       const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3,
    };
    n148i_encode_options_t options;
    uint8_t *control = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *compatibility = NULL;
    uint8_t *invalid = NULL, *corrupt = NULL;
    size_t control_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, compatibility_size = 0, invalid_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, compatibility_decoded = {0};
    n148i_image_t rejected = {0};
    n148i_image_info_t info = {0};
    N148SyntaxInspection inspection = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_4;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.effort = 5;
    options.feature_flags = N148I_PROFILE_CHROMA_SEGMENTATION;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &control, &control_size) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_PROFILE_ADAPTIVE_CHROMA_FILTER;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK || (scalar_size == control_size &&
            memcmp(scalar, control, scalar_size) == 0) ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != N148I_PROFILE_ADAPTIVE_CHROMA_FILTER ||
        info.encoded_header_size + info.payload_size != scalar_size ||
        !info.payload_size || scalar[info.encoded_header_size] != 9 ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK ||
        !n148_adaptive_filter_inspect(
            scalar + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            scalar + info.encoded_header_size, info.payload_size,
            original->width, original->height, CHROMA_420,
            N148I_FORMAT_3_DEFAULT_FEATURES, 0, &inspection) ||
        inspection.segment_count == 0) goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    corrupt = (uint8_t *) malloc(scalar_size);
    if (!corrupt) goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[22] &= (uint8_t) ~1u;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[info.encoded_header_size] = 7;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;

    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_ADAPTIVE_CHROMA_FILTER &
        ~N148I_FEATURE_SEGMENTATION;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_ADAPTIVE_CHROMA_FILTER &
        ~N148I_FEATURE_LOOP_FILTER;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    /* Reconstruction-aware chroma is the only 4:2:0-specific bit. The adaptive filter
       follows the full-resolution segment map for every chroma layout. */
    options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES |
        N148I_FEATURE_CALIBRATED_CHROMA_QUANT |
        N148I_FEATURE_SEGMENTATION |
        N148I_FEATURE_ADAPTIVE_LOOP_FILTER;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420; chroma++) {
        options.chroma = (n148i_chroma_t) chroma;
        if (n148i_encode_memory(&source, &options, &compatibility,
                                &compatibility_size) != N148I_OK ||
            n148i_decode_memory(compatibility, compatibility_size,
                                &compatibility_decoded) != N148I_OK)
            goto cleanup;
        n148i_free_buffer(compatibility);
        compatibility = NULL;
        compatibility_size = 0;
        n148i_free_image(&compatibility_decoded);
    }
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "payload_revision_9_segment_filter_levels_2_1_1_0_"
           "dependencies_corruption_all_layouts_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, success ? "PASS" : "FAIL");
    n148i_free_buffer(control); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(compatibility); n148i_free_buffer(invalid); free(corrupt);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded);
    n148i_free_image(&compatibility_decoded); n148i_free_image(&rejected);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_calibrated_luma_quantization(const Image *original, const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    uint8_t *format_4_stream = NULL, *format_5_control = NULL, *scalar = NULL;
    uint8_t *vector = NULL, *parallel = NULL, *invalid = NULL;
    size_t format_4_size = 0, format_5_control_size = 0, scalar_size = 0;
    size_t vector_size = 0, parallel_size = 0, invalid_size = 0;
    n148i_image_t format_4_decoded = {0}, control_decoded = {0};
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0}, rejected = {0};
    n148i_image_info_t info = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.feature_flags != N148I_DEFAULT_FEATURES ||
        options.effort != 3) goto cleanup;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_4;
    options.feature_flags = N148I_FORMAT_4_DEFAULT_FEATURES;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &format_4_stream,
                            &format_4_size) != N148I_OK ||
        n148i_decode_memory(format_4_stream, format_4_size,
                            &format_4_decoded) != N148I_OK)
        goto cleanup;

    /* Turning calibrated luma quantization off must reuse the exact accepted
       format 4 payload and
       reconstruction. Only the outer format byte is allowed to differ. */
    options.format_version = N148I_FORMAT_VERSION_5;
    if (n148i_encode_memory(&source, &options, &format_5_control,
                            &format_5_control_size) != N148I_OK ||
        format_5_control_size != format_4_size ||
        !same_stream_except_version(format_4_stream, format_5_control,
                                    format_4_size) ||
        n148i_decode_memory(format_5_control, format_5_control_size,
                            &control_decoded) != N148I_OK ||
        memcmp(format_4_decoded.pixels, control_decoded.pixels,
               format_4_decoded.stride * format_4_decoded.height) != 0)
        goto cleanup;

    options.feature_flags = N148I_PROFILE_CALIBRATED_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_5 ||
        info.feature_flags != N148I_PROFILE_CALIBRATED_LUMA_QUANT ||
        info.chroma_quality != 55 ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK)
        goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size,
                            &parallel_decoded) != N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_CALIBRATED_LUMA_QUANT &
        ~N148I_FEATURE_CONTEXTUAL_RDO;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    invalid = (uint8_t *) malloc(scalar_size);
    if (!invalid) goto cleanup;
    memcpy(invalid, scalar, scalar_size);
    invalid[23] |= 0x80u;
    if (n148i_decode_memory(invalid, scalar_size, &rejected) !=
            N148I_ERROR_UNSUPPORTED_FEATURE) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "format_4_payload_compatibility_calibrated_luma_feature_"
           "dependencies_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, success ? "PASS" : "FAIL");
    n148i_free_buffer(format_4_stream); n148i_free_buffer(format_5_control);
    n148i_free_buffer(scalar); n148i_free_buffer(vector);
    n148i_free_buffer(parallel); n148i_free_buffer(invalid);
    n148i_free_image(&format_4_decoded); n148i_free_image(&control_decoded);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded); n148i_free_image(&rejected);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_variable_luma_transform(const Image *original,
                                                const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    uint8_t *calibrated_luma = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *all_features = NULL;
    uint8_t *legacy_quant = NULL, *invalid = NULL, *corrupt = NULL;
    size_t calibrated_luma_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, all_features_size = 0;
    size_t legacy_quant_size = 0, invalid_size = 0;
    n148i_image_t calibrated_luma_decoded = {0}, scalar_decoded = {0};
    n148i_image_t vector_decoded = {0}, parallel_decoded = {0};
    n148i_image_t all_features_decoded = {0}, legacy_quant_decoded = {0};
    n148i_image_t rejected = {0};
    n148i_image_info_t info = {0};
    N148SyntaxInspection inspection = {0};
    uint32_t stored_transform_count = 0;
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_5;
    options.quality = 30;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_CALIBRATED_LUMA_QUANT;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &calibrated_luma,
                            &calibrated_luma_size) !=
            N148I_OK ||
        n148i_decode_memory(calibrated_luma, calibrated_luma_size,
                            &calibrated_luma_decoded) != N148I_OK)
        goto cleanup;

    options.feature_flags = N148I_PROFILE_VARIABLE_LUMA_TRANSFORM;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != N148I_PROFILE_VARIABLE_LUMA_TRANSFORM ||
        info.encoded_header_size + info.payload_size != scalar_size ||
        info.payload_size < 28 ||
        scalar[info.encoded_header_size] != 11 ||
        scalar[info.encoded_header_size + 3] != 6 ||
        !inspect_variable_luma_stream(scalar, scalar_size, original, options.chroma,
                           options.feature_flags, &inspection) ||
        inspection.luma_transform_regions == 0 ||
        inspection.luma_transform_16x16 == 0 ||
        inspection.luma_transform_16x16 >
            inspection.luma_transform_regions ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK ||
        scalar_decoded.width != source.width ||
        scalar_decoded.height != source.height)
        goto cleanup;
    stored_transform_count = validation_u32(
        scalar + info.encoded_header_size + 24);
    if (stored_transform_count != inspection.luma_transform_regions)
        goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size, &parallel_decoded) !=
            N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    /* The transform is independently switchable from calibrated luma
       table and remains composable with every retained opt-in experiment. */
    options.thread_count = 1;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK) goto cleanup;
    options.feature_flags = N148I_PROFILE_VARIABLE_LUMA_TRANSFORM &
        ~N148I_FEATURE_CALIBRATED_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &legacy_quant,
                            &legacy_quant_size) != N148I_OK ||
        n148i_decode_memory(legacy_quant, legacy_quant_size,
                            &legacy_quant_decoded) != N148I_OK)
        goto cleanup;
    options.feature_flags = N148I_FORMAT_5_FEATURE_MASK;
    if (n148i_encode_memory(&source, &options, &all_features,
                            &all_features_size) != N148I_OK ||
        n148i_decode_memory(all_features, all_features_size,
                            &all_features_decoded) != N148I_OK)
        goto cleanup;

    options.feature_flags = N148I_PROFILE_VARIABLE_LUMA_TRANSFORM &
        ~N148I_FEATURE_CONTEXTUAL_RDO;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_VARIABLE_LUMA_TRANSFORM &
        ~N148I_FEATURE_INTRA_4X4;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    corrupt = (uint8_t *) malloc(scalar_size);
    if (!corrupt) goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[info.encoded_header_size] = 5;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[info.encoded_header_size + 3] &= (uint8_t) ~2u;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    validation_put_u32(corrupt + info.encoded_header_size + 24,
                       stored_transform_count + 1u);
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[21] &= (uint8_t) ~4u;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "payload_revision_11_dct16_%llu_of_%llu_calibrated_luma_control_dependencies_"
           "counts_independent_switching_full_feature_composition_"
           "corruption_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           (unsigned long long) inspection.luma_transform_16x16,
           (unsigned long long) inspection.luma_transform_regions,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(calibrated_luma); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(all_features); n148i_free_buffer(legacy_quant);
    n148i_free_buffer(invalid); free(corrupt);
    n148i_free_image(&calibrated_luma_decoded);
    n148i_free_image(&scalar_decoded);
    n148i_free_image(&vector_decoded); n148i_free_image(&parallel_decoded);
    n148i_free_image(&all_features_decoded);
    n148i_free_image(&legacy_quant_decoded);
    n148i_free_image(&rejected);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_perceptual_luma_trellis(const Image *original,
                                                const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    uint8_t *control = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *layout = NULL, *invalid = NULL;
    size_t control_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, layout_size = 0, invalid_size = 0;
    n148i_image_t scalar_decoded = {0}, vector_decoded = {0};
    n148i_image_t parallel_decoded = {0};
    n148i_image_info_t info = {0};
    N148SyntaxInspection inspection = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_5;
    options.quality = 30;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_VARIABLE_LUMA_TRANSFORM;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &control, &control_size) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_PROFILE_PERCEPTUAL_LUMA_TRELLIS;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        (scalar_size == control_size &&
         memcmp(scalar, control, scalar_size) == 0) ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != N148I_PROFILE_PERCEPTUAL_LUMA_TRELLIS ||
        !inspect_variable_luma_stream(scalar, scalar_size, original, options.chroma,
                           options.feature_flags, &inspection) ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK)
        goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size,
                            &parallel_decoded) != N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    options.thread_count = 1;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_422;
         chroma++) {
        n148i_image_t decoded = {0};
        n148i_free_buffer(layout);
        layout = NULL;
        layout_size = 0;
        options.chroma = (n148i_chroma_t) chroma;
        int layout_ok = n148i_encode_memory(
                &source, &options, &layout, &layout_size) == N148I_OK &&
            n148i_decode_memory(layout, layout_size, &decoded) == N148I_OK;
        n148i_free_image(&decoded);
        if (!layout_ok) goto cleanup;
    }

    options.chroma = N148I_CHROMA_420;
    options.feature_flags = N148I_PROFILE_PERCEPTUAL_LUMA_TRELLIS &
        ~N148I_FEATURE_CONTEXTUAL_RDO;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,0+1+2,%d,%d,%zu,%u,%u,0.0,"
           "perceptual_luma_trellis_opt_in_all_layouts_"
           "dependencies_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, success ? "PASS" : "FAIL");
    n148i_free_buffer(control); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(layout); n148i_free_buffer(invalid);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&parallel_decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_quant_adaptive_luma_filter(const Image *original,
                                                   const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    uint8_t *control = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *parallel = NULL, *layout = NULL, *same_control = NULL;
    uint8_t *same_candidate = NULL, *invalid = NULL;
    size_t control_size = 0, scalar_size = 0, vector_size = 0;
    size_t parallel_size = 0, layout_size = 0, same_control_size = 0;
    size_t same_candidate_size = 0, invalid_size = 0;
    n148i_image_t control_decoded = {0}, scalar_decoded = {0};
    n148i_image_t vector_decoded = {0}, parallel_decoded = {0};
    n148i_image_t same_control_decoded = {0}, same_candidate_decoded = {0};
    n148i_image_info_t info = {0};
    N148SyntaxInspection inspection = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.format_version = N148I_FORMAT_VERSION_5;
    options.quality = 30;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_VARIABLE_LUMA_TRANSFORM;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &control, &control_size) !=
            N148I_OK ||
        n148i_decode_memory(control, control_size, &control_decoded) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_PROFILE_QUANT_ADAPTIVE_LUMA_FILTER;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.feature_flags != N148I_PROFILE_QUANT_ADAPTIVE_LUMA_FILTER ||
        !inspect_variable_luma_stream(scalar, scalar_size, original, options.chroma,
                           options.feature_flags, &inspection) ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) != N148I_OK ||
        memcmp(control_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) == 0 ||
        n148_loop_filter_quant_luma_strength(30) != 5 ||
        n148_loop_filter_quant_luma_strength(45) != 3 ||
        n148_loop_filter_quant_luma_strength(60) != 2 ||
        n148_loop_filter_quant_luma_strength(75) != 2 ||
        n148_loop_filter_quant_luma_strength(90) != 1 ||
        n148_loop_filter_quant_luma_strength(96) != 0 ||
        n148_loop_filter_quant_luma_strength(0) != -1) goto cleanup;

    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) != N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;
    options.thread_count = 4;
    if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size,
                            &parallel_decoded) != N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    options.thread_count = 1;
    for (int chroma = N148I_CHROMA_444; chroma <= N148I_CHROMA_420;
         chroma++) {
        n148i_image_t decoded = {0};
        n148i_free_buffer(layout);
        layout = NULL;
        layout_size = 0;
        options.chroma = (n148i_chroma_t) chroma;
        int layout_ok = n148i_encode_memory(
                &source, &options, &layout, &layout_size) == N148I_OK &&
            n148i_decode_memory(layout, layout_size, &decoded) == N148I_OK;
        n148i_free_image(&decoded);
        if (!layout_ok) goto cleanup;
    }

    options.chroma = N148I_CHROMA_420;
    options.quality = 60;
    options.feature_flags = N148I_PROFILE_VARIABLE_LUMA_TRANSFORM;
    if (n148i_encode_memory(
            &source, &options, &same_control, &same_control_size) != N148I_OK ||
        n148i_decode_memory(same_control, same_control_size,
                            &same_control_decoded) != N148I_OK)
        goto cleanup;
    options.feature_flags = N148I_PROFILE_QUANT_ADAPTIVE_LUMA_FILTER;
    if (n148i_encode_memory(
            &source, &options, &same_candidate,
            &same_candidate_size) != N148I_OK ||
        same_candidate_size != same_control_size ||
        n148i_decode_memory(same_candidate, same_candidate_size,
                            &same_candidate_decoded) != N148I_OK ||
        memcmp(same_control_decoded.pixels, same_candidate_decoded.pixels,
               same_control_decoded.stride *
                   same_control_decoded.height) != 0)
        goto cleanup;

    options.quality = 30;
    options.feature_flags = N148I_PROFILE_QUANT_ADAPTIVE_LUMA_FILTER &
        ~N148I_FEATURE_LOOP_FILTER;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    n148i_free_buffer(invalid);
    invalid = NULL;
    invalid_size = 0;
    options.feature_flags = N148I_PROFILE_QUANT_ADAPTIVE_LUMA_FILTER &
        ~N148I_FEATURE_CALIBRATED_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,0+1+2,%d,%d,%zu,%u,%u,0.0,"
           "strengths_5_3_2_2_1_q60_exact_reconstruction_all_layouts_"
           "dependencies_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, success ? "PASS" : "FAIL");
    n148i_free_buffer(control); n148i_free_buffer(scalar);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(layout); n148i_free_buffer(same_control);
    n148i_free_buffer(same_candidate); n148i_free_buffer(invalid);
    n148i_free_image(&control_decoded); n148i_free_image(&scalar_decoded);
    n148i_free_image(&vector_decoded); n148i_free_image(&parallel_decoded);
    n148i_free_image(&same_control_decoded);
    n148i_free_image(&same_candidate_decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_automatic_chroma(const Image *original,
                                         const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    Image detail = {0};
    n148i_image_t detail_source = {0};
    n148i_encode_options_t options;
    uint8_t *fast = NULL, *fixed = NULL, *scalar = NULL, *explicit = NULL;
    uint8_t *vector = NULL, *parallel = NULL, *detail_stream = NULL;
    uint8_t *invalid = NULL;
    size_t fast_size = 0, fixed_size = 0, scalar_size = 0;
    size_t explicit_size = 0, vector_size = 0, parallel_size = 0;
    size_t detail_size = 0, invalid_size = 0;
    n148i_image_t decoded = {0}, detail_decoded = {0};
    n148i_image_info_t fast_info = {0}, info = {0}, detail_info = {0};
    int success = 0;

    n148i_encode_options_init(&options);
    options.quality = 50;
    options.chroma = N148I_CHROMA_AUTO;
    options.effort = 0;
    options.thread_count = 1;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &fast, &fast_size) != N148I_OK ||
        n148i_read_header(fast, fast_size, &fast_info) != N148I_OK ||
        fast_info.chroma != N148I_CHROMA_420) goto cleanup;

    options.chroma = N148I_CHROMA_420;
    if (n148i_encode_memory(&source, &options, &fixed, &fixed_size) !=
            N148I_OK || fixed_size != fast_size ||
        memcmp(fixed, fast, fast_size) != 0) goto cleanup;

    options.chroma = N148I_CHROMA_AUTO;
    options.format_version = N148I_FORMAT_VERSION_4;
    options.feature_flags = N148I_FORMAT_4_DEFAULT_FEATURES;
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;

    options.format_version = N148I_FORMAT_VERSION_5;
    options.feature_flags = N148I_PROFILE_CALIBRATED_LUMA_QUANT;
    options.quality = 75;
    options.effort = 5;
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.chroma < N148I_CHROMA_444 ||
        info.chroma > N148I_CHROMA_420 ||
        n148i_decode_memory(scalar, scalar_size, &decoded) != N148I_OK)
        goto cleanup;

    /* AUTO must be exactly one of the three independently usable streams;
       no encoder-only sentinel may leak into the file header. */
    options.chroma = info.chroma;
    if (n148i_encode_memory(&source, &options, &explicit, &explicit_size) !=
            N148I_OK || explicit_size != scalar_size ||
        memcmp(explicit, scalar, scalar_size) != 0) goto cleanup;

    options.chroma = N148I_CHROMA_AUTO;
    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0) goto cleanup;
    options.thread_count = 4;
    if (n148i_encode_memory(&source, &options, &parallel, &parallel_size) !=
            N148I_OK || parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0) goto cleanup;

    fill_chroma_detail_image(&detail);
    if (!detail.pixels) goto cleanup;
    detail_source.pixels = detail.pixels;
    detail_source.width = (uint32_t) detail.width;
    detail_source.height = (uint32_t) detail.height;
    detail_source.stride = (size_t) detail.width * 3u;
    options.thread_count = 1;
    options.quality = 95;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&detail_source, &options,
                            &detail_stream, &detail_size) != N148I_OK ||
        n148i_read_header(detail_stream, detail_size, &detail_info) !=
            N148I_OK || detail_info.chroma != N148I_CHROMA_444 ||
        n148i_decode_memory(detail_stream, detail_size,
                            &detail_decoded) != N148I_OK) goto cleanup;
    success = 1;

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,3,%d,%d,%zu,%u,%u,0.0,"
           "effort0_exact_420_auto_is_concrete_best_candidate_"
           "high_frequency_fixture_selects_444_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size, success ? "PASS" : "FAIL");
    n148i_free_buffer(fast); n148i_free_buffer(fixed);
    n148i_free_buffer(scalar); n148i_free_buffer(explicit);
    n148i_free_buffer(vector); n148i_free_buffer(parallel);
    n148i_free_buffer(detail_stream); n148i_free_buffer(invalid);
    n148i_free_image(&decoded); n148i_free_image(&detail_decoded);
    free_image(&detail);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_fidelity_tools(const Image *original, const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    uint8_t *format_5_stream = NULL, *compatibility = NULL, *scalar = NULL;
    uint8_t *vector = NULL, *combined = NULL, *invalid = NULL;
    size_t format_5_size = 0, compatibility_size = 0, scalar_size = 0;
    size_t vector_size = 0, combined_size = 0, invalid_size = 0;
    n148i_image_t compatibility_decoded = {0}, scalar_decoded = {0};
    n148i_image_t vector_decoded = {0}, combined_decoded = {0};
    n148i_image_info_t info = {0};
    N148SyntaxInspection inspection = {0};
    int32_t impulse[32 * 32] = {0};
    int32_t transformed[32 * 32], reconstructed[32 * 32];
    const char *failure = "initialization";
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.feature_flags != N148I_DEFAULT_FEATURES ||
        options.effort != 3 ||
        n148_variable_transform_matrix32_hash() !=
            0xb27f5da0e4d360b1ull) goto cleanup;
    impulse[0] = 127;
    impulse[31] = -63;
    impulse[32 * 31] = 31;
    if (!n148_variable_forward(impulse, 32, transformed) ||
        !n148_variable_inverse(transformed, 32, reconstructed)) goto cleanup;
    for (int index = 0; index < 32 * 32; index++)
        if (abs(reconstructed[index] - impulse[index]) > 1) goto cleanup;

    options.quality = 30;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_5;
    options.feature_flags = N148I_FORMAT_5_DEFAULT_FEATURES;
    failure = "format 5 reference encode";
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &format_5_stream,
                            &format_5_size) != N148I_OK)
        goto cleanup;

    options.format_version = N148I_FORMAT_VERSION_6;
    failure = "format 6 compatibility encode/decode";
    if (n148i_encode_memory(&source, &options, &compatibility,
                            &compatibility_size) != N148I_OK ||
        compatibility_size != format_5_size ||
        !same_stream_except_version(format_5_stream, compatibility,
                                    format_5_size) ||
        n148i_decode_memory(compatibility, compatibility_size,
                            &compatibility_decoded) != N148I_OK)
        goto cleanup;

    options.feature_flags = N148I_PROFILE_LUMA_TRANSFORM_32X32;
    failure = "format 6 dct32 encode/decode/inspect";
    if (n148i_encode_memory(&source, &options, &scalar, &scalar_size) !=
            N148I_OK) goto cleanup;
    failure = "format 6 dct32 header";
    if (n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_6 ||
        info.feature_flags != N148I_PROFILE_LUMA_TRANSFORM_32X32) goto cleanup;
    failure = "format 6 dct32 decode";
    if (n148i_decode_memory(scalar, scalar_size, &scalar_decoded) !=
            N148I_OK) goto cleanup;
    failure = "format 6 dct32 inspect";
    if (!n148_predictive_inspect(
            scalar + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            scalar + info.encoded_header_size, info.payload_size,
            original->width, original->height, N148I_CHROMA_420,
            info.feature_flags & N148_FORMAT_3_SUPPORTED_FEATURES, 0, 0, 0, 1,
            1, &inspection)) goto cleanup;

    options.thread_count = 4;
    failure = "format 6 dct32 simd/thread determinism";
    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector, &vector_size) !=
            N148I_OK || vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0 ||
        n148i_decode_memory(vector, vector_size, &vector_decoded) !=
            N148I_OK ||
        memcmp(vector_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_COMBINED_INTRA_PREDICTION;
    failure = "format 6 combined prediction roundtrip";
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &combined, &combined_size) !=
            N148I_OK ||
        n148i_decode_memory(combined, combined_size, &combined_decoded) !=
            N148I_OK) goto cleanup;

    options.feature_flags = N148I_PROFILE_LUMA_TRANSFORM_32X32 &
        ~N148I_FEATURE_LUMA_TRANSFORM_16X16;
    failure = "format 6 dct32 dependency on dct16";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_LUMA_TRANSFORM_32X32 &
        ~N148I_FEATURE_CALIBRATED_LUMA_QUANT;
    failure = "format 6 dct32 dependency on calibrated luma";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_5;
    options.feature_flags = N148I_PROFILE_LUMA_TRANSFORM_32X32;
    failure = "format 5 rejects format 6 features";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_UNSUPPORTED_FEATURE) goto cleanup;
    success = 1;
    failure = "none";

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "format_6_compatibility_dct32_%llu_combined_prediction_"
           "dependencies_matrix_hash_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           (unsigned long long) inspection.luma_transform_32x32,
           success ? "PASS" : "FAIL");
    if (!success) fprintf(stderr, "format 6 fidelity failure: %s\n", failure);
    n148i_free_buffer(format_5_stream); n148i_free_buffer(compatibility);
    n148i_free_buffer(scalar); n148i_free_buffer(vector);
    n148i_free_buffer(combined); n148i_free_buffer(invalid);
    n148i_free_image(&compatibility_decoded);
    n148i_free_image(&scalar_decoded); n148i_free_image(&vector_decoded);
    n148i_free_image(&combined_decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_adaptive_rich_context(const Image *original,
                                         const char *name) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    uint8_t *format_6_stream = NULL, *compatibility = NULL, *adaptive = NULL;
    uint8_t *scalar = NULL, *parallel = NULL, *invalid = NULL;
    uint8_t *multiple = NULL, *multiple_parallel = NULL;
    uint8_t *corrupt = NULL;
    size_t format_6_size = 0, compatibility_size = 0, adaptive_size = 0;
    size_t scalar_size = 0, parallel_size = 0, invalid_size = 0;
    size_t multiple_size = 0, multiple_parallel_size = 0;
    n148i_image_t format_6_decoded = {0}, compatibility_decoded = {0};
    n148i_image_t adaptive_decoded = {0}, scalar_decoded = {0};
    n148i_image_t parallel_decoded = {0}, rejected = {0};
    n148i_image_t multiple_decoded = {0};
    n148i_image_info_t info = {0};
    N148SyntaxInspection inspection = {0};
    const char *failure = "initialization";
    int success = 0;

    n148i_encode_options_init(&options);
    if (options.format_version != N148I_FORMAT_VERSION_7 ||
        options.feature_flags != N148I_DEFAULT_FEATURES ||
        options.effort != 3) goto cleanup;
    options.quality = 50;
    options.chroma = N148I_CHROMA_420;
    options.thread_count = 1;
    options.format_version = N148I_FORMAT_VERSION_6;
    options.feature_flags = N148I_FORMAT_6_DEFAULT_FEATURES;
    failure = "format 6 reference encode/decode";
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &format_6_stream,
                            &format_6_size) != N148I_OK ||
        n148i_decode_memory(format_6_stream, format_6_size,
                            &format_6_decoded) != N148I_OK)
        goto cleanup;

    /* With every format 7 tool disabled, format 7 must be an exact syntax
       wrapper around the accepted format 6 stream. */
    options.format_version = N148I_FORMAT_VERSION_7;
    failure = "format 7 compatibility stream";
    if (n148i_encode_memory(&source, &options, &compatibility,
                            &compatibility_size) != N148I_OK ||
        compatibility_size != format_6_size ||
        !same_stream_except_version(format_6_stream, compatibility,
                                    format_6_size) ||
        n148i_decode_memory(compatibility, compatibility_size,
                            &compatibility_decoded) != N148I_OK ||
        memcmp(format_6_decoded.pixels, compatibility_decoded.pixels,
               format_6_decoded.stride * format_6_decoded.height) != 0)
        goto cleanup;

    options.feature_flags = N148I_PROFILE_ADAPTIVE_ENTROPY;
    failure = "adaptive-only roundtrip";
    if (n148i_encode_memory(&source, &options, &adaptive,
                            &adaptive_size) != N148I_OK ||
        n148i_decode_memory(adaptive, adaptive_size,
                            &adaptive_decoded) != N148I_OK ||
        memcmp(format_6_decoded.pixels, adaptive_decoded.pixels,
               format_6_decoded.stride * format_6_decoded.height) != 0)
        goto cleanup;

    options.feature_flags = N148I_PROFILE_RICH_COEFFICIENT_CONTEXTS;
    /* Effort 3 may choose different intra modes in the rich format 7 profile.
       Validate its syntax and reconstruction independently of format 6. */
    failure = "rich adaptive encode/header/decode";
    if (n148i_encode_memory(&source, &options, &scalar,
                            &scalar_size) != N148I_OK ||
        scalar_size == 0 ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_7 ||
        info.feature_flags != N148I_PROFILE_RICH_COEFFICIENT_CONTEXTS ||
        n148i_decode_memory(scalar, scalar_size, &scalar_decoded) !=
            N148I_OK ||
        scalar_decoded.width != source.width ||
        scalar_decoded.height != source.height ||
        !n148_inspect_format_7(
            scalar + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            scalar + info.encoded_header_size, info.payload_size,
            original->width, original->height, N148I_CHROMA_420,
            info.feature_flags, &inspection))
        goto cleanup;

    options.thread_count = 4;
    failure = "format 7 SIMD/thread determinism";
    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &parallel,
                            &parallel_size) != N148I_OK ||
        parallel_size != scalar_size ||
        memcmp(parallel, scalar, scalar_size) != 0 ||
        n148i_decode_memory(parallel, parallel_size,
                            &parallel_decoded) != N148I_OK ||
        memcmp(parallel_decoded.pixels, scalar_decoded.pixels,
               scalar_decoded.stride * scalar_decoded.height) != 0)
        goto cleanup;

    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_RICH_COEFFICIENT_CONTEXTS &
        ~N148I_FEATURE_ADAPTIVE_ENTROPY;
    failure = "rich-context dependency";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_MULTIPLE_INTRA_REFERENCES;
    failure = "multiple-reference encode/header/decode/inspection";
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &multiple, &multiple_size) !=
            N148I_OK ||
        n148i_read_header(multiple, multiple_size, &info) != N148I_OK ||
        info.feature_flags != N148I_PROFILE_MULTIPLE_INTRA_REFERENCES ||
        n148i_decode_memory(multiple, multiple_size, &multiple_decoded) !=
            N148I_OK ||
        !n148_inspect_format_7(
            multiple + N148_EXTENDED_HEADER_SIZE, info.metadata_size,
            multiple + info.encoded_header_size, info.payload_size,
            original->width, original->height, N148I_CHROMA_420,
            info.feature_flags, &inspection) ||
        inspection.luma_reference_histogram[0] +
            inspection.luma_reference_histogram[1] +
            inspection.luma_reference_histogram[2] == 0)
        goto cleanup;

    options.thread_count = 4;
    failure = "multiple-reference SIMD/thread determinism";
    if (n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &multiple_parallel,
                            &multiple_parallel_size) != N148I_OK ||
        multiple_parallel_size != multiple_size ||
        memcmp(multiple_parallel, multiple, multiple_size) != 0)
        goto cleanup;

    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_MULTIPLE_INTRA_REFERENCES &
        ~N148I_FEATURE_RICH_COEFFICIENT_CONTEXTS;
    failure = "multiple-reference rich-context dependency";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_MULTIPLE_INTRA_REFERENCES &
        ~N148I_FEATURE_CONTEXTUAL_RDO;
    failure = "multiple-reference RDO dependency";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_INVALID_ARGUMENT) goto cleanup;
    options.feature_flags = N148I_PROFILE_DIRECTIONAL_SCAN;
    failure = "reserved directional-scan rejection";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_UNSUPPORTED_FEATURE) goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_6;
    options.feature_flags = N148I_PROFILE_RICH_COEFFICIENT_CONTEXTS;
    failure = "format 6 rejects format 7 features";
    if (n148i_encode_memory(&source, &options, &invalid, &invalid_size) !=
            N148I_ERROR_UNSUPPORTED_FEATURE) goto cleanup;

    failure = "truncated/corrupt format 7 rejection";
    if (n148i_decode_memory(scalar, scalar_size - 1, &rejected) !=
            N148I_ERROR_TRUNCATED_DATA) goto cleanup;
    corrupt = (uint8_t *) malloc(scalar_size);
    if (!corrupt) goto cleanup;
    memcpy(corrupt, scalar, scalar_size);
    corrupt[N148_EXTENDED_HEADER_SIZE] = 0xffu;
    if (n148i_decode_memory(corrupt, scalar_size, &rejected) == N148I_OK)
        goto cleanup;
    success = 1;
    failure = "none";

cleanup:
    printf("comparison,%s,scalar+AVX2,1+4,2,%d,%d,%zu,%u,%u,0.0,"
           "format_7_format_6_compatibility_adaptive_rich_context_state_"
           "fallback_multiple_references_dependencies_and_determinism,%s\n",
           name, original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           success ? "PASS" : "FAIL");
    if (!success)
        fprintf(stderr, "format 7 adaptive-context failure: %s\n", failure);
    n148i_free_buffer(format_6_stream); n148i_free_buffer(compatibility);
    n148i_free_buffer(adaptive); n148i_free_buffer(scalar);
    n148i_free_buffer(parallel); n148i_free_buffer(invalid);
    n148i_free_buffer(multiple); n148i_free_buffer(multiple_parallel);
    free(corrupt);
    n148i_free_image(&format_6_decoded);
    n148i_free_image(&compatibility_decoded);
    n148i_free_image(&adaptive_decoded);
    n148i_free_image(&scalar_decoded);
    n148i_free_image(&parallel_decoded);
    n148i_free_image(&multiple_decoded);
    n148i_free_image(&rejected);
    n148i_simd_force(N148I_SIMD_AUTO);
    n148_cpu_force(-1);
    n148_set_thread_count(1);
    return success;
}

static int validate_format_7_full_trellis(const Image *original) {
    n148i_image_t source = {
        original->pixels, (uint32_t)original->width,
        (uint32_t)original->height, (size_t)original->width * 3u,
    };
    n148i_encode_options_t options;
    n148i_encode_options_init(&options);
    options.quality = 60;
    options.thread_count = 1;
    options.feature_flags = N148I_FORMAT_7_FULL_TRELLIS_FEATURES;
    uint8_t *scalar = NULL, *vector = NULL;
    size_t scalar_size = 0, vector_size = 0;
    n148i_image_t decoded = {0};
    n148i_image_info_t info = {0};
    int success =
        n148i_simd_force(N148I_SIMD_SCALAR) == N148I_OK &&
        n148i_encode_memory(&source, &options, &scalar,
                            &scalar_size) == N148I_OK &&
        n148i_read_header(scalar, scalar_size, &info) == N148I_OK &&
        info.feature_flags == N148I_FORMAT_7_FULL_TRELLIS_FEATURES &&
        n148i_simd_force(N148I_SIMD_AVX2) == N148I_OK &&
        n148i_encode_memory(&source, &options, &vector,
                            &vector_size) == N148I_OK &&
        scalar_size == vector_size &&
        memcmp(scalar, vector, scalar_size) == 0 &&
        n148i_decode_memory(vector, vector_size, &decoded) == N148I_OK &&
        decoded.width == source.width && decoded.height == source.height;
    printf("comparison,format_7_full_trellis,scalar+AVX2,1,2,%d,%d,%zu,"
           "%u,%u,0.0,explicit_full_trellis_roundtrip_and_determinism,%s\n",
           original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(scalar);
    n148i_free_buffer(vector);
    n148i_free_image(&decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    return success;
}

static int validate_format_7_spectral_luma(const Image *original) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    n148i_encode_options_init(&options);
    options.quality = 60;
    options.thread_count = 1;
    uint8_t *baseline = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *invalid = NULL;
    size_t baseline_size = 0, scalar_size = 0, vector_size = 0;
    size_t invalid_size = 0;
    n148i_image_t baseline_rgb = {0}, spectral_rgb = {0};
    n148i_image_info_t info = {0};
    int success = 0;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &baseline,
                            &baseline_size) != N148I_OK ||
        n148i_decode_memory(baseline, baseline_size, &baseline_rgb) !=
            N148I_OK) goto cleanup;
    options.feature_flags = N148I_PROFILE_SPECTRAL_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &scalar,
                            &scalar_size) != N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_7 ||
        info.feature_flags != N148I_PROFILE_SPECTRAL_LUMA_QUANT ||
        n148i_decode_memory(scalar, scalar_size, &spectral_rgb) != N148I_OK ||
        spectral_rgb.width != source.width ||
        spectral_rgb.height != source.height ||
        memcmp(baseline_rgb.pixels, spectral_rgb.pixels,
               spectral_rgb.stride * spectral_rgb.height) == 0 ||
        n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector,
                            &vector_size) != N148I_OK ||
        vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0)
        goto cleanup;
    options.feature_flags &= ~N148I_FEATURE_CALIBRATED_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &invalid,
                            &invalid_size) != N148I_ERROR_INVALID_ARGUMENT)
        goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_6;
    options.feature_flags = N148I_FORMAT_6_DEFAULT_FEATURES |
        N148I_FORMAT_7_SPECTRAL_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &invalid,
                            &invalid_size) != N148I_ERROR_UNSUPPORTED_FEATURE)
        goto cleanup;
    success = 1;
cleanup:
    printf("comparison,format_7_spectral_luma,scalar+AVX2,1,2,%d,%d,%zu,"
           "%u,%u,0.0,flagged_table_roundtrip_old_profile_isolated_"
           "dependencies_and_determinism,%s\n",
           original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(baseline);
    n148i_free_buffer(scalar);
    n148i_free_buffer(vector);
    n148i_free_buffer(invalid);
    n148i_free_image(&baseline_rgb);
    n148i_free_image(&spectral_rgb);
    n148i_simd_force(N148I_SIMD_AUTO);
    return success;
}

static int validate_format_7_refined_spectral_luma(const Image *original) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    n148i_encode_options_init(&options);
    options.quality = 60;
    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_SPECTRAL_LUMA_QUANT;
    uint8_t *spectral = NULL, *scalar = NULL, *vector = NULL;
    uint8_t *invalid = NULL;
    size_t spectral_size = 0, scalar_size = 0, vector_size = 0;
    size_t invalid_size = 0;
    n148i_image_t spectral_rgb = {0}, refined_rgb = {0};
    n148i_image_info_t info = {0};
    int success = 0;
    if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
        n148i_encode_memory(&source, &options, &spectral,
                            &spectral_size) != N148I_OK ||
        n148i_decode_memory(spectral, spectral_size, &spectral_rgb) !=
            N148I_OK) goto cleanup;
    options.feature_flags = N148I_PROFILE_REFINED_SPECTRAL_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &scalar,
                            &scalar_size) != N148I_OK ||
        n148i_read_header(scalar, scalar_size, &info) != N148I_OK ||
        info.format_version != N148I_FORMAT_VERSION_7 ||
        info.feature_flags != N148I_PROFILE_REFINED_SPECTRAL_LUMA_QUANT ||
        n148i_decode_memory(scalar, scalar_size, &refined_rgb) != N148I_OK ||
        refined_rgb.width != source.width ||
        refined_rgb.height != source.height ||
        memcmp(spectral_rgb.pixels, refined_rgb.pixels,
               refined_rgb.stride * refined_rgb.height) == 0 ||
        n148i_simd_force(N148I_SIMD_AVX2) != N148I_OK ||
        n148i_encode_memory(&source, &options, &vector,
                            &vector_size) != N148I_OK ||
        vector_size != scalar_size ||
        memcmp(vector, scalar, scalar_size) != 0)
        goto cleanup;
    options.feature_flags &= ~N148I_FORMAT_7_SPECTRAL_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &invalid,
                            &invalid_size) != N148I_ERROR_INVALID_ARGUMENT)
        goto cleanup;
    options.feature_flags = N148I_PROFILE_REFINED_SPECTRAL_LUMA_QUANT &
        ~N148I_FEATURE_CALIBRATED_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &invalid,
                            &invalid_size) != N148I_ERROR_INVALID_ARGUMENT)
        goto cleanup;
    options.format_version = N148I_FORMAT_VERSION_6;
    options.feature_flags = N148I_FORMAT_6_DEFAULT_FEATURES |
        N148I_FORMAT_7_REFINED_SPECTRAL_LUMA_QUANT;
    if (n148i_encode_memory(&source, &options, &invalid,
                            &invalid_size) != N148I_ERROR_UNSUPPORTED_FEATURE)
        goto cleanup;
    success = 1;
cleanup:
    printf("comparison,format_7_refined_spectral_luma,scalar+AVX2,1,2,%d,%d,%zu,"
           "%u,%u,0.0,refined_table_roundtrip_old_spectral_isolated_"
           "dependencies_and_determinism,%s\n",
           original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(spectral);
    n148i_free_buffer(scalar);
    n148i_free_buffer(vector);
    n148i_free_buffer(invalid);
    n148i_free_image(&spectral_rgb);
    n148i_free_image(&refined_rgb);
    n148i_simd_force(N148I_SIMD_AUTO);
    return success;
}

static int validate_format_7_structural_luma(void) {
    const int widths[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33};
    const int heights[] = {1, 3, 2, 8, 7, 9, 16, 15, 19, 32, 31, 35};
    const int qualities[] = {1, 30, 60, 100};
    const int efforts[] = {0, 3, 5, 9};
    const uint32_t required[] = {
        N148I_FORMAT_7_REFINED_SPECTRAL_LUMA_QUANT,
        N148I_FEATURE_SEGMENTATION, N148I_FEATURE_CONTEXTUAL_RDO,
        N148I_FEATURE_CALIBRATED_CHROMA_QUANT,
        N148I_FEATURE_LUMA_TRANSFORM_16X16,
    };
    int completed = 0;
    for (int trial = 0; trial < 24; trial++) {
        int width = widths[trial % 12], height = heights[trial % 12];
        size_t row = (size_t)width * 3, stride = row + 13;
        size_t pixel_bytes = row * (size_t)height;
        size_t padded_bytes = stride * (size_t)(height - 1) + row;
        uint8_t *tight = malloc(pixel_bytes), *padded = malloc(padded_bytes);
        uint8_t *encoded[2] = {NULL, NULL};
        size_t sizes[2] = {0, 0};
        n148i_image_t decoded[2] = {{0}, {0}};
        n148i_image_info_t info = {0};
        int success = 0;
        if (!tight || !padded) goto cleanup;
        uint32_t random = 731871u + (uint32_t)trial;
        for (size_t i = 0; i < pixel_bytes; i++) {
            random = random * 1664525u + 1013904223u;
            tight[i] = trial % 3 ? (uint8_t)(random >> 24) :
                (uint8_t)((i % 7 < 3) ? 0 : 255);
        }
        memset(padded, 0xa5, padded_bytes);
        for (int y = 0; y < height; y++)
            memcpy(padded + (size_t)y * stride, tight + (size_t)y * row, row);
        n148i_image_t source[2] = {
            {tight, (uint32_t)width, (uint32_t)height, trial % 2 ? row : 0},
            {padded, (uint32_t)width, (uint32_t)height, stride},
        };
        n148i_encode_options_t options;
        n148i_encode_options_init(&options);
        options.feature_flags = N148I_PROFILE_STRUCTURAL_LUMA_QUANT;
        options.quality = qualities[trial % 4];
        options.chroma = (n148i_chroma_t)(trial / 6);
        options.effort = efforts[(trial / 3) % 4];
        options.thread_count = 1;
        if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
            n148i_encode_memory(&source[0], &options, &encoded[0], &sizes[0]) != N148I_OK ||
            n148i_read_header(encoded[0], sizes[0], &info) != N148I_OK ||
            !(info.feature_flags & N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT) ||
            info.chroma_quality != options.quality ||
            n148i_decode_memory(encoded[0], sizes[0], &decoded[0]) != N148I_OK)
            goto cleanup;
        options.chroma_quality = options.quality;
        if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
            n148i_encode_memory(&source[1], &options, &encoded[1], &sizes[1]) != N148I_OK ||
            sizes[0] != sizes[1] || memcmp(encoded[0], encoded[1], sizes[0]) ||
            n148i_decode_memory(encoded[1], sizes[1], &decoded[1]) != N148I_OK ||
            decoded[0].width != (uint32_t)width || decoded[0].height != (uint32_t)height ||
            decoded[1].width != (uint32_t)width || decoded[1].height != (uint32_t)height ||
            memcmp(decoded[0].pixels, decoded[1].pixels, pixel_bytes))
            goto cleanup;
        if (trial == 0) {
            uint32_t original_flags = info.feature_flags;
            for (size_t j = 0; j < sizeof(required) / sizeof(required[0]); j++) {
                uint32_t invalid_flags = original_flags & ~required[j];
                for (int byte = 0; byte < 4; byte++)
                    encoded[0][20 + byte] = (uint8_t)(invalid_flags >> (8 * byte));
                n148i_image_info_t invalid_info = {0};
                n148i_image_t invalid_image = {0};
                int header_status = n148i_read_header(encoded[0], sizes[0], &invalid_info);
                int decode_status = n148i_decode_memory(encoded[0], sizes[0], &invalid_image);
                n148i_free_image(&invalid_image);
                if (header_status == N148I_OK || decode_status == N148I_OK)
                    goto cleanup;
            }
            for (int byte = 0; byte < 4; byte++)
                encoded[0][20 + byte] = (uint8_t)(original_flags >> (8 * byte));
        }
        success = 1;
cleanup:
        for (int i = 0; i < 2; i++) {
            n148i_free_buffer(encoded[i]);
            n148i_free_image(&decoded[i]);
        }
        free(tight);
        free(padded);
        n148i_simd_force(N148I_SIMD_AUTO);
        if (!success) break;
        completed++;
    }
    printf("comparison,format_7_structural_luma,scalar+auto,1,2,33,35,0,0,0,0.0,"
           "24_odd_stride_chroma_effort_pairs_extreme_quality_dependencies,%s\n",
           completed == 24 ? "PASS" : "FAIL");
    return completed == 24;
}

static int validate_format_7_reconstruction(uint32_t profile, int detail) {
    const int widths[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33};
    const int heights[] = {1, 3, 2, 8, 7, 9, 16, 15, 19, 32, 31, 35};
    const int qualities[] = {1, 30, 60, 100};
    const int efforts[] = {0, 3, 5, 9};
    const uint32_t required[] = {
        N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT,
        N148I_FORMAT_7_REFINED_SPECTRAL_LUMA_QUANT,
        N148I_FEATURE_SEGMENTATION, N148I_FEATURE_CONTEXTUAL_RDO,
        N148I_FEATURE_CALIBRATED_CHROMA_QUANT,
        N148I_FEATURE_LUMA_TRANSFORM_16X16,
        N148I_FORMAT_7_BALANCED_RECONSTRUCTION,
    };
    int completed = 0;
    for (int trial = 0; trial < 24; trial++) {
        int width = widths[trial % 12], height = heights[trial % 12];
        size_t row = (size_t)width * 3, stride = row + 13;
        size_t pixel_bytes = row * (size_t)height;
        size_t padded_bytes = stride * (size_t)(height - 1) + row;
        uint8_t *tight = malloc(pixel_bytes), *padded = malloc(padded_bytes);
        uint8_t *encoded[2] = {NULL, NULL};
        size_t sizes[2] = {0, 0};
        n148i_image_t decoded[2] = {{0}, {0}};
        n148i_image_info_t info = {0};
        int success = 0;
        if (!tight || !padded) goto cleanup;
        uint32_t random = 731871u + (uint32_t)trial;
        for (size_t i = 0; i < pixel_bytes; i++) {
            random = random * 1664525u + 1013904223u;
            tight[i] = trial % 3 ? (uint8_t)(random >> 24) :
                (uint8_t)((i % 7 < 3) ? 0 : 255);
        }
        memset(padded, 0xa5, padded_bytes);
        for (int y = 0; y < height; y++)
            memcpy(padded + (size_t)y * stride, tight + (size_t)y * row, row);
        n148i_image_t source[2] = {
            {tight, (uint32_t)width, (uint32_t)height, trial % 2 ? row : 0},
            {padded, (uint32_t)width, (uint32_t)height, stride},
        };
        n148i_encode_options_t options;
        n148i_encode_options_init(&options);
        options.feature_flags = profile;
        options.quality = qualities[trial % 4];
        options.chroma = (n148i_chroma_t)(trial / 6);
        options.effort = efforts[(trial / 3 + (detail ? 2 : 0)) % 4];
        options.thread_count = 1;
        if (n148i_simd_force(N148I_SIMD_SCALAR) != N148I_OK ||
            n148i_encode_memory(&source[0], &options, &encoded[0], &sizes[0]) != N148I_OK ||
            n148i_read_header(encoded[0], sizes[0], &info) != N148I_OK ||
            !(info.feature_flags & (detail ? N148I_FORMAT_7_DETAIL_RECONSTRUCTION :
                                     N148I_FORMAT_7_BALANCED_RECONSTRUCTION)) ||
            info.chroma_quality != options.quality ||
            n148i_decode_memory(encoded[0], sizes[0], &decoded[0]) != N148I_OK)
            goto cleanup;
        options.chroma_quality = options.quality;
        if (n148i_simd_force(N148I_SIMD_AUTO) != N148I_OK ||
            n148i_encode_memory(&source[1], &options, &encoded[1], &sizes[1]) != N148I_OK ||
            sizes[0] != sizes[1] || memcmp(encoded[0], encoded[1], sizes[0]) ||
            n148i_decode_memory(encoded[1], sizes[1], &decoded[1]) != N148I_OK ||
            decoded[0].width != (uint32_t)width || decoded[0].height != (uint32_t)height ||
            decoded[1].width != (uint32_t)width || decoded[1].height != (uint32_t)height ||
            memcmp(decoded[0].pixels, decoded[1].pixels, pixel_bytes))
            goto cleanup;
        if (trial == 0) {
            uint32_t original_flags = info.feature_flags;
            for (size_t j = 0; j < sizeof(required) / sizeof(required[0]) - (detail ? 0u : 1u); j++) {
                uint32_t invalid_flags = original_flags & ~required[j];
                for (int byte = 0; byte < 4; byte++)
                    encoded[0][20 + byte] = (uint8_t)(invalid_flags >> (8 * byte));
                n148i_image_info_t invalid_info = {0};
                n148i_image_t invalid_image = {0};
                int header_status = n148i_read_header(encoded[0], sizes[0], &invalid_info);
                int decode_status = n148i_decode_memory(encoded[0], sizes[0], &invalid_image);
                n148i_free_image(&invalid_image);
                if (header_status == N148I_OK || decode_status == N148I_OK)
                    goto cleanup;
            }
            for (int byte = 0; byte < 4; byte++)
                encoded[0][20 + byte] = (uint8_t)(original_flags >> (8 * byte));
        }
        success = 1;
cleanup:
        for (int i = 0; i < 2; i++) {
            n148i_free_buffer(encoded[i]);
            n148i_free_image(&decoded[i]);
        }
        free(tight);
        free(padded);
        n148i_simd_force(N148I_SIMD_AUTO);
        if (!success) break;
        completed++;
    }
    printf("comparison,format_7_%s,scalar+auto,1,2,33,35,0,0,0,0.0,"
           "24_odd_stride_chroma_effort_pairs_extreme_quality_dependencies,%s\n",
           detail ? "detail" : "balanced", completed == 24 ? "PASS" : "FAIL");
    return completed == 24;
}


static int validate_format_7_selective_luma_search(const Image *original) {
    n148i_image_t source = {
        original->pixels, (uint32_t) original->width,
        (uint32_t) original->height, (size_t) original->width * 3u,
    };
    n148i_encode_options_t options;
    n148i_encode_options_init(&options);
    options.quality = 60;
    options.thread_count = 1;
    options.feature_flags = N148I_PROFILE_SPECTRAL_LUMA_QUANT;
    uint8_t *baseline = NULL, *scalar = NULL, *vector = NULL;
    size_t baseline_size = 0, scalar_size = 0, vector_size = 0;
    n148i_image_t baseline_rgb = {0}, decoded = {0};
    n148i_image_info_t info = {0};
    int baseline_ok =
        n148i_simd_force(N148I_SIMD_SCALAR) == N148I_OK &&
        n148i_encode_memory(&source, &options, &baseline,
                            &baseline_size) == N148I_OK &&
        n148i_decode_memory(baseline, baseline_size, &baseline_rgb) ==
            N148I_OK;
    options.effort = 4;
    int success = baseline_ok &&
        n148i_encode_memory(&source, &options, &scalar,
                            &scalar_size) == N148I_OK &&
        n148i_read_header(scalar, scalar_size, &info) == N148I_OK &&
        info.format_version == N148I_FORMAT_VERSION_7 &&
        info.feature_flags == N148I_PROFILE_SPECTRAL_LUMA_QUANT &&
        info.effort == 4 &&
        n148i_simd_force(N148I_SIMD_AVX2) == N148I_OK &&
        n148i_encode_memory(&source, &options, &vector,
                            &vector_size) == N148I_OK &&
        scalar_size == vector_size &&
        memcmp(scalar, vector, scalar_size) == 0 &&
        n148i_decode_memory(vector, vector_size, &decoded) == N148I_OK &&
        decoded.width == source.width && decoded.height == source.height &&
        baseline_rgb.width == source.width &&
        baseline_rgb.height == source.height &&
        baseline_rgb.stride == decoded.stride &&
        memcmp(baseline_rgb.pixels, decoded.pixels,
               decoded.stride * decoded.height) != 0;
    printf("comparison,format_7_selective_luma_search,scalar+AVX2,1,2,%d,%d,%zu,"
           "%u,%u,0.0,effort4_changes_odd_fixture_roundtrip_"
           "header_and_determinism,%s\n",
           original->width, original->height, scalar_size,
           info.payload_size, info.payload_size,
           success ? "PASS" : "FAIL");
    n148i_free_buffer(baseline);
    n148i_free_buffer(scalar);
    n148i_free_buffer(vector);
    n148i_free_image(&baseline_rgb);
    n148i_free_image(&decoded);
    n148i_simd_force(N148I_SIMD_AUTO);
    return success;
}

int main(int argc, char **argv) {
    const char *example_path =
        argc > 1 ? argv[1] : "images/example.ppm";
    const char *odd_path =
        argc > 2 ? argv[2] : "output/validation-317x241.ppm";
    Image example = {0};
    Image odd = {0};
    CycleResult scalar = {0}, avx2 = {0};
    int failures = 0;

    printf("kind,name,cpu,threads,chroma,width,height,file_bytes,"
           "payload_bytes,bytes_consumed,psnr_db,detail,result\n");

    if (!validate_rans_core()) failures++;
    if (!validate_adaptive_rans_core()) failures++;

    n148_cpu_force(-1);
    int detected_level = n148_cpu_level();
    if (detected_level < N148_CPU_AVX2) {
        fprintf(stderr, "AVX2 is unavailable; path comparison cannot run\n");
        return EXIT_FAILURE;
    }
    if (!load_ppm(example_path, &example)) {
        fprintf(stderr, "could not load %s\n", example_path);
        return EXIT_FAILURE;
    }

    int scalar_ok = run_cycle(&example, 50, CHROMA_420,
                              N148_CPU_BASELINE, 1, &scalar);
    int avx2_ok = run_cycle(&example, 50, CHROMA_420,
                            N148_CPU_AVX2, 1, &avx2);
    if (scalar_ok) print_cycle("example_scalar", &scalar);
    if (avx2_ok) print_cycle("example_avx2", &avx2);

    int path_match =
        scalar_ok && avx2_ok &&
        scalar.stream_size == EXPECTED_EXAMPLE_SIZE &&
        avx2.stream_size == EXPECTED_EXAMPLE_SIZE &&
        cycles_identical(&scalar, &avx2);
    printf("comparison,scalar_vs_avx2,scalar+AVX2,1,2,%d,%d,%ld,%ld,%ld,"
           "%.12f,byte_and_psnr_identical,%s\n",
           example.width, example.height,
           scalar_ok ? scalar.stream_size : 0,
           scalar_ok ? scalar.payload_size : 0,
           scalar_ok ? scalar.bytes_consumed : 0,
           scalar_ok ? scalar.psnr : 0.0,
           path_match ? "PASS" : "FAIL");
    if (!path_match) failures++;

    if (scalar_ok && !validate_public_api(&example, &scalar)) failures++;
    if (scalar_ok && !validate_rans_entropy(&example, &scalar)) failures++;
    if (scalar_ok && !validate_context_modeling(&example, &scalar)) failures++;
    if (scalar_ok && !validate_reconstructed_intra(&example)) failures++;
    if (scalar_ok && !validate_perceptual_color(&example)) failures++;
    if (scalar_ok && !validate_adaptive_quantization(&example)) failures++;
    if (scalar_ok && !validate_variable_transforms(&example)) failures++;
    if (scalar_ok && !validate_rate_distortion_optimization(&example)) failures++;
    if (scalar_ok && !validate_causal_loop_filter(&example)) failures++;
    if (scalar_ok && !validate_directional_intra(&example)) failures++;
    if (scalar_ok && !validate_chroma_format_matrix(
            &example, "chroma_format_matrix")) failures++;
    if (scalar_ok && !validate_reconstruction_aware_chroma(
            &example, "reconstruction_aware_chroma")) failures++;
    if (scalar_ok && !validate_joint_chroma_intra(
            &example, "joint_chroma_intra")) failures++;
    if (scalar_ok && !validate_calibrated_chroma_quantization(
            &example, "calibrated_chroma_quant")) failures++;
    if (scalar_ok && !validate_frame_segmentation(
            &example, "frame_segmentation")) failures++;
    if (scalar_ok && !validate_segment_adaptive_filter(
            &example, "adaptive_loop_filter")) failures++;
    if (scalar_ok && !validate_calibrated_luma_quantization(
            &example, "calibrated_luma_quant")) failures++;
    if (scalar_ok && !validate_variable_luma_transform(
            &example, "variable_luma_transform")) failures++;
    if (scalar_ok && !validate_perceptual_luma_trellis(
            &example, "perceptual_luma_trellis")) failures++;
    if (scalar_ok && !validate_quant_adaptive_luma_filter(
            &example, "quant_adaptive_luma_filter")) failures++;
    if (scalar_ok && !validate_automatic_chroma(
            &example, "automatic_chroma")) failures++;
    if (scalar_ok && !validate_fidelity_tools(
            &example, "fidelity_tools")) failures++;
    if (scalar_ok && !validate_adaptive_rich_context(
            &example, "adaptive_rich_context")) failures++;
    if (scalar_ok && !validate_format_7_full_trellis(&example)) failures++;
    if (scalar_ok && !validate_format_7_spectral_luma(&example)) failures++;
    if (scalar_ok && !validate_format_7_refined_spectral_luma(&example)) failures++;
    if (scalar_ok && !validate_format_7_structural_luma()) failures++;
    if (scalar_ok && !validate_format_7_reconstruction(
            N148I_PROFILE_BALANCED_RECONSTRUCTION, 0)) failures++;
    if (scalar_ok && !validate_format_7_reconstruction(
            N148I_PROFILE_DETAIL_RECONSTRUCTION, 1)) failures++;
    fill_odd_image(&odd);
    if (!odd.pixels || !save_ppm(odd_path, &odd)) {
        fprintf(stderr, "could not create %s\n", odd_path);
        failures++;
    } else {
        if (!validate_format_7_selective_luma_search(&odd)) failures++;
        if (!validate_directional_intra_odd_dimensions(&odd)) failures++;
        if (!validate_chroma_format_matrix(
                &odd, "chroma_format_matrix_odd")) failures++;
        if (!validate_reconstruction_aware_chroma(
                &odd, "reconstruction_aware_chroma_odd")) failures++;
        if (!validate_joint_chroma_intra(
                &odd, "joint_chroma_intra_odd")) failures++;
        if (!validate_calibrated_chroma_quantization(
                &odd, "calibrated_chroma_quant_odd")) failures++;
        if (!validate_frame_segmentation(
                &odd, "frame_segmentation_odd")) failures++;
        if (!validate_segment_adaptive_filter(
                &odd, "adaptive_loop_filter_odd")) failures++;
        if (!validate_calibrated_luma_quantization(
                &odd, "calibrated_luma_quant_odd")) failures++;
        if (!validate_variable_luma_transform(
                &odd, "variable_luma_transform_odd")) failures++;
        if (!validate_automatic_chroma(
                &odd, "automatic_chroma_odd")) failures++;
        for (int chroma = CHROMA_444; chroma <= CHROMA_420; chroma++) {
            CycleResult single = {0}, parallel = {0};
            int single_ok = run_cycle(&odd, 50, chroma, -1, 1, &single);
            int parallel_ok = run_cycle(&odd, 50, chroma, -1, 4, &parallel);
            char single_name[32];
            char parallel_name[32];
            snprintf(single_name, sizeof(single_name),
                     "odd_chroma%d_t1", chroma);
            snprintf(parallel_name, sizeof(parallel_name),
                     "odd_chroma%d_t4", chroma);
            if (single_ok) print_cycle(single_name, &single);
            if (parallel_ok) print_cycle(parallel_name, &parallel);

            int thread_match =
                single_ok && parallel_ok &&
                cycles_identical(&single, &parallel);
            printf("comparison,odd_chroma%d_t1_vs_t4,auto,1+4,%d,%d,%d,%ld,"
                   "%ld,%ld,%.12f,byte_and_psnr_identical,%s\n",
                   chroma, chroma, odd.width, odd.height,
                   single_ok ? single.stream_size : 0,
                   single_ok ? single.payload_size : 0,
                   single_ok ? single.bytes_consumed : 0,
                   single_ok ? single.psnr : 0.0,
                   thread_match ? "PASS" : "FAIL");
            if (!thread_match) failures++;
            release_cycle(&single);
            release_cycle(&parallel);
        }
    }

    release_cycle(&scalar);
    release_cycle(&avx2);
    free_image(&example);
    free_image(&odd);
    n148_cpu_force(-1);
    n148_set_thread_count(1);

    fprintf(stderr, "validation %s (%d failure%s)\n",
            failures == 0 ? "PASS" : "FAIL",
            failures, failures == 1 ? "" : "s");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

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

#include "decoder.h"
#include "encoder.h"
#include "header.h"
#include "huffman.h"
#include "ppm.h"
#include "cpu.h"
#include "parallel.h"

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
    header.version = N148I_VERSION;
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
        N148I_HEADER_SIZE + huffman_tables_size(encode_specs) +
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

    fill_odd_image(&odd);
    if (!odd.pixels || !save_ppm(odd_path, &odd)) {
        fprintf(stderr, "could not create %s\n", odd_path);
        failures++;
    } else {
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

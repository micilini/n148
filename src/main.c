/*
 * N.148i command-line roundtrip tool
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "n148i.h"
#include "ppm.h"

#define DEFAULT_INPUT   "images/example.ppm"
#define DEFAULT_ENCODED "output/image.n148i"
#define DEFAULT_DECODED "output/decoded.ppm"
#define N148I_FIXED_HEADER_SIZE 21
#ifndef QUALITY
#define QUALITY 50
#endif
#ifndef CHROMA_MODE
#define CHROMA_MODE N148I_CHROMA_420
#endif
#ifndef OPTIMIZE
#define OPTIMIZE 1
#endif

static long file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long size = ftell(file);
    fclose(file);
    return size;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int written = fwrite(data, 1, size, file) == size;
    int closed = fclose(file) == 0;
    return written && closed;
}

static double compute_psnr(const Image *first, const n148i_image_t *second) {
    if ((uint32_t) first->width != second->width ||
        (uint32_t) first->height != second->height) {
        return 0.0;
    }

    long value_count = (long) first->width * first->height * 3;
    double mean_squared_error = 0.0;
    for (long index = 0; index < value_count; index++) {
        double difference =
            (double) first->pixels[index] - (double) second->pixels[index];
        mean_squared_error += difference * difference;
    }
    mean_squared_error /= value_count;
    if (mean_squared_error == 0.0) return 999.0;
    return 10.0 * log10(255.0 * 255.0 / mean_squared_error);
}

static const char *chroma_name(n148i_chroma_t chroma) {
    switch (chroma) {
        case N148I_CHROMA_444: return "4:4:4";
        case N148I_CHROMA_422: return "4:2:2";
        case N148I_CHROMA_420: return "4:2:0";
        default: return "unknown";
    }
}

static void cli_chroma_dimensions(n148i_chroma_t chroma,
                                  int width, int height,
                                  int *chroma_width, int *chroma_height) {
    *chroma_width = chroma == N148I_CHROMA_444 ? width : (width + 1) / 2;
    *chroma_height = chroma == N148I_CHROMA_420 ? (height + 1) / 2 : height;
}

static long block_count(int width, int height) {
    return (long) ((width + 7) / 8) * ((height + 7) / 8);
}

static void print_usage(const char *program) {
    printf("usage: %s [input.ppm] [quality] [chroma]\n", program);
    printf("  input.ppm  source image (default: %s)\n", DEFAULT_INPUT);
    printf("  quality    1..100        (default: %d)\n", QUALITY);
    printf("  chroma     0=4:4:4 1=4:2:2 2=4:2:0 (default: %d)\n",
           CHROMA_MODE);
}

int main(int argc, char **argv) {
    const char *input_path = DEFAULT_INPUT;
    int quality = QUALITY;
    int chroma_value = CHROMA_MODE;
    Image original = {0};
    n148i_image_t decoded = {0};
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int exit_code = EXIT_FAILURE;

    if (argc > 1) {
        if (argv[1][0] == '-') {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        input_path = argv[1];
    }
    if (argc > 2) quality = atoi(argv[2]);
    if (argc > 3) chroma_value = atoi(argv[3]);
    if (quality < 1 || quality > 100) {
        printf("quality must be 1..100\n");
        return EXIT_FAILURE;
    }
    if (chroma_value < N148I_CHROMA_444 ||
        chroma_value > N148I_CHROMA_420) {
        printf("chroma must be 0, 1 or 2\n");
        return EXIT_FAILURE;
    }

    if (!load_ppm(input_path, &original)) goto cleanup;

    n148i_chroma_t chroma = (n148i_chroma_t) chroma_value;
    n148i_image_t source = {
        original.pixels,
        (uint32_t) original.width,
        (uint32_t) original.height,
        (size_t) original.width * 3
    };
    n148i_encode_options_t options;
    n148i_encode_options_init(&options);
    options.quality = quality;
    options.chroma = chroma;
    options.optimize_huffman = OPTIMIZE;

    printf("=== N.148i encoder ===\n");
    printf("Input:    %s  (%d x %d)\n", input_path,
           original.width, original.height);
    printf("Quality:  %d\n", quality);
    printf("Chroma:   %s\n", chroma_name(chroma));
    printf("Huffman:  %s\n\n",
           options.optimize_huffman ? "optimized for this image" :
                                      "standard tables");

    int chroma_width, chroma_height;
    cli_chroma_dimensions(chroma, original.width, original.height,
                          &chroma_width, &chroma_height);
    printf("Y plane:      %d x %d\n", original.width, original.height);
    printf("Cb/Cr planes: %d x %d\n\n", chroma_width, chroma_height);

    n148i_result_t result = n148i_encode_memory(
        &source, &options, &encoded, &encoded_size);
    if (result != N148I_OK) {
        printf("Encoding failed: %s.\n", n148i_result_string(result));
        goto cleanup;
    }

    n148i_image_info_t info;
    result = n148i_read_header(encoded, encoded_size, &info);
    if (result != N148I_OK) {
        printf("Encoded header is invalid: %s.\n",
               n148i_result_string(result));
        goto cleanup;
    }
    long blocks = block_count(original.width, original.height) +
        2 * block_count(chroma_width, chroma_height);
    printf("Encoded %ld blocks\n", blocks);
    if (info.optimized_huffman) {
        printf("Huffman tables: %zu bytes stored in the file\n",
               info.encoded_header_size - N148I_FIXED_HEADER_SIZE);
    }
    printf("Entropy data:   %u bytes\n", info.payload_size);

    if (!write_file(DEFAULT_ENCODED, encoded, encoded_size)) {
        printf("Could not write '%s'\n", DEFAULT_ENCODED);
        goto cleanup;
    }
    printf("Wrote %s\n\n", DEFAULT_ENCODED);

    printf("=== N.148i decoder ===\n");
    printf("Header:  v%u, %u x %u, quality %u, chroma %s, %s tables\n",
           info.format_version, info.width, info.height, info.quality,
           chroma_name(info.chroma),
           info.optimized_huffman ? "custom" : "standard");
    result = n148i_decode_memory(encoded, encoded_size, &decoded);
    if (result != N148I_OK) {
        printf("Decoding failed: %s.\n", n148i_result_string(result));
        goto cleanup;
    }
    printf("Decoded %ld blocks, consumed %u of %u bytes\n",
           blocks, info.payload_size, info.payload_size);
    printf("Upsampling: bilinear\n");

    Image decoded_view = {
        (int) decoded.width,
        (int) decoded.height,
        decoded.pixels
    };
    if (!save_ppm(DEFAULT_DECODED, &decoded_view)) {
        printf("Could not write '%s'\n", DEFAULT_DECODED);
        goto cleanup;
    }
    printf("Wrote %s\n\n", DEFAULT_DECODED);

    long original_bytes = file_size(input_path);
    long encoded_bytes = file_size(DEFAULT_ENCODED);
    double psnr = compute_psnr(&original, &decoded);
    printf("=== Results ===\n");
    printf("Original PPM: %8ld bytes (%.1f KB)\n",
           original_bytes, original_bytes / 1024.0);
    printf("N148i file:   %8ld bytes (%.1f KB)\n",
           encoded_bytes, encoded_bytes / 1024.0);
    printf("Compression:  %.1f:1\n", (double) original_bytes / encoded_bytes);
    printf("PSNR:         %.2f dB\n", psnr);
    exit_code = EXIT_SUCCESS;

cleanup:
    n148i_free_buffer(encoded);
    n148i_free_image(&decoded);
    free_image(&original);
    return exit_code;
}

/*
 * N.148i command-line roundtrip tool
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n148i.h"
#include "ppm.h"

#define DEFAULT_INPUT   "images/example.ppm"
#define DEFAULT_ENCODED "output/image.n148i"
#define DEFAULT_DECODED "output/decoded.ppm"
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
        case N148I_CHROMA_AUTO: return "auto";
        default: return "unknown";
    }
}

static void cli_chroma_dimensions(n148i_chroma_t chroma,
                                  int width, int height,
                                  int *chroma_width, int *chroma_height) {
    *chroma_width = chroma == N148I_CHROMA_444 ?
        width : width / 2 + (width & 1);
    *chroma_height = chroma == N148I_CHROMA_420 ?
        height / 2 + (height & 1) : height;
}

static long block_count(int width, int height) {
    int blocks_x = width / 8 + (width % 8 != 0);
    int blocks_y = height / 8 + (height % 8 != 0);
    return (long) blocks_x * blocks_y;
}

static void print_usage(const char *program) {
    printf("usage: %s [input.ppm] [quality] [chroma] [format] [profile] [effort]\n", program);
    printf("  input.ppm  source image (default: %s)\n", DEFAULT_INPUT);
    printf("  quality    1..100        (default: %d)\n", QUALITY);
    printf("  chroma     0=4:4:4 1=4:2:2 2=4:2:0 3=auto (default: %d)\n",
           CHROMA_MODE);
    printf("  format     1=legacy 2-6=compatible 7=current (default: %d)\n",
           N148I_FORMAT_VERSION);
    printf("  profile    default, spectral, refined, structural, balanced, or detail (custom profiles require format 7)\n");
    printf("  effort     0..9 (default: 3; use 4 for selective luma search)\n");
}

int main(int argc, char **argv) {
    const char *input_path = DEFAULT_INPUT;
    int quality = QUALITY;
    int chroma_value = CHROMA_MODE;
    int format_version = N148I_FORMAT_VERSION;
    const char *profile = "default";
    int effort = 3;
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
    if (argc > 4) format_version = atoi(argv[4]);
    if (argc > 5) profile = argv[5];
    if (argc > 6) effort = atoi(argv[6]);
    if (argc > 7 || effort < 0 || effort > 9 ||
        (strcmp(profile, "default") != 0 &&
         strcmp(profile, "spectral") != 0 &&
         strcmp(profile, "refined") != 0 &&
         strcmp(profile, "structural") != 0 &&
         strcmp(profile, "balanced") != 0 &&
         strcmp(profile, "detail") != 0)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (quality < 1 || quality > 100) {
        printf("quality must be 1..100\n");
        return EXIT_FAILURE;
    }
    if (chroma_value < N148I_CHROMA_444 ||
        chroma_value > N148I_CHROMA_AUTO) {
        printf("chroma must be 0, 1, 2 or 3\n");
        return EXIT_FAILURE;
    }
    if (chroma_value == N148I_CHROMA_AUTO &&
        format_version != N148I_FORMAT_VERSION_5 &&
        format_version != N148I_FORMAT_VERSION_6 &&
        format_version != N148I_FORMAT_VERSION_7) {
        printf("automatic chroma requires format 5, 6 or 7\n");
        return EXIT_FAILURE;
    }
    if (format_version != N148I_FORMAT_VERSION_1 &&
        format_version != N148I_FORMAT_VERSION_2 &&
        format_version != N148I_FORMAT_VERSION_3 &&
        format_version != N148I_FORMAT_VERSION_4 &&
        format_version != N148I_FORMAT_VERSION_5 &&
        format_version != N148I_FORMAT_VERSION_6 &&
        format_version != N148I_FORMAT_VERSION_7) {
        printf("format must be 1, 2, 3, 4, 5, 6 or 7\n");
        return EXIT_FAILURE;
    }
    if (strcmp(profile, "default") != 0 &&
        format_version != N148I_FORMAT_VERSION_7) {
        printf("spectral, refined, structural, balanced, and detail profiles require format 7\n");
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
    options.effort = effort;
    options.chroma = chroma;
    options.optimize_huffman = OPTIMIZE;
    options.format_version = (uint32_t) format_version;
    if (format_version == N148I_FORMAT_VERSION_2) {
        options.feature_flags = N148I_FORMAT_2_DEFAULT_FEATURES;
    } else if (format_version == N148I_FORMAT_VERSION_3) {
        options.feature_flags = N148I_FORMAT_3_DEFAULT_FEATURES;
    } else if (format_version == N148I_FORMAT_VERSION_4) {
        options.feature_flags = N148I_FORMAT_4_DEFAULT_FEATURES;
    } else if (format_version == N148I_FORMAT_VERSION_5) {
        options.feature_flags = N148I_FORMAT_5_DEFAULT_FEATURES;
    } else if (format_version == N148I_FORMAT_VERSION_6) {
        options.feature_flags = N148I_FORMAT_6_DEFAULT_FEATURES;
    } else if (format_version == N148I_FORMAT_VERSION_7) {
        options.feature_flags = strcmp(profile, "detail") == 0 ?
            N148I_PROFILE_DETAIL_RECONSTRUCTION :
            strcmp(profile, "balanced") == 0 ?
            N148I_PROFILE_BALANCED_RECONSTRUCTION :
            strcmp(profile, "structural") == 0 ?
            N148I_PROFILE_STRUCTURAL_LUMA_QUANT :
            strcmp(profile, "spectral") == 0 ?
            N148I_PROFILE_SPECTRAL_LUMA_QUANT :
            strcmp(profile, "refined") == 0 ?
            N148I_PROFILE_REFINED_SPECTRAL_LUMA_QUANT :
            N148I_DEFAULT_FEATURES;
    }

    printf("=== N.148i encoder ===\n");
    printf("Input:    %s  (%d x %d)\n", input_path,
           original.width, original.height);
    printf("Quality:  %d\n", quality);
    printf("Chroma:   %s\n", chroma_name(chroma));
    printf("Format:   v%d\n", format_version);
    printf("Effort:   %d\n", effort);
    if (format_version == N148I_FORMAT_VERSION_7)
        printf("Profile:  %s\n", profile);
    if (format_version >= N148I_FORMAT_VERSION_2) {
        printf("Coding:   contextual byte rANS with intra prediction\n\n");
    } else {
        printf("Huffman:  %s\n\n",
               options.optimize_huffman ? "optimized for this image" :
                                          "standard tables");
    }

    int chroma_width = 0, chroma_height = 0;
    printf("Y plane:      %d x %d\n", original.width, original.height);
    if (chroma == N148I_CHROMA_AUTO) {
        printf("Cb/Cr planes: automatic search\n\n");
    } else {
        cli_chroma_dimensions(chroma, original.width, original.height,
                              &chroma_width, &chroma_height);
        printf("Cb/Cr planes: %d x %d\n\n", chroma_width, chroma_height);
    }

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
    cli_chroma_dimensions(info.chroma, original.width, original.height,
                          &chroma_width, &chroma_height);
    long blocks = block_count(original.width, original.height) +
        2 * block_count(chroma_width, chroma_height);
    printf("Encoded %ld blocks\n", blocks);
    if (info.format_version >= N148I_FORMAT_VERSION_2) {
        printf("Feature flags:   0x%08x\n", info.feature_flags);
        printf("Entropy metadata: %u bytes\n", info.metadata_size);
    } else if (info.optimized_huffman) {
        printf("Huffman tables: %u bytes stored in the file\n",
               info.metadata_size);
    }
    printf("Entropy data:   %u bytes\n", info.payload_size);

    if (!write_file(DEFAULT_ENCODED, encoded, encoded_size)) {
        printf("Could not write '%s'\n", DEFAULT_ENCODED);
        goto cleanup;
    }
    printf("Wrote %s\n\n", DEFAULT_ENCODED);

    printf("=== N.148i decoder ===\n");
    printf("Header:  v%u, %u x %u, quality %u, chroma %s",
           info.format_version, info.width, info.height, info.quality,
           chroma_name(info.chroma));
    if (info.format_version >= N148I_FORMAT_VERSION_2) {
        printf(", features 0x%08x\n", info.feature_flags);
    } else {
        printf(", %s Huffman tables\n",
               info.optimized_huffman ? "custom" : "standard");
    }
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

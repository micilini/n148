#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "decoder.h"
#include "encoder.h"
#include "header.h"
#include "huffman.h"
#include "ppm.h"

#define INPUT_PATH "../images/example.ppm"
#define N148I_PATH "../output/image.n148i"
#define DECODED_PATH "../output/decoded.ppm"
#ifndef QUALITY
#define QUALITY 50
#endif
#ifndef CHROMA_MODE
#define CHROMA_MODE CHROMA_420
#endif
#ifndef OPTIMIZE
#define OPTIMIZE 1
#endif
#ifndef SMOOTH_UPSAMPLING
#define SMOOTH_UPSAMPLING 1
#endif

static long file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long size = ftell(file);
    fclose(file);
    return size;
}

static double compute_psnr(Image *first, Image *second) {
    if (first->width != second->width || first->height != second->height) {
        return 0.0;
    }

    long value_count = (long)first->width * first->height * 3;
    double mean_squared_error = 0.0;

    for (long i = 0; i < value_count; i++) {
        double difference =
            (double)first->pixels[i] - (double)second->pixels[i];
        mean_squared_error += difference * difference;
    }

    mean_squared_error /= value_count;
    if (mean_squared_error == 0.0) {
        return 999.0;
    }

    return 10.0 * log10(255.0 * 255.0 / mean_squared_error);
}

int main(void) {
    Image original = {0};
    Image decoded = {0};
    Plane y = {0};
    Plane cb = {0};
    Plane cr = {0};
    Plane decoded_y = {0};
    Plane decoded_cb = {0};
    Plane decoded_cr = {0};
    unsigned char *compressed = NULL;
    unsigned char *payload = NULL;
    HuffSpec encode_specs[HUFFMAN_TABLE_COUNT];
    HuffSpec decode_specs[HUFFMAN_TABLE_COUNT];
    FILE *file = NULL;
    int exit_code = EXIT_FAILURE;

    if ((CHROMA_MODE != CHROMA_444 && CHROMA_MODE != CHROMA_422 &&
         CHROMA_MODE != CHROMA_420) ||
        (OPTIMIZE != 0 && OPTIMIZE != 1) ||
        (SMOOTH_UPSAMPLING != 0 && SMOOTH_UPSAMPLING != 1)) {
        printf("Invalid codec configuration.\n");
        goto cleanup;
    }

    // ================= ENCODE =================
    if (!load_ppm(INPUT_PATH, &original)) {
        goto cleanup;
    }

    printf("=== N.148i encoder ===\n");
    printf("Input:    %s  (%d x %d)\n", INPUT_PATH,
           original.width, original.height);
    printf("Quality:  %d\n", QUALITY);
    printf("Chroma:   %s\n", chroma_name(CHROMA_MODE));
    printf("Huffman:  %s\n\n",
           OPTIMIZE ? "optimized for this image" : "standard tables");

    if (!split_channels(&original, &y, &cb, &cr, CHROMA_MODE)) {
        printf("Could not split channels.\n");
        goto cleanup;
    }
    printf("Y plane:      %d x %d\n", y.width, y.height);
    printf("Cb/Cr planes: %d x %d\n\n", cb.width, cb.height);

    EncodeStats encode_stats;
    if (!encode_image(&y, &cb, &cr, QUALITY, OPTIMIZE, encode_specs,
                      &compressed, &encode_stats)) {
        printf("Encoding failed.\n");
        goto cleanup;
    }

    long encoded_blocks =
        encode_stats.blocks_y +
        encode_stats.blocks_cb +
        encode_stats.blocks_cr;
    printf("Encoded %ld blocks\n", encoded_blocks);
    if (OPTIMIZE) {
        printf("Huffman tables: %ld bytes stored in the file\n",
               encode_stats.table_size);
    }
    printf("Entropy data:   %ld bytes\n", encode_stats.data_size);

    file = fopen(N148I_PATH, "wb");
    if (!file) {
        printf("Could not create '%s'\n", N148I_PATH);
        goto cleanup;
    }

    N148iHeader header;
    header.version = N148I_VERSION;
    header.width = (uint32_t)original.width;
    header.height = (uint32_t)original.height;
    header.quality = QUALITY;
    header.chroma = CHROMA_MODE;
    header.optimized = OPTIMIZE;
    header.data_size = (uint32_t)encode_stats.data_size;

    int write_ok = write_header(file, &header);
    if (write_ok && header.optimized) {
        write_ok = write_huffman_tables(file, encode_specs);
    }
    if (write_ok) {
        write_ok =
            fwrite(compressed, 1, (size_t)encode_stats.data_size, file) ==
            (size_t)encode_stats.data_size;
    }
    int close_ok = fclose(file) == 0;
    file = NULL;
    if (!write_ok || !close_ok) {
        printf("Could not write '%s'\n", N148I_PATH);
        goto cleanup;
    }
    printf("Wrote %s\n\n", N148I_PATH);

    free(compressed);
    compressed = NULL;
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);

    // ================= DECODE =================
    printf("=== N.148i decoder ===\n");

    file = fopen(N148I_PATH, "rb");
    if (!file) {
        printf("Could not open '%s'\n", N148I_PATH);
        goto cleanup;
    }

    N148iHeader loaded;
    if (!read_header(file, &loaded)) {
        printf("This is not a valid N148i file!\n");
        goto cleanup;
    }
    if (loaded.version != N148I_VERSION ||
        loaded.width == 0 || loaded.width > INT_MAX ||
        loaded.height == 0 || loaded.height > INT_MAX ||
        loaded.quality == 0 || loaded.quality > 100 ||
        (loaded.chroma != CHROMA_444 && loaded.chroma != CHROMA_422 &&
         loaded.chroma != CHROMA_420) ||
        loaded.optimized > 1 ||
        loaded.data_size == 0) {
        printf("Unsupported N148i header!\n");
        goto cleanup;
    }

    printf("Header:  v%d, %u x %u, quality %d, chroma %s, %s tables\n",
           loaded.version, loaded.width, loaded.height,
           loaded.quality, chroma_name(loaded.chroma),
           loaded.optimized ? "custom" : "standard");

    if (loaded.optimized) {
        if (!read_huffman_tables(file, decode_specs)) {
            printf("Custom Huffman tables are invalid or truncated!\n");
            goto cleanup;
        }
    } else {
        huffman_default_specs(decode_specs);
    }

    payload = (unsigned char *)malloc(loaded.data_size);
    if (!payload) {
        printf("Out of memory while reading the payload.\n");
        goto cleanup;
    }

    if (fread(payload, 1, loaded.data_size, file) != loaded.data_size) {
        printf("File is truncated!\n");
        goto cleanup;
    }
    fclose(file);
    file = NULL;

    DecodeStats decode_stats;
    if (!decode_image(payload, loaded.data_size,
                      (int)loaded.width, (int)loaded.height,
                      loaded.quality, loaded.chroma,
                      decode_specs,
                      &decoded_y, &decoded_cb, &decoded_cr,
                      &decode_stats)) {
        printf("Compressed payload is invalid or truncated!\n");
        goto cleanup;
    }

    long decoded_blocks =
        decode_stats.blocks_y +
        decode_stats.blocks_cb +
        decode_stats.blocks_cr;
    printf("Decoded %ld blocks, consumed %ld of %u bytes\n",
           decoded_blocks, decode_stats.bytes_consumed,
           loaded.data_size);

    if (!merge_channels(&decoded_y, &decoded_cb, &decoded_cr,
                        SMOOTH_UPSAMPLING, &decoded)) {
        printf("Could not rebuild the RGB image.\n");
        goto cleanup;
    }
    printf("Upsampling: %s\n",
           SMOOTH_UPSAMPLING ? "bilinear" : "nearest neighbour");
    if (!save_ppm(DECODED_PATH, &decoded)) {
        printf("Could not write '%s'\n", DECODED_PATH);
        goto cleanup;
    }
    printf("Wrote %s\n\n", DECODED_PATH);

    // ================= RESULTS =================
    long original_bytes = file_size(INPUT_PATH);
    long n148i_bytes = file_size(N148I_PATH);
    double psnr = compute_psnr(&original, &decoded);

    printf("=== Results ===\n");
    printf("Original PPM: %8ld bytes (%.1f KB)\n",
           original_bytes, original_bytes / 1024.0);
    printf("N148i file:   %8ld bytes (%.1f KB)\n",
           n148i_bytes, n148i_bytes / 1024.0);
    printf("Compression:  %.1f:1\n",
           (double)original_bytes / n148i_bytes);
    printf("PSNR:         %.2f dB\n", psnr);

    exit_code = EXIT_SUCCESS;

cleanup:
    if (file) {
        fclose(file);
    }
    free(compressed);
    free(payload);
    free_image(&original);
    free_image(&decoded);
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    free_plane(&decoded_y);
    free_plane(&decoded_cb);
    free_plane(&decoded_cr);
    return exit_code;
}

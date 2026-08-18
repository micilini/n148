#include <stdio.h>
#include <stdlib.h>

#include "encoder.h"
#include "header.h"
#include "ppm.h"

#define INPUT_PATH "../images/example.ppm"
#define OUTPUT_PATH "../output/image.n148i"
#ifndef QUALITY
#define QUALITY 50
#endif

static long file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    return size;
}

int main(void) {
    // 1. Load the PPM image.
    Image image;
    if (!load_ppm(INPUT_PATH, &image)) {
        return EXIT_FAILURE;
    }

    printf("=== N.148i encoder ===\n\n");
    printf("Input:    %s\n", INPUT_PATH);
    printf("Size:     %d x %d pixels\n", image.width, image.height);
    printf("Quality:  %d\n\n", QUALITY);

    // 2. Split into Y, Cb, and Cr planes using 4:2:0.
    Plane y;
    Plane cb;
    Plane cr;
    if (!split_channels(&image, &y, &cb, &cr, 1)) {
        printf("Could not split channels.\n");
        free_image(&image);
        return EXIT_FAILURE;
    }

    printf("Y plane:      %d x %d\n", y.width, y.height);
    printf("Cb/Cr planes: %d x %d  (4:2:0)\n\n", cb.width, cb.height);

    // 3. Encode every plane.
    unsigned char *compressed = NULL;
    EncodeStats stats;
    if (!encode_image(&y, &cb, &cr, QUALITY, &compressed, &stats)) {
        printf("Encoding failed.\n");
        free_image(&image);
        free_plane(&y);
        free_plane(&cb);
        free_plane(&cr);
        return EXIT_FAILURE;
    }

    printf("Blocks encoded:  Y=%ld  Cb=%ld  Cr=%ld  (total %ld)\n",
           stats.blocks_y, stats.blocks_cb, stats.blocks_cr,
           stats.blocks_y + stats.blocks_cb + stats.blocks_cr);
    printf("Compressed data: %ld bytes\n\n", stats.data_size);

    // 4. Write the N.148i version 2 header and compressed payload.
    FILE *file = fopen(OUTPUT_PATH, "wb");
    if (!file) {
        printf("Could not create '%s'\n", OUTPUT_PATH);
        free(compressed);
        free_image(&image);
        free_plane(&y);
        free_plane(&cb);
        free_plane(&cr);
        return EXIT_FAILURE;
    }

    N148iHeader header;
    header.version = N148I_VERSION;
    header.width = (uint32_t)image.width;
    header.height = (uint32_t)image.height;
    header.quality = QUALITY;
    header.chroma = CHROMA_420;
    header.data_size = (uint32_t)stats.data_size;

    write_header(file, &header);
    fwrite(compressed, 1, (size_t)stats.data_size, file);
    fclose(file);

    // 5. Report compression statistics.
    long ppm_bytes = file_size(INPUT_PATH);
    long n148_bytes = file_size(OUTPUT_PATH);

    printf("Wrote %s\n\n", OUTPUT_PATH);
    printf("PPM  original:  %8ld bytes  (%.1f KB)\n",
           ppm_bytes, ppm_bytes / 1024.0);
    printf("N148i file:     %8ld bytes  (%.1f KB)\n",
           n148_bytes, n148_bytes / 1024.0);
    printf("Header:               %2d bytes\n", 20);
    printf("Compression:    %.1f:1  (%.1f%% smaller)\n",
           (double)ppm_bytes / n148_bytes,
           100.0 * (1.0 - (double)n148_bytes / ppm_bytes));

    // 6. Clean up.
    free(compressed);
    free_image(&image);
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    return EXIT_SUCCESS;
}

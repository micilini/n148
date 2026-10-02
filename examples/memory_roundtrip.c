/*
 * Minimal N.148i memory-to-memory example
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <n148i.h>

static void report_error(const char *operation, n148i_result_t result) {
    fprintf(stderr, "%s: %s\n", operation, n148i_result_string(result));
}

int main(void) {
    enum { WIDTH = 64, HEIGHT = 48 };
    uint8_t pixels[WIDTH * HEIGHT * 3];
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            size_t offset = ((size_t) y * WIDTH + (size_t) x) * 3;
            pixels[offset + 0] = (uint8_t) (x * 255 / (WIDTH - 1));
            pixels[offset + 1] = (uint8_t) (y * 255 / (HEIGHT - 1));
            pixels[offset + 2] = (uint8_t) ((x ^ y) * 4);
        }
    }

    n148i_image_t source = {pixels, WIDTH, HEIGHT, WIDTH * 3};
    n148i_encode_options_t options;
    n148i_encode_options_init(&options);
    options.quality = 75;

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    n148i_result_t result = n148i_encode_memory(
        &source, &options, &encoded, &encoded_size);
    if (result != N148I_OK) {
        report_error("encode", result);
        return EXIT_FAILURE;
    }

    n148i_image_info_t info;
    result = n148i_read_header(encoded, encoded_size, &info);
    if (result != N148I_OK) {
        report_error("read header", result);
        n148i_free_buffer(encoded);
        return EXIT_FAILURE;
    }

    n148i_image_t decoded = {0};
    result = n148i_decode_memory(encoded, encoded_size, &decoded);
    if (result != N148I_OK) {
        report_error("decode", result);
        n148i_free_buffer(encoded);
        return EXIT_FAILURE;
    }

    printf("N.148i %s encoded %ux%u RGB into %zu bytes; decoded %ux%u\n",
           n148i_library_version(), info.width, info.height, encoded_size,
           decoded.width, decoded.height);

    n148i_free_image(&decoded);
    n148i_free_buffer(encoded);
    return EXIT_SUCCESS;
}

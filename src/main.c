#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// DATA STRUCTURES
// ============================================================

typedef struct {
    int width;
    int height;
    unsigned char *pixels; // RGB interleaved: R,G,B,R,G,B...
} Image;

typedef struct {
    int width;
    int height;
    unsigned char *y;  // Luminance.
    unsigned char *cb; // Blue-difference chroma.
    unsigned char *cr; // Red-difference chroma.
} YCbCrImage;

// ============================================================
// PPM HEADER PARSING HELPERS
// ============================================================
//
// The PPM header is plain text, and the format allows any amount
// of whitespace between the numbers, plus '#' comment lines.
// This helper eats all of that so we land exactly on the next
// real piece of content.

static void skip_whitespace_and_comments(FILE *file) {
    int character;

    for (;;) {
        character = fgetc(file);
        if (character == '#') {
            // Comment: skip the whole line.
            while (character != '\n' && character != EOF) {
                character = fgetc(file);
            }
        } else if (character == ' ' || character == '\t' ||
                   character == '\n' || character == '\r') {
            continue; // Whitespace: keep going.
        } else {
            ungetc(character, file); // Real content: put it back.
            return;
        }
    }
}

// Reads one decimal number from the header.
// NOTE: this also consumes the single character that ends the
// number (usually a newline), which is exactly the separator the
// format expects before the pixel data starts.
static int read_number(FILE *file) {
    skip_whitespace_and_comments(file);

    int value = 0;
    int character;
    int digits = 0;

    while ((character = fgetc(file)) != EOF &&
           character >= '0' && character <= '9') {
        value = value * 10 + (character - '0');
        digits++;
    }

    if (digits == 0) {
        return -1;
    }

    return value;
}

// ============================================================
// LOAD A BINARY PPM (P6) FILE
// ============================================================

int load_ppm(const char *path, Image *image) {
    FILE *file = fopen(path, "rb"); // "rb" = read binary.
    if (!file) {
        printf("Could not open '%s'. Is the file in the images/ folder?\n", path);
        return 0;
    }

    // 1. Check the magic number: it must be "P6".
    char magic[3] = {0};
    if (fread(magic, 1, 2, file) != 2) {
        printf("File is too small to be a PPM.\n");
        fclose(file);
        return 0;
    }

    if (magic[0] != 'P' || magic[1] != '6') {
        printf("Not a binary PPM (P6). Found '%s'.\n", magic);
        printf("Tip: if it says P3, re-export choosing the BINARY option.\n");
        fclose(file);
        return 0;
    }

    // 2. Read width, height, and maxval from the text header.
    int width = read_number(file);
    int height = read_number(file);
    int maxval = read_number(file);

    if (width <= 0 || height <= 0) {
        printf("Invalid dimensions in header.\n");
        fclose(file);
        return 0;
    }

    if (maxval != 255) {
        printf("Only maxval 255 is supported (found %d).\n", maxval);
        fclose(file);
        return 0;
    }

    // 3. Read all the raw pixel bytes at once.
    long pixel_count = (long)width * height;
    long byte_count = pixel_count * 3; // 3 bytes per pixel (R, G, B).

    unsigned char *pixels = (unsigned char *)malloc((size_t)byte_count);
    if (!pixels) {
        printf("Out of memory.\n");
        fclose(file);
        return 0;
    }

    if (fread(pixels, 1, (size_t)byte_count, file) != (size_t)byte_count) {
        printf("Pixel data ended too early. Is the file truncated?\n");
        free(pixels);
        fclose(file);
        return 0;
    }

    fclose(file);

    image->width = width;
    image->height = height;
    image->pixels = pixels;
    return 1;
}

// ============================================================
// RGB -> YCbCr CONVERSION
// ============================================================
//
// These are the JPEG / ITU-R BT.601 formulas introduced earlier
// in the series. Y holds brightness; Cb and Cr hold colour.

static unsigned char clamp_byte(double value) {
    if (value < 0.0) {
        return 0;
    }
    if (value > 255.0) {
        return 255;
    }

    return (unsigned char)(value + 0.5); // Round to nearest.
}

int convert_to_ycbcr(Image *image, YCbCrImage *output) {
    long pixel_count = (long)image->width * image->height;

    output->width = image->width;
    output->height = image->height;
    output->y = (unsigned char *)malloc((size_t)pixel_count);
    output->cb = (unsigned char *)malloc((size_t)pixel_count);
    output->cr = (unsigned char *)malloc((size_t)pixel_count);

    if (!output->y || !output->cb || !output->cr) {
        printf("Out of memory.\n");
        free(output->y);
        free(output->cb);
        free(output->cr);
        output->y = NULL;
        output->cb = NULL;
        output->cr = NULL;
        return 0;
    }

    for (long i = 0; i < pixel_count; i++) {
        double red = image->pixels[i * 3 + 0];
        double green = image->pixels[i * 3 + 1];
        double blue = image->pixels[i * 3 + 2];

        output->y[i] = clamp_byte(
            0.299000 * red + 0.587000 * green + 0.114000 * blue);
        output->cb[i] = clamp_byte(
            -0.168736 * red - 0.331264 * green + 0.500000 * blue + 128.0);
        output->cr[i] = clamp_byte(
            0.500000 * red - 0.418688 * green - 0.081312 * blue + 128.0);
    }

    return 1;
}

// ============================================================
// SAVE A SINGLE CHANNEL AS A GRAYSCALE PGM (P5) FILE
// ============================================================
//
// PGM is the grayscale cousin of PPM: the same idea, but with one
// byte per pixel instead of three. It lets us inspect each channel.

int save_pgm(const char *path, unsigned char *data, int width, int height) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        printf("Could not create '%s'\n", path);
        return 0;
    }

    fprintf(file, "P5\n%d %d\n255\n", width, height); // Text header.
    fwrite(data, 1, (size_t)((long)width * height), file); // Raw pixels.

    fclose(file);
    return 1;
}

// ============================================================
// MAIN
// ============================================================

int main(void) {
    Image image;
    if (!load_ppm("../images/example.ppm", &image)) {
        return EXIT_FAILURE;
    }

    long pixel_count = (long)image.width * image.height;

    printf("=== PPM loaded successfully ===\n");
    printf("Size:        %d x %d pixels\n", image.width, image.height);
    printf("Pixels:      %ld\n", pixel_count);
    printf("Pixel data:  %ld bytes (%.1f KB)\n\n",
           pixel_count * 3, (pixel_count * 3) / 1024.0);

    printf("First 3 pixels (RGB):\n");
    for (int i = 0; i < 3; i++) {
        printf("  pixel %d: R=%3d G=%3d B=%3d\n", i,
               image.pixels[i * 3 + 0],
               image.pixels[i * 3 + 1],
               image.pixels[i * 3 + 2]);
    }
    printf("\n");

    YCbCrImage ycbcr;
    if (!convert_to_ycbcr(&image, &ycbcr)) {
        free(image.pixels);
        return EXIT_FAILURE;
    }

    printf("Same 3 pixels after RGB -> YCbCr:\n");
    for (int i = 0; i < 3; i++) {
        printf("  pixel %d: Y=%3d Cb=%3d Cr=%3d\n", i,
               ycbcr.y[i], ycbcr.cb[i], ycbcr.cr[i]);
    }
    printf("\n");

    // Save each channel so we can inspect it as an image.
    int channels_saved =
        save_pgm("../output/channel_y.pgm", ycbcr.y, image.width, image.height) &&
        save_pgm("../output/channel_cb.pgm", ycbcr.cb, image.width, image.height) &&
        save_pgm("../output/channel_cr.pgm", ycbcr.cr, image.width, image.height);

    if (channels_saved) {
        printf("Channels saved to output/: channel_y.pgm, channel_cb.pgm, channel_cr.pgm\n");
    }

    // Always give allocated memory back.
    free(image.pixels);
    free(ycbcr.y);
    free(ycbcr.cb);
    free(ycbcr.cr);

    return channels_saved ? EXIT_SUCCESS : EXIT_FAILURE;
}

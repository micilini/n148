#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppm.h"

// PPM header parsing helpers carried forward from article 15.
static void skip_whitespace_and_comments(FILE *file) {
    int character;

    for (;;) {
        character = fgetc(file);
        if (character == '#') {
            while (character != '\n' && character != EOF) {
                character = fgetc(file);
            }
        } else if (character == ' ' || character == '\t' ||
                   character == '\n' || character == '\r') {
            continue;
        } else {
            ungetc(character, file);
            return;
        }
    }
}

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

    return digits ? value : -1;
}

int load_ppm(const char *path, Image *image) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        printf("Could not open '%s'. Is the file in the images/ folder?\n", path);
        return 0;
    }

    char magic[3] = {0};
    if (fread(magic, 1, 2, file) != 2) {
        fclose(file);
        return 0;
    }
    if (magic[0] != 'P' || magic[1] != '6') {
        printf("Not a binary PPM (P6). Found '%s'.\n", magic);
        fclose(file);
        return 0;
    }

    int width = read_number(file);
    int height = read_number(file);
    int maxval = read_number(file);

    if (width <= 0 || height <= 0 || maxval != 255) {
        printf("Unsupported PPM header.\n");
        fclose(file);
        return 0;
    }

    long byte_count = (long)width * height * 3;
    unsigned char *pixels = (unsigned char *)malloc((size_t)byte_count);
    if (!pixels) {
        fclose(file);
        return 0;
    }

    if (fread(pixels, 1, (size_t)byte_count, file) != (size_t)byte_count) {
        printf("Pixel data ended too early.\n");
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

void free_image(Image *image) {
    free(image->pixels);
    image->pixels = NULL;
}

Plane create_plane(int width, int height) {
    Plane plane;
    plane.width = width;
    plane.height = height;
    plane.data = (unsigned char *)malloc((size_t)((long)width * height));
    return plane;
}

void free_plane(Plane *plane) {
    free(plane->data);
    plane->data = NULL;
}

// Clamping the coordinates is the padding for partial blocks.
int plane_sample(Plane *plane, int x, int y) {
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= plane->width) {
        x = plane->width - 1;
    }
    if (y >= plane->height) {
        y = plane->height - 1;
    }

    return plane->data[(long)y * plane->width + x];
}

static unsigned char clamp_byte(double value) {
    if (value < 0.0) {
        return 0;
    }
    if (value > 255.0) {
        return 255;
    }

    return (unsigned char)(value + 0.5);
}

int split_channels(Image *image, Plane *y, Plane *cb, Plane *cr, int subsample) {
    int width = image->width;
    int height = image->height;
    long pixel_count = (long)width * height;

    *y = create_plane(width, height);

    // Build full-resolution chroma first, then average it down if needed.
    unsigned char *cb_full = (unsigned char *)malloc((size_t)pixel_count);
    unsigned char *cr_full = (unsigned char *)malloc((size_t)pixel_count);
    if (!y->data || !cb_full || !cr_full) {
        free_plane(y);
        free(cb_full);
        free(cr_full);
        return 0;
    }

    for (long i = 0; i < pixel_count; i++) {
        double red = image->pixels[i * 3 + 0];
        double green = image->pixels[i * 3 + 1];
        double blue = image->pixels[i * 3 + 2];

        y->data[i] = clamp_byte(
            0.299000 * red + 0.587000 * green + 0.114000 * blue);
        cb_full[i] = clamp_byte(
            -0.168736 * red - 0.331264 * green + 0.500000 * blue + 128.0);
        cr_full[i] = clamp_byte(
            0.500000 * red - 0.418688 * green - 0.081312 * blue + 128.0);
    }

    if (!subsample) {
        *cb = create_plane(width, height);
        *cr = create_plane(width, height);
        if (!cb->data || !cr->data) {
            free_plane(y);
            free_plane(cb);
            free_plane(cr);
            free(cb_full);
            free(cr_full);
            return 0;
        }
        memcpy(cb->data, cb_full, (size_t)pixel_count);
        memcpy(cr->data, cr_full, (size_t)pixel_count);
    } else {
        // 4:2:0: one chroma sample for every 2x2 block of pixels.
        int chroma_width = (width + 1) / 2;
        int chroma_height = (height + 1) / 2;
        *cb = create_plane(chroma_width, chroma_height);
        *cr = create_plane(chroma_width, chroma_height);
        if (!cb->data || !cr->data) {
            free_plane(y);
            free_plane(cb);
            free_plane(cr);
            free(cb_full);
            free(cr_full);
            return 0;
        }

        for (int yy = 0; yy < chroma_height; yy++) {
            for (int xx = 0; xx < chroma_width; xx++) {
                int x0 = xx * 2;
                int y0 = yy * 2;
                int x1 = (x0 + 1 < width) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < height) ? y0 + 1 : y0;

                int sum_cb =
                    cb_full[(long)y0 * width + x0] +
                    cb_full[(long)y0 * width + x1] +
                    cb_full[(long)y1 * width + x0] +
                    cb_full[(long)y1 * width + x1];
                int sum_cr =
                    cr_full[(long)y0 * width + x0] +
                    cr_full[(long)y0 * width + x1] +
                    cr_full[(long)y1 * width + x0] +
                    cr_full[(long)y1 * width + x1];

                cb->data[(long)yy * chroma_width + xx] =
                    (unsigned char)((sum_cb + 2) / 4);
                cr->data[(long)yy * chroma_width + xx] =
                    (unsigned char)((sum_cr + 2) / 4);
            }
        }
    }

    free(cb_full);
    free(cr_full);
    return 1;
}

int save_pgm(const char *path, Plane *plane) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return 0;
    }

    fprintf(file, "P5\n%d %d\n255\n", plane->width, plane->height);
    fwrite(plane->data, 1,
           (size_t)((long)plane->width * plane->height), file);
    fclose(file);
    return 1;
}

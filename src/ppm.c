#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "header.h"
#include "ppm.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

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

int save_ppm(const char *path, Image *image) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return 0;
    }

    long byte_count = (long)image->width * image->height * 3;
    int header_ok =
        fprintf(file, "P6\n%d %d\n255\n", image->width, image->height) > 0;
    int pixels_ok =
        fwrite(image->pixels, 1, (size_t)byte_count, file) ==
        (size_t)byte_count;
    int close_ok = fclose(file) == 0;
    return header_ok && pixels_ok && close_ok;
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

#define YCC_FIX_BITS 16
#define YCC_FIX(x)   ((int)((x) * (1 << YCC_FIX_BITS) + 0.5))
#define YCC_HALF     (1 << (YCC_FIX_BITS - 1))

static const int YCC_YR  = YCC_FIX(0.299000);
static const int YCC_YG  = YCC_FIX(0.587000);
static const int YCC_YB  = YCC_FIX(0.114000);
static const int YCC_CBR = YCC_FIX(0.168736);
static const int YCC_CBG = YCC_FIX(0.331264);
static const int YCC_CBB = YCC_FIX(0.500000);
static const int YCC_CRR = YCC_FIX(0.500000);
static const int YCC_CRG = YCC_FIX(0.418688);
static const int YCC_CRB = YCC_FIX(0.081312);

static unsigned char clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (unsigned char)value;
}

static void rgb_to_ycbcr_scalar(const unsigned char *rgb,
                                unsigned char *y,
                                unsigned char *cb,
                                unsigned char *cr,
                                long count) {
    const int offset = 128 << YCC_FIX_BITS;

    for (long i = 0; i < count; i++) {
        int red = rgb[i * 3 + 0];
        int green = rgb[i * 3 + 1];
        int blue = rgb[i * 3 + 2];

        int luma = YCC_YR * red + YCC_YG * green + YCC_YB * blue;
        int blue_difference = offset - YCC_CBR * red - YCC_CBG * green +
                              YCC_CBB * blue;
        int red_difference = offset + YCC_CRR * red - YCC_CRG * green -
                             YCC_CRB * blue;

        y[i] = clamp_byte((luma + YCC_HALF) >> YCC_FIX_BITS);
        cb[i] = clamp_byte((blue_difference + YCC_HALF) >> YCC_FIX_BITS);
        cr[i] = clamp_byte((red_difference + YCC_HALF) >> YCC_FIX_BITS);
    }
}

#if defined(__x86_64__) || defined(__i386__)

// Eight interleaved RGB pixels are split into three vectors with pshufb,
// converted with the same fixed-point arithmetic as the scalar path, and
// narrowed with unsigned saturation so packing also performs the clamp.
__attribute__((target("avx2")))
static long rgb_to_ycbcr_avx2(const unsigned char *rgb,
                              unsigned char *y,
                              unsigned char *cb,
                              unsigned char *cr,
                              long count) {
    const __m256i shuffle_r = _mm256_setr_epi8(
        0,-1,-1,-1, 3,-1,-1,-1, 6,-1,-1,-1, 9,-1,-1,-1,
        0,-1,-1,-1, 3,-1,-1,-1, 6,-1,-1,-1, 9,-1,-1,-1);
    const __m256i shuffle_g = _mm256_setr_epi8(
        1,-1,-1,-1, 4,-1,-1,-1, 7,-1,-1,-1,10,-1,-1,-1,
        1,-1,-1,-1, 4,-1,-1,-1, 7,-1,-1,-1,10,-1,-1,-1);
    const __m256i shuffle_b = _mm256_setr_epi8(
        2,-1,-1,-1, 5,-1,-1,-1, 8,-1,-1,-1,11,-1,-1,-1,
        2,-1,-1,-1, 5,-1,-1,-1, 8,-1,-1,-1,11,-1,-1,-1);

    const __m256i yr = _mm256_set1_epi32(YCC_YR);
    const __m256i yg = _mm256_set1_epi32(YCC_YG);
    const __m256i yb = _mm256_set1_epi32(YCC_YB);
    const __m256i cbr = _mm256_set1_epi32(YCC_CBR);
    const __m256i cbg = _mm256_set1_epi32(YCC_CBG);
    const __m256i cbb = _mm256_set1_epi32(YCC_CBB);
    const __m256i crr = _mm256_set1_epi32(YCC_CRR);
    const __m256i crg = _mm256_set1_epi32(YCC_CRG);
    const __m256i crb = _mm256_set1_epi32(YCC_CRB);
    const __m256i half = _mm256_set1_epi32(YCC_HALF);
    const __m256i offset = _mm256_set1_epi32(128 << YCC_FIX_BITS);

    long i = 0;
    // Both 16-byte loads stay inside the input while a scalar tail handles
    // the final pixels that cannot form a safe eight-pixel group.
    for (; i + 16 <= count; i += 8) {
        const unsigned char *p = rgb + i * 3;
        __m128i lo = _mm_loadu_si128((const __m128i *)p);
        __m128i hi = _mm_loadu_si128((const __m128i *)(p + 12));
        __m256i packed = _mm256_set_m128i(hi, lo);

        __m256i red = _mm256_shuffle_epi8(packed, shuffle_r);
        __m256i green = _mm256_shuffle_epi8(packed, shuffle_g);
        __m256i blue = _mm256_shuffle_epi8(packed, shuffle_b);

        __m256i luma = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_mullo_epi32(red, yr),
                             _mm256_mullo_epi32(green, yg)),
            _mm256_add_epi32(_mm256_mullo_epi32(blue, yb), half));
        __m256i blue_difference = _mm256_add_epi32(
            _mm256_sub_epi32(
                _mm256_sub_epi32(offset, _mm256_mullo_epi32(red, cbr)),
                _mm256_mullo_epi32(green, cbg)),
            _mm256_add_epi32(_mm256_mullo_epi32(blue, cbb), half));
        __m256i red_difference = _mm256_add_epi32(
            _mm256_sub_epi32(
                _mm256_add_epi32(offset, _mm256_mullo_epi32(red, crr)),
                _mm256_mullo_epi32(green, crg)),
            _mm256_sub_epi32(half, _mm256_mullo_epi32(blue, crb)));

        luma = _mm256_srai_epi32(luma, YCC_FIX_BITS);
        blue_difference = _mm256_srai_epi32(blue_difference, YCC_FIX_BITS);
        red_difference = _mm256_srai_epi32(red_difference, YCC_FIX_BITS);

        __m128i y16 = _mm_packs_epi32(_mm256_castsi256_si128(luma),
                                      _mm256_extracti128_si256(luma, 1));
        __m128i cb16 = _mm_packs_epi32(
            _mm256_castsi256_si128(blue_difference),
            _mm256_extracti128_si256(blue_difference, 1));
        __m128i cr16 = _mm_packs_epi32(
            _mm256_castsi256_si128(red_difference),
            _mm256_extracti128_si256(red_difference, 1));

        _mm_storel_epi64((__m128i *)(y + i), _mm_packus_epi16(y16, y16));
        _mm_storel_epi64((__m128i *)(cb + i),
                         _mm_packus_epi16(cb16, cb16));
        _mm_storel_epi64((__m128i *)(cr + i),
                         _mm_packus_epi16(cr16, cr16));
    }

    return i;
}

#endif

void chroma_dimensions(int mode, int width, int height, int *cw, int *ch) {
    switch (mode) {
        case CHROMA_422:
            *cw = (width + 1) / 2;
            *ch = height;
            break;
        case CHROMA_420:
            *cw = (width + 1) / 2;
            *ch = (height + 1) / 2;
            break;
        default:
            *cw = width;
            *ch = height;
            break;
    }
}

// Average all full-resolution samples represented by one stored chroma
// sample. Clamping duplicates the final row or column for odd dimensions.
static void downsample_plane(unsigned char *full, int width, int height,
                             Plane *output, int step_x, int step_y) {
    for (int y = 0; y < output->height; y++) {
        for (int x = 0; x < output->width; x++) {
            int sum = 0;
            int count = 0;

            for (int dy = 0; dy < step_y; dy++) {
                for (int dx = 0; dx < step_x; dx++) {
                    int source_x = x * step_x + dx;
                    int source_y = y * step_y + dy;
                    if (source_x >= width) {
                        source_x = width - 1;
                    }
                    if (source_y >= height) {
                        source_y = height - 1;
                    }
                    sum += full[(long)source_y * width + source_x];
                    count++;
                }
            }

            output->data[(long)y * output->width + x] =
                (unsigned char)((sum + count / 2) / count);
        }
    }
}

int split_channels(Image *image, Plane *y, Plane *cb, Plane *cr,
                   int chroma_mode) {
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

    long converted = 0;
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        converted = rgb_to_ycbcr_avx2(image->pixels, y->data,
                                      cb_full, cr_full, pixel_count);
    }
#endif
    rgb_to_ycbcr_scalar(image->pixels + converted * 3,
                        y->data + converted,
                        cb_full + converted,
                        cr_full + converted,
                        pixel_count - converted);

    int chroma_width;
    int chroma_height;
    chroma_dimensions(chroma_mode, width, height,
                      &chroma_width, &chroma_height);
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

    int step_x = (chroma_mode == CHROMA_444) ? 1 : 2;
    int step_y = (chroma_mode == CHROMA_420) ? 2 : 1;
    downsample_plane(cb_full, width, height, cb, step_x, step_y);
    downsample_plane(cr_full, width, height, cr, step_x, step_y);

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

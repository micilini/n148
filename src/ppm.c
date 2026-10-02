#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "header.h"
#include <string.h>
#include "ppm.h"
#include "cpu.h"
#include "parallel.h"
#include "perceptual.h"
#include "directional_intra.h"

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

void chroma_dimensions(int mode, int width, int height, int *cw, int *ch) {
    switch (mode) {
        case CHROMA_422:
            *cw = width / 2 + (width & 1);
            *ch = height;
            break;
        case CHROMA_420:
            *cw = width / 2 + (width & 1);
            *ch = height / 2 + (height & 1);
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
    // The 2x2 case is by far the most common, so it gets a tight loop
    // with no per-sample bounds checks in the interior.
    if (step_x == 2 && step_y == 2 && (width & 1) == 0 && (height & 1) == 0) {
        for (int y = 0; y < output->height; y++) {
            const unsigned char *r0 = full + (long)(y*2)     * width;
            const unsigned char *r1 = full + (long)(y*2 + 1) * width;
            unsigned char *dst = output->data + (long) y * output->width;
            for (int x = 0; x < output->width; x++) {
                int sx = x * 2;
                dst[x] = (unsigned char)((r0[sx] + r0[sx+1] + r1[sx] + r1[sx+1] + 2) >> 2);
            }
        }
        return;
    }
    if (step_x == 1 && step_y == 1) {
        for (int y = 0; y < output->height; y++)
            memcpy(output->data + (long) y * output->width,
                   full + (long) y * width, output->width);
        return;
    }

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

// ============================================================
// RGB -> YCbCr
// ============================================================
//
// The scalar version below is the readable reference. The vector
// version does the same arithmetic on eight pixels at a time, using
// fixed point integers: the coefficients are pre-multiplied by 2^16
// so the whole conversion stays in integer registers, which keeps
// every lane busy and avoids float conversions per pixel.

#define YCC_FIX_BITS 16
#define YCC_FIX(x)   ((int)((x) * (1 << YCC_FIX_BITS) + 0.5))
#define YCC_HALF     (1 << (YCC_FIX_BITS - 1))
#define YCC_OFFSET   ((128 << YCC_FIX_BITS) + YCC_HALF)

static const int YCC_YR =  YCC_FIX(0.299000);
static const int YCC_YG =  YCC_FIX(0.587000);
static const int YCC_YB =  YCC_FIX(0.114000);
static const int YCC_CBR = -YCC_FIX(0.168736);
static const int YCC_CBG = -YCC_FIX(0.331264);
static const int YCC_CBB =  YCC_FIX(0.500000);
static const int YCC_CRR =  YCC_FIX(0.500000);
static const int YCC_CRG = -YCC_FIX(0.418688);
static const int YCC_CRB = -YCC_FIX(0.081312);

static void rgb_to_ycbcr_scalar(const unsigned char *rgb, long count,
                                unsigned char *y, unsigned char *cb,
                                unsigned char *cr) {
    for (long i = 0; i < count; i++) {
        int red   = rgb[i*3 + 0];
        int green = rgb[i*3 + 1];
        int blue  = rgb[i*3 + 2];

        int luma = (YCC_YR * red + YCC_YG * green + YCC_YB * blue + YCC_HALF)
                   >> YCC_FIX_BITS;
        int blue_diff = (YCC_CBR * red + YCC_CBG * green + YCC_CBB * blue
                         + YCC_OFFSET) >> YCC_FIX_BITS;
        int red_diff  = (YCC_CRR * red + YCC_CRG * green + YCC_CRB * blue
                         + YCC_OFFSET) >> YCC_FIX_BITS;

        y[i]  = (unsigned char)(luma      < 0 ? 0 : luma      > 255 ? 255 : luma);
        cb[i] = (unsigned char)(blue_diff < 0 ? 0 : blue_diff > 255 ? 255 : blue_diff);
        cr[i] = (unsigned char)(red_diff  < 0 ? 0 : red_diff  > 255 ? 255 : red_diff);
    }
}

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

__attribute__((target("avx2"), always_inline))
static inline void convert_rgb8_avx2(const unsigned char *rgb,
                                     __m128i *packed_y,
                                     __m128i *packed_cb,
                                     __m128i *packed_cr) {
    const __m256i shuffle_r = _mm256_setr_epi8(
        0,-1,-1,-1, 3,-1,-1,-1, 6,-1,-1,-1, 9,-1,-1,-1,
        0,-1,-1,-1, 3,-1,-1,-1, 6,-1,-1,-1, 9,-1,-1,-1);
    const __m256i shuffle_g = _mm256_setr_epi8(
        1,-1,-1,-1, 4,-1,-1,-1, 7,-1,-1,-1,10,-1,-1,-1,
        1,-1,-1,-1, 4,-1,-1,-1, 7,-1,-1,-1,10,-1,-1,-1);
    const __m256i shuffle_b = _mm256_setr_epi8(
        2,-1,-1,-1, 5,-1,-1,-1, 8,-1,-1,-1,11,-1,-1,-1,
        2,-1,-1,-1, 5,-1,-1,-1, 8,-1,-1,-1,11,-1,-1,-1);
    __m128i lo = _mm_loadu_si128((const __m128i *)rgb);
    __m128i hi = _mm_loadu_si128((const __m128i *)(rgb + 12));
    __m256i source = _mm256_set_m128i(hi, lo);
    __m256i r = _mm256_shuffle_epi8(source, shuffle_r);
    __m256i g = _mm256_shuffle_epi8(source, shuffle_g);
    __m256i b = _mm256_shuffle_epi8(source, shuffle_b);
    __m256i rb = _mm256_sub_epi32(r, b);
    __m256i gb = _mm256_sub_epi32(g, b);
    __m256i pair_low = _mm256_unpacklo_epi32(rb, gb);
    __m256i pair_high = _mm256_unpackhi_epi32(rb, gb);
    __m256i differences = _mm256_packs_epi32(pair_low, pair_high);
    /* rb and gb fit signed 16 bits. Madd calculates both color terms per
       pixel; correct coefficients above INT16_MAX with one exact shift. */
    const __m256i y_coefficients = _mm256_set1_epi32(
        (int32_t)((uint16_t) YCC_YR | ((uint32_t)(uint16_t) YCC_YG << 16)));
    const __m256i cb_coefficients = _mm256_set1_epi32(
        (int32_t)((uint16_t) YCC_CBR | ((uint32_t)(uint16_t) YCC_CBG << 16)));
    const __m256i cr_coefficients = _mm256_set1_epi32(
        (int32_t)((uint16_t) YCC_CRR | ((uint32_t)(uint16_t) YCC_CRG << 16)));
    __m256i luma = _mm256_add_epi32(
        _mm256_add_epi32(_mm256_madd_epi16(differences, y_coefficients),
                         _mm256_slli_epi32(gb, 16)),
        _mm256_add_epi32(
            _mm256_slli_epi32(b, YCC_FIX_BITS),
            _mm256_set1_epi32(YCC_HALF)));
    __m256i blue_diff = _mm256_add_epi32(
        _mm256_madd_epi16(differences, cb_coefficients),
        _mm256_set1_epi32(YCC_OFFSET));
    __m256i red_diff = _mm256_add_epi32(
        _mm256_add_epi32(_mm256_madd_epi16(differences, cr_coefficients),
                         _mm256_slli_epi32(rb, 16)),
        _mm256_set1_epi32(YCC_OFFSET));
    luma = _mm256_srai_epi32(luma, YCC_FIX_BITS);
    blue_diff = _mm256_srai_epi32(blue_diff, YCC_FIX_BITS);
    red_diff = _mm256_srai_epi32(red_diff, YCC_FIX_BITS);
    __m128i y16 = _mm_packs_epi32(
        _mm256_castsi256_si128(luma), _mm256_extracti128_si256(luma, 1));
    __m128i cb16 = _mm_packs_epi32(
        _mm256_castsi256_si128(blue_diff),
        _mm256_extracti128_si256(blue_diff, 1));
    __m128i cr16 = _mm_packs_epi32(
        _mm256_castsi256_si128(red_diff),
        _mm256_extracti128_si256(red_diff, 1));
    *packed_y = _mm_packus_epi16(y16, y16);
    *packed_cb = _mm_packus_epi16(cb16, cb16);
    *packed_cr = _mm_packus_epi16(cr16, cr16);
}

__attribute__((target("avx2")))
static void rgb_to_ycbcr_avx2(const unsigned char *rgb, long count,
                              unsigned char *y, unsigned char *cb,
                              unsigned char *cr) {
    long i = 0;
    // Stop early: the high lane reads twelve bytes ahead, so the tail
    // is finished by the scalar routine to stay inside the buffer.
    for (; i + 12 <= count; i += 8) {
        const unsigned char *p = rgb + i * 3;
        __m128i y8, cb8, cr8;
        convert_rgb8_avx2(p, &y8, &cb8, &cr8);
        _mm_storel_epi64((__m128i *)(y + i), y8);
        _mm_storel_epi64((__m128i *)(cb + i), cb8);
        _mm_storel_epi64((__m128i *)(cr + i), cr8);
    }

    if (i < count)
        rgb_to_ycbcr_scalar(rgb + i*3, count - i, y + i, cb + i, cr + i);
}
#endif

typedef struct {
    const unsigned char *rgb;
    unsigned char *y, *cb, *cr;
    int width;
    int perceptual_color;
} ColorJob;

static void rgb_to_ycbcr(const unsigned char *rgb, long count,
                         unsigned char *y, unsigned char *cb, unsigned char *cr) {
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        rgb_to_ycbcr_avx2(rgb, count, y, cb, cr);
        return;
    }
#endif
    rgb_to_ycbcr_scalar(rgb, count, y, cb, cr);
}

static void rgb_to_coding_planes(const unsigned char *rgb, long count,
                                 unsigned char *first,
                                 unsigned char *second,
                                 unsigned char *third,
                                 int perceptual_color) {
    if (perceptual_color) {
        n148_rgb_to_perceptual(rgb, count, first, second, third);
        return;
    }
    rgb_to_ycbcr(rgb, count, first, second, third);
}

static void color_rows(long start, long end, int worker, void *context) {
    (void) worker;
    ColorJob *job = (ColorJob *) context;
    for (long row = start; row < end; row++) {
        long offset = row * job->width;
        rgb_to_coding_planes(job->rgb + offset * 3, job->width,
                             job->y + offset, job->cb + offset,
                             job->cr + offset, job->perceptual_color);
    }
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static void downsample_420_rows_avx2(const unsigned char *cb0,
                                     const unsigned char *cb1,
                                     const unsigned char *cr0,
                                     const unsigned char *cr1,
                                     int width, unsigned char *out_cb,
                                     unsigned char *out_cr, int out_width) {
    const __m256i ones = _mm256_set1_epi8(1);
    const __m256i two = _mm256_set1_epi16(2);
    int x = 0;
    for (; x + 16 <= out_width && 2 * x + 32 <= width; x += 16) {
        int source = x * 2;
        __m256i cb_top = _mm256_loadu_si256((const __m256i *)(cb0 + source));
        __m256i cb_bottom = _mm256_loadu_si256((const __m256i *)(cb1 + source));
        __m256i cr_top = _mm256_loadu_si256((const __m256i *)(cr0 + source));
        __m256i cr_bottom = _mm256_loadu_si256((const __m256i *)(cr1 + source));
        __m256i cb_sum = _mm256_add_epi16(_mm256_maddubs_epi16(cb_top, ones),
                                          _mm256_maddubs_epi16(cb_bottom, ones));
        __m256i cr_sum = _mm256_add_epi16(_mm256_maddubs_epi16(cr_top, ones),
                                          _mm256_maddubs_epi16(cr_bottom, ones));
        cb_sum = _mm256_srli_epi16(_mm256_add_epi16(cb_sum, two), 2);
        cr_sum = _mm256_srli_epi16(_mm256_add_epi16(cr_sum, two), 2);
        __m128i cb_bytes = _mm_packus_epi16(_mm256_castsi256_si128(cb_sum),
                                            _mm256_extracti128_si256(cb_sum, 1));
        __m128i cr_bytes = _mm_packus_epi16(_mm256_castsi256_si128(cr_sum),
                                            _mm256_extracti128_si256(cr_sum, 1));
        _mm_storeu_si128((__m128i *)(out_cb + x), cb_bytes);
        _mm_storeu_si128((__m128i *)(out_cr + x), cr_bytes);
    }
    for (; x < out_width; x++) {
        int source = x * 2;
        int next = source + 1 < width ? source + 1 : width - 1;
        out_cb[x] = (unsigned char)((cb0[source] + cb0[next] +
                                     cb1[source] + cb1[next] + 2) >> 2);
        out_cr[x] = (unsigned char)((cr0[source] + cr0[next] +
                                     cr1[source] + cr1[next] + 2) >> 2);
    }
}

/* Convert two RGB rows and immediately reduce their chroma to 4:2:0. This
   keeps the exact per-pixel color transform and the existing rounded 2x2
   average while avoiding four full-width temporary chroma rows. */
__attribute__((target("avx2")))
static void rgb_to_ycbcr_420_rows_avx2(
    const unsigned char *rgb0, const unsigned char *rgb1, int width,
    unsigned char *y0, unsigned char *y1,
    unsigned char *out_cb, unsigned char *out_cr, int out_width) {
    const __m128i ones = _mm_set1_epi8(1);
    const __m128i two = _mm_set1_epi16(2);
    int source = 0;
    int output = 0;
    for (; source + 20 <= width; source += 16, output += 8) {
        __m128i y_a, cb_a, cr_a, y_b, cb_b, cr_b;
        __m128i cb_c, cr_c, cb_d, cr_d;
        convert_rgb8_avx2(rgb0 + source * 3, &y_a, &cb_a, &cr_a);
        convert_rgb8_avx2(rgb0 + (source + 8) * 3, &y_b, &cb_b, &cr_b);
        _mm_storeu_si128((__m128i *)(y0 + source),
                         _mm_unpacklo_epi64(y_a, y_b));
        if (rgb1) {
            __m128i y_c, y_d;
            convert_rgb8_avx2(rgb1 + source * 3,
                              &y_c, &cb_c, &cr_c);
            convert_rgb8_avx2(rgb1 + (source + 8) * 3,
                              &y_d, &cb_d, &cr_d);
            _mm_storeu_si128((__m128i *)(y1 + source),
                             _mm_unpacklo_epi64(y_c, y_d));
        } else {
            cb_c = cb_a; cr_c = cr_a;
            cb_d = cb_b; cr_d = cr_b;
        }
        __m128i cb_top = _mm_unpacklo_epi64(cb_a, cb_b);
        __m128i cb_bottom = _mm_unpacklo_epi64(cb_c, cb_d);
        __m128i cr_top = _mm_unpacklo_epi64(cr_a, cr_b);
        __m128i cr_bottom = _mm_unpacklo_epi64(cr_c, cr_d);
        __m128i cb_sum = _mm_add_epi16(
            _mm_maddubs_epi16(cb_top, ones),
            _mm_maddubs_epi16(cb_bottom, ones));
        __m128i cr_sum = _mm_add_epi16(
            _mm_maddubs_epi16(cr_top, ones),
            _mm_maddubs_epi16(cr_bottom, ones));
        cb_sum = _mm_srli_epi16(_mm_add_epi16(cb_sum, two), 2);
        cr_sum = _mm_srli_epi16(_mm_add_epi16(cr_sum, two), 2);
        _mm_storel_epi64((__m128i *)(out_cb + output),
            _mm_packus_epi16(cb_sum, cb_sum));
        _mm_storel_epi64((__m128i *)(out_cr + output),
            _mm_packus_epi16(cr_sum, cr_sum));
    }
    for (; source + 12 <= width; source += 8, output += 4) {
        __m128i top_y, top_cb, top_cr;
        __m128i bottom_y, bottom_cb, bottom_cr;
        convert_rgb8_avx2(rgb0 + source * 3,
                          &top_y, &top_cb, &top_cr);
        _mm_storel_epi64((__m128i *)(y0 + source), top_y);
        if (rgb1) {
            convert_rgb8_avx2(rgb1 + source * 3,
                              &bottom_y, &bottom_cb, &bottom_cr);
            _mm_storel_epi64((__m128i *)(y1 + source), bottom_y);
        } else {
            bottom_cb = top_cb;
            bottom_cr = top_cr;
        }
        __m128i cb_sum = _mm_add_epi16(
            _mm_maddubs_epi16(top_cb, ones),
            _mm_maddubs_epi16(bottom_cb, ones));
        __m128i cr_sum = _mm_add_epi16(
            _mm_maddubs_epi16(top_cr, ones),
            _mm_maddubs_epi16(bottom_cr, ones));
        cb_sum = _mm_srli_epi16(_mm_add_epi16(cb_sum, two), 2);
        cr_sum = _mm_srli_epi16(_mm_add_epi16(cr_sum, two), 2);
        uint32_t cb4 = (uint32_t)_mm_cvtsi128_si32(
            _mm_packus_epi16(cb_sum, cb_sum));
        uint32_t cr4 = (uint32_t)_mm_cvtsi128_si32(
            _mm_packus_epi16(cr_sum, cr_sum));
        memcpy(out_cb + output, &cb4, sizeof(cb4));
        memcpy(out_cr + output, &cr4, sizeof(cr4));
    }

    unsigned char top_cb[12], top_cr[12];
    unsigned char bottom_cb[12], bottom_cr[12];
    int remaining = width - source;
    if (remaining > 0) {
        rgb_to_ycbcr_scalar(rgb0 + source * 3, remaining,
                            y0 + source, top_cb, top_cr);
        if (rgb1) {
            rgb_to_ycbcr_scalar(rgb1 + source * 3, remaining,
                                y1 + source, bottom_cb, bottom_cr);
        } else {
            memcpy(bottom_cb, top_cb, (size_t)remaining);
            memcpy(bottom_cr, top_cr, (size_t)remaining);
        }
    }
    for (; output < out_width; output++) {
        int first = output * 2 - source;
        int next = first + 1;
        if (next >= remaining) next = remaining - 1;
        out_cb[output] = (unsigned char)((top_cb[first] + top_cb[next] +
                                          bottom_cb[first] +
                                          bottom_cb[next] + 2) >> 2);
        out_cr[output] = (unsigned char)((top_cr[first] + top_cr[next] +
                                          bottom_cr[first] +
                                          bottom_cr[next] + 2) >> 2);
    }
}
#endif

typedef struct {
    const Image *image;
    Plane *y, *cb, *cr;
    int use_avx2;
    int perceptual_color;
    int failed;
} Color420Job;

static void color_420_rows(long start, long end, int worker, void *context) {
    (void)worker;
    Color420Job *job = (Color420Job *)context;
    int width = job->image->width;
    int height = job->image->height;

#if defined(__x86_64__) || defined(__i386__)
    if (job->use_avx2 && !job->perceptual_color) {
        for (long cy = start; cy < end; cy++) {
            int row0 = (int)cy * 2;
            int row1 = row0 + 1;
            const unsigned char *rgb0 =
                job->image->pixels + (long)row0 * width * 3;
            const unsigned char *rgb1 = row1 < height ?
                job->image->pixels + (long)row1 * width * 3 : NULL;
            unsigned char *y0 = job->y->data + (long)row0 * width;
            unsigned char *y1 = row1 < height ?
                job->y->data + (long)row1 * width : NULL;
            rgb_to_ycbcr_420_rows_avx2(
                rgb0, rgb1, width, y0, y1,
                job->cb->data + cy * job->cb->width,
                job->cr->data + cy * job->cr->width, job->cb->width);
        }
        return;
    }
#endif

#define COLOR_STACK_WIDTH 2048
    _Alignas(32) unsigned char stack_rows[COLOR_STACK_WIDTH * 4];
    unsigned char *allocated = NULL;
    unsigned char *cb0;
    if (width <= COLOR_STACK_WIDTH) {
        cb0 = stack_rows;
    } else {
        allocated = (unsigned char *)malloc((size_t)width * 4);
        if (!allocated) {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&job->failed, 1, __ATOMIC_RELAXED);
#else
            job->failed = 1;
#endif
            return;
        }
        cb0 = allocated;
    }
    unsigned char *cr0 = cb0 + width;
    unsigned char *cb1 = cr0 + width;
    unsigned char *cr1 = cb1 + width;

    for (long cy = start; cy < end; cy++) {
        int row0 = (int)cy * 2;
        int row1 = row0 + 1;
        const unsigned char *rgb0 = job->image->pixels + (long)row0 * width * 3;
        rgb_to_coding_planes(rgb0, width,
                             job->y->data + (long)row0 * width, cb0, cr0,
                             job->perceptual_color);
        if (row1 < height) {
            const unsigned char *rgb1 = job->image->pixels + (long)row1 * width * 3;
            rgb_to_coding_planes(rgb1, width,
                                 job->y->data + (long)row1 * width, cb1, cr1,
                                 job->perceptual_color);
        } else {
            memcpy(cb1, cb0, (size_t)width);
            memcpy(cr1, cr0, (size_t)width);
        }

        unsigned char *out_cb = job->cb->data + cy * job->cb->width;
        unsigned char *out_cr = job->cr->data + cy * job->cr->width;
#if defined(__x86_64__) || defined(__i386__)
        if (job->use_avx2) {
            downsample_420_rows_avx2(cb0, cb1, cr0, cr1, width,
                                     out_cb, out_cr, job->cb->width);
            continue;
        }
#endif
        for (int x = 0; x < job->cb->width; x++) {
            int source = x * 2;
            int next = source + 1 < width ? source + 1 : width - 1;
            out_cb[x] = (unsigned char)((cb0[source] + cb0[next] +
                                         cb1[source] + cb1[next] + 2) >> 2);
            out_cr[x] = (unsigned char)((cr0[source] + cr0[next] +
                                         cr1[source] + cr1[next] + 2) >> 2);
        }
    }
    free(allocated);
#undef COLOR_STACK_WIDTH
}

/* Return one coefficient of the decoder's one-dimensional reconstruction
   kernel. A full-resolution axis is the identity scaled by four; a reduced
   axis uses the decoder's 3:1 interpolation. Keeping both cases on the same
   scale makes the two-dimensional product equal to the decoder's x16 chroma
   representation for both 4:2:2 and 4:2:0. */
static int reconstruction_weight_1d(int source_size, int output_size,
                                    int output_index, int source_index) {
    if (source_size <= 0 || source_index < 0 ||
        source_index >= source_size || output_index < 0 ||
        output_index >= output_size) return 0;
    if (source_size == output_size)
        return source_index == output_index ? 4 : 0;
    if (source_size == 1)
        return source_index == 0 ? 4 : 0;
    if (output_index == 0)
        return source_index == 0 ? 4 : 0;
    if (output_index == 1)
        return source_index == 0 ? 3 : source_index == 1 ? 1 : 0;

    int last = source_size - 1;
    if (output_index >= source_size * 2)
        return source_index == last ? 4 : 0;
    if (output_index == last * 2)
        return source_index == last ? 3 :
            source_index == last - 1 ? 1 : 0;
    if (output_index == last * 2 + 1)
        return source_index == last ? 4 : 0;

    int centre = output_index >> 1;
    if ((output_index & 1) == 0)
        return source_index == centre ? 3 :
            source_index == centre - 1 ? 1 : 0;
    return source_index == centre ? 3 :
        source_index == centre + 1 ? 1 : 0;
}

static void sample_support(int sample, int source_size, int output_size,
                           int *first, int *last) {
    if (source_size == output_size) {
        *first = sample;
        *last = sample;
        return;
    }
    int begin = sample * 2 - 1;
    int end = sample * 2 + 2;
    if (begin < 0) begin = 0;
    if (end >= output_size) end = output_size - 1;
    *first = begin;
    *last = end;
}

typedef struct {
    int first;
    int count;
    int weights[4];
    int squared_sum;
} ChromaReconstructionSupport;

static void prepare_reconstruction_support(
    int sample, int source_size, int output_size,
    ChromaReconstructionSupport *support) {
    int last;
    sample_support(sample, source_size, output_size, &support->first, &last);
    support->count = last - support->first + 1;
    support->squared_sum = 0;
    for (int offset = 0; offset < support->count; offset++) {
        int weight = reconstruction_weight_1d(
            source_size, output_size, support->first + offset, sample);
        support->weights[offset] = weight;
        support->squared_sum += weight * weight;
    }
}

/* Each coordinate solves denominator*x*x - 2*numerator*x on [0,255].
   The nearest integer is the unique minimum except at an exact half-integer.
   The old loop retains old_value on a tie, otherwise the lower tied value. */
static int best_chroma_value(int old_value, long long numerator,
                                 long long denominator) {
    long long rounded = numerator >= 0 ?
        (numerator + denominator / 2) / denominator :
        -((-numerator + denominator / 2) / denominator);
    int candidate = rounded < 0 ? 0 : rounded > 255 ? 255 : (int)rounded;
    if (candidate > 0 && candidate != old_value && !(denominator & 1) &&
        numerator == (long long)candidate * denominator - denominator / 2)
        candidate--;
    return candidate;
}

/* Gather the existing separable 3:1 kernel once per output pixel. The old
   initialization scatters the same four exact products from source supports.
   A full-resolution axis has neighbour == centre, giving weight four. */
static void initialize_chroma(const Plane *stored, int width, int height,
                                   int *reconstruction) {
    for (int y = 0; y < height; y++) {
        int cy = stored->height == height ? y : y/2;
        int ny = stored->height == height ? cy : cy + (y&1 ? 1 : -1);
        if (ny < 0) ny = 0;
        if (ny >= stored->height) ny = stored->height-1;
        const unsigned char *row = stored->data+(long)cy*stored->width;
        const unsigned char *neighbor = stored->data+(long)ny*stored->width;
        for (int x = 0; x < width; x++) {
            int cx = stored->width == width ? x : x/2;
            int nx = stored->width == width ? cx : cx + (x&1 ? 1 : -1);
            if (nx < 0) nx = 0;
            if (nx >= stored->width) nx = stored->width-1;
            reconstruction[(long)y*width+x] =
                9*row[cx] + 3*row[nx] + 3*neighbor[cx] + neighbor[nx];
        }
    }
}

static int best_chroma_reconstruction_value(int old_value,
                                             long long numerator,
                                             long long denominator) {
    return best_chroma_value(old_value,numerator,denominator);
}

#if defined(__x86_64__) || defined(__i386__)
/* A complete 4:2:0 support has the separable weights 1,3,3,1 on each axis.
   Accumulating its 16 exact integer products four pixels at a time avoids
   repeated support traversal in the coordinate-descent sweeps. */
__attribute__((target("avx2")))
static void refine_chroma_sample_simd4(
    const unsigned char *target, int width, Plane *stored,
    int *reconstruction, int sample_x, int sample_y,
    const ChromaReconstructionSupport *x_support,
    const ChromaReconstructionSupport *y_support) {
    long sample_index = (long) sample_y * stored->width + sample_x;
    int old_value = stored->data[sample_index];
    __m128i old_vector = _mm_set1_epi32(old_value);
    __m128i sum = _mm_setzero_si128();
    for (int yi = 0; yi < 4; yi++) {
        int weight_y = yi == 0 || yi == 3 ? 1 : 3;
        __m128i weights = _mm_setr_epi32(
            weight_y, 3 * weight_y, 3 * weight_y, weight_y);
        long index = (long)(y_support->first + yi) * width +
            x_support->first;
        uint32_t packed;
        memcpy(&packed, target + index, sizeof(packed));
        __m128i desired = _mm_slli_epi32(
            _mm_cvtepu8_epi32(_mm_cvtsi32_si128((int) packed)), 4);
        __m128i current = _mm_loadu_si128(
            (const __m128i *)(reconstruction + index));
        __m128i without = _mm_sub_epi32(
            current, _mm_mullo_epi32(weights, old_vector));
        sum = _mm_add_epi32(sum, _mm_mullo_epi32(
            weights, _mm_sub_epi32(desired, without)));
    }
    __m128i high = _mm_srli_si128(sum, 8);
    sum = _mm_add_epi32(sum, high);
    high = _mm_srli_si128(sum, 4);
    sum = _mm_add_epi32(sum, high);
    long long numerator = (int32_t) _mm_cvtsi128_si32(sum);
    /* Four supported samples have weights 1, 3, 3, 1 on each axis.
       Their squared sums are both 20, so the exact divisor is 400.
       Keeping it constant also lets the compiler avoid integer division
       by a value loaded from the support tables for every chroma sample. */
    const long long denominator = 400;
    int best_value = best_chroma_reconstruction_value(
        old_value, numerator, denominator);
    if (best_value == old_value) return;

    int change = best_value - old_value;
    stored->data[sample_index] = (unsigned char) best_value;
    __m128i change_vector = _mm_set1_epi32(change);
    for (int yi = 0; yi < 4; yi++) {
        int weight_y = yi == 0 || yi == 3 ? 1 : 3;
        __m128i weights = _mm_setr_epi32(
            weight_y, 3 * weight_y, 3 * weight_y, weight_y);
        long index = (long)(y_support->first + yi) * width +
            x_support->first;
        __m128i current = _mm_loadu_si128(
            (const __m128i *)(reconstruction + index));
        _mm_storeu_si128((__m128i *)(reconstruction + index),
                        _mm_add_epi32(current,
                            _mm_mullo_epi32(weights, change_vector)));
    }
}
#endif

static void refine_chroma_sample(
    const unsigned char *target, int width,
    Plane *stored, int *reconstruction, int sample_x, int sample_y,
    const ChromaReconstructionSupport *x_support,
    const ChromaReconstructionSupport *y_support, int use_avx2) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2 && x_support->count == 4 && y_support->count == 4) {
        refine_chroma_sample_simd4(target, width, stored, reconstruction,
                                   sample_x, sample_y, x_support, y_support);
        return;
    }
#else
    (void) use_avx2;
#endif
    long sample_index = (long) sample_y * stored->width + sample_x;
    int old_value = stored->data[sample_index];
    long long numerator = 0;
    /* The squared two-dimensional weights factor into the axis sums. */
    long long denominator = (long long) x_support->squared_sum *
                            y_support->squared_sum;

    for (int yi = 0; yi < y_support->count; yi++) {
        int py = y_support->first + yi;
        int weight_y = y_support->weights[yi];
        for (int xi = 0; xi < x_support->count; xi++) {
            int px = x_support->first + xi;
            int weight_x = x_support->weights[xi];
            int weight = weight_x * weight_y;
            if (!weight) continue;
            long index = (long) py * width + px;
            int without_sample = reconstruction[index] - weight * old_value;
            numerator += (long long) weight *
                (((int) target[index] << 4) - without_sample);
        }
    }
    if (!denominator) return;

    int best_value = best_chroma_reconstruction_value(
        old_value, numerator, denominator);
    if (best_value == old_value) return;

    int change = best_value - old_value;
    stored->data[sample_index] = (unsigned char) best_value;
    for (int yi = 0; yi < y_support->count; yi++) {
        int py = y_support->first + yi;
        int weight_y = y_support->weights[yi];
        for (int xi = 0; xi < x_support->count; xi++) {
            int px = x_support->first + xi;
            int weight_x = x_support->weights[xi];
            int weight = weight_x * weight_y;
            if (weight)
                reconstruction[(long) py * width + px] += weight * change;
        }
    }
}

/* Integer coordinate descent solves the decoder-aware least-squares problem.
   The reconstruction buffer carries residual error from one sample to the
   next, so rounding loss is compensated spatially instead of being discarded
   independently in every input sample group. */
static int optimize_chroma_for_reconstruction(
    const unsigned char *target, int width, int height, Plane *stored) {
    if (!target || !stored || !stored->data || width <= 0 || height <= 0 ||
        stored->width <= 0 || stored->height <= 0) return 0;
    size_t pixel_count = (size_t) width * (size_t) height;
    if (pixel_count / (size_t) width != (size_t) height ||
        pixel_count > SIZE_MAX / sizeof(int)) return 0;
    int *reconstruction = (int *) calloc(pixel_count, sizeof(*reconstruction));
    if (!reconstruction) return 0;

    if ((size_t) stored->width > SIZE_MAX / sizeof(ChromaReconstructionSupport) ||
        (size_t) stored->height > SIZE_MAX / sizeof(ChromaReconstructionSupport)) {
        free(reconstruction);
        return 0;
    }
    ChromaReconstructionSupport *x_support = (ChromaReconstructionSupport *)
        malloc((size_t) stored->width * sizeof(*x_support));
    ChromaReconstructionSupport *y_support = (ChromaReconstructionSupport *)
        malloc((size_t) stored->height * sizeof(*y_support));
    if (!x_support || !y_support) {
        free(x_support);
        free(y_support);
        free(reconstruction);
        return 0;
    }
    for (int x = 0; x < stored->width; x++)
        prepare_reconstruction_support(x, stored->width, width,
                                       &x_support[x]);
    for (int y = 0; y < stored->height; y++)
        prepare_reconstruction_support(y, stored->height, height,
                                       &y_support[y]);

    if ((stored->width == width || stored->width == (width+1)/2) &&
        (stored->height == height || stored->height == (height+1)/2)) {
        initialize_chroma(stored,width,height,reconstruction);
    } else {
    for (int sample_y = 0; sample_y < stored->height; sample_y++) {
        const ChromaReconstructionSupport *ys = &y_support[sample_y];
        for (int sample_x = 0; sample_x < stored->width; sample_x++) {
            const ChromaReconstructionSupport *xs = &x_support[sample_x];
            int value = stored->data[
                (long) sample_y * stored->width + sample_x];
            for (int yi = 0; yi < ys->count; yi++) {
                int py = ys->first + yi;
                int weight_y = ys->weights[yi];
                for (int xi = 0; xi < xs->count; xi++) {
                    int px = xs->first + xi;
                    int weight_x = xs->weights[xi];
                    reconstruction[(long) py * width + px] +=
                        weight_x * weight_y * value;
                }
            }
        }
    }

    }

    /* Alternating scan direction reduces raster-order bias. Two complete
       forward/reverse sweeps converge to byte-level stability in practice. */
    int use_avx2 = 0;
#if defined(__x86_64__) || defined(__i386__)
    use_avx2 = n148_cpu_level() >= N148_CPU_AVX2;
#endif
    for (int sweep = 0; sweep < 2; sweep++) {
        for (int sample_y = 0; sample_y < stored->height; sample_y++)
            for (int sample_x = 0; sample_x < stored->width; sample_x++)
                refine_chroma_sample(target, width, stored,
                                     reconstruction, sample_x, sample_y,
                                     &x_support[sample_x], &y_support[sample_y],
                                     use_avx2);
        for (int sample_y = stored->height - 1; sample_y >= 0; sample_y--)
            for (int sample_x = stored->width - 1; sample_x >= 0; sample_x--)
                refine_chroma_sample(target, width, stored,
                                     reconstruction, sample_x, sample_y,
                                     &x_support[sample_x], &y_support[sample_y],
                                     use_avx2);
    }
    free(x_support);
    free(y_support);
    free(reconstruction);
    return 1;
}

int n148_split_channels_reconstruction_aware(Image *image, Plane *y, Plane *cb, Plane *cr,
                           int chroma_mode, int reconstruction_aware) {
    if (!reconstruction_aware || chroma_mode == CHROMA_444)
        return n148_split_channels_ex(image, y, cb, cr, chroma_mode, 0);
    if (!image || !image->pixels || !y || !cb || !cr ||
        image->width <= 0 || image->height <= 0) return 0;

    int width = image->width;
    int height = image->height;
    size_t pixel_count = (size_t) width * (size_t) height;
    if (pixel_count / (size_t) width != (size_t) height) return 0;
    int chroma_width, chroma_height;
    chroma_dimensions(chroma_mode, width, height,
                      &chroma_width, &chroma_height);
    *y = create_plane(width, height);
    *cb = create_plane(chroma_width, chroma_height);
    *cr = create_plane(chroma_width, chroma_height);
    unsigned char *cb_full = (unsigned char *) malloc(pixel_count);
    unsigned char *cr_full = (unsigned char *) malloc(pixel_count);
    if (!y->data || !cb->data || !cr->data || !cb_full || !cr_full) {
        free_plane(y); free_plane(cb); free_plane(cr);
        free(cb_full); free(cr_full);
        return 0;
    }

    ColorJob job;
    job.rgb = image->pixels;
    job.y = y->data;
    job.cb = cb_full;
    job.cr = cr_full;
    job.width = width;
    job.perceptual_color = 0;
    n148_parallel_for(height, color_rows, &job);
    int step_y = chroma_mode == CHROMA_420 ? 2 : 1;
    downsample_plane(cb_full, width, height, cb, 2, step_y);
    downsample_plane(cr_full, width, height, cr, 2, step_y);
    int success = optimize_chroma_for_reconstruction(
        cb_full, width, height, cb) &&
        optimize_chroma_for_reconstruction(cr_full, width, height, cr);
    free(cb_full);
    free(cr_full);
    if (!success) {
        free_plane(y); free_plane(cb); free_plane(cr);
    }
    return success;
}

int n148_split_channels_ex(Image *image, Plane *y, Plane *cb, Plane *cr,
                           int chroma_mode, int perceptual_color) {
    int width = image->width;
    int height = image->height;
    long pixel_count = (long)width * height;

    int chroma_width;
    int chroma_height;
    chroma_dimensions(chroma_mode, width, height,
                      &chroma_width, &chroma_height);
    *y = create_plane(width, height);
    *cb = create_plane(chroma_width, chroma_height);
    *cr = create_plane(chroma_width, chroma_height);
    if (!y->data || !cb->data || !cr->data) {
        free_plane(y);
        free_plane(cb);
        free_plane(cr);
        return 0;
    }
    if (perceptual_color) n148_perceptual_init();

    if (chroma_mode == CHROMA_444) {
        ColorJob job;
        job.rgb = image->pixels; job.y = y->data;
        job.cb = cb->data; job.cr = cr->data; job.width = width;
        job.perceptual_color = perceptual_color;
        n148_parallel_for(height, color_rows, &job);
        return 1;
    }

    if (chroma_mode == CHROMA_420) {
        Color420Job job;
        job.image = image; job.y = y; job.cb = cb; job.cr = cr;
        job.perceptual_color = perceptual_color;
        job.failed = 0;
#if defined(__x86_64__) || defined(__i386__)
        job.use_avx2 = (n148_cpu_level() >= N148_CPU_AVX2);
#else
        job.use_avx2 = 0;
#endif
        n148_parallel_for(chroma_height, color_420_rows, &job);
        if (job.failed) {
            free_plane(y); free_plane(cb); free_plane(cr);
            return 0;
        }
        return 1;
    }

    // 4:2:2 keeps the generic full-resolution intermediate.
    unsigned char *cb_full = (unsigned char *)malloc((size_t)pixel_count);
    unsigned char *cr_full = (unsigned char *)malloc((size_t)pixel_count);
    if (!cb_full || !cr_full) {
        free_plane(y); free_plane(cb); free_plane(cr);
        free(cb_full); free(cr_full);
        return 0;
    }
    ColorJob job;
    job.rgb = image->pixels; job.y = y->data;
    job.cb = cb_full; job.cr = cr_full; job.width = width;
    job.perceptual_color = perceptual_color;
    n148_parallel_for(height, color_rows, &job);

    int step_x = (chroma_mode == CHROMA_444) ? 1 : 2;
    int step_y = (chroma_mode == CHROMA_420) ? 2 : 1;
    downsample_plane(cb_full, width, height, cb, step_x, step_y);
    downsample_plane(cr_full, width, height, cr, step_x, step_y);

    free(cb_full);
    free(cr_full);
    return 1;
}

int split_channels(Image *image, Plane *y, Plane *cb, Plane *cr,
                   int chroma_mode) {
    return n148_split_channels_ex(image, y, cb, cr, chroma_mode, 0);
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

/* Continuous RGB SSE projection using the actual quantized, filtered
   chroma reconstruction retained by the encoder. No decoder change. */
#define STRUCTURAL_CHROMA_PROJECTION_STRENGTH 0.625
#include "luma_projection_avx2.inc"
int n148_project_luma_from_chroma(const N148RgbSource *reference,
    const Plane *source,
    const Plane *cb, const Plane *cr, Plane *corrected) {
    if (!reference || !source || !cb || !cr || !corrected ||
        !source->data || source->width <= 0 || source->height <= 0 ||
        !reference->pixels || reference->stride < (size_t)source->width * 3 ||
        reference->width != source->width ||
        reference->height != source->height || !cb->data || !cr->data ||
        cb->width != cr->width || cb->height != cr->height ||
        (cb->width != source->width &&
         cb->width != source->width / 2 + source->width % 2) ||
        (cb->height != source->height &&
         cb->height != source->height / 2 + source->height % 2))
        return 0;
    *corrected = create_plane(source->width,source->height);
    if (!corrected->data) return 0;
    double strength = n148_fidelity_balanced_reconstruction() ? 0.25 :
        STRUCTURAL_CHROMA_PROJECTION_STRENGTH;
    int sx = cb->width != source->width;
    int sy = cb->height != source->height;
    int vector = 0;
#if defined(__x86_64__) || defined(__i386__)
    vector = sx && n148_cpu_level() >= N148_CPU_AVX2;
#endif
    for (int y = 0; y < source->height; y++) {
        int y0 = sy ? y / 2 : y;
        int y1 = sy ? y0 + ((y & 1) ? 1 : -1) : y0;
        if (y1 < 0) y1 = 0;
        if (y1 >= cb->height) y1 = cb->height - 1;
        for (int x = 0; x < source->width; x++) {
#if defined(__x86_64__) || defined(__i386__)
            if (vector && x >= 2 && !(x & 1) &&
                x + 8 < source->width && x / 2 + 7 <= cb->width) {
                size_t top = (size_t)y0 * cb->width + x / 2 - 1;
                size_t other = (size_t)y1 * cb->width + x / 2 - 1;
                size_t index = (size_t)y * source->width + x;
                project_luma8_avx2(reference->pixels + (size_t)y * reference->stride + (size_t)x * 3,
                    source->data + index,cb->data + top,cb->data + other,
                    cr->data + top,cr->data + other,corrected->data + index,strength);
                x += 7;
                continue;
            }
#endif
            int x0 = sx ? x / 2 : x;
            int x1 = sx ? x0 + ((x & 1) ? 1 : -1) : x0;
            if (x1 < 0) x1 = 0;
            if (x1 >= cb->width) x1 = cb->width - 1;
            size_t a = (size_t)y0 * cb->width + x0;
            size_t b = (size_t)y0 * cb->width + x1;
            size_t c = (size_t)y1 * cb->width + x0;
            size_t d = (size_t)y1 * cb->width + x1;
            double blue = (9 * cb->data[a] + 3 * cb->data[b] +
                3 * cb->data[c] + cb->data[d]) / 16.0 - 128.0;
            double red = (9 * cr->data[a] + 3 * cr->data[b] +
                3 * cr->data[c] + cr->data[d]) / 16.0 - 128.0;
            size_t index = (size_t)y * source->width + x;
            const unsigned char *rgb = reference->pixels + (size_t)y * reference->stride + (size_t)x * 3;
            double target = (rgb[0] + rgb[1] + rgb[2] -
                1.427864 * blue - 0.687864 * red) / 3.0;
            target = source->data[index] + strength *
                (target - source->data[index]);
            if (target < 0.0) target = 0.0;
            if (target > 255.0) target = 255.0;
            corrected->data[index] = (unsigned char)(target + 0.5);
        }
    }
    return 1;
}

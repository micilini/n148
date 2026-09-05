#include <stdio.h>
#include <stdlib.h>

#include "header.h"
#include <string.h>
#include "ppm.h"
#include "cpu.h"
#include "parallel.h"

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

__attribute__((target("avx2")))
static void rgb_to_ycbcr_avx2(const unsigned char *rgb, long count,
                              unsigned char *y, unsigned char *cb,
                              unsigned char *cr) {
    // Pixels arrive interleaved as R G B R G B ... A byte shuffle
    // pulls the three channels apart without touching memory: each
    // 128 bit lane holds four pixels, so one 256 bit register covers
    // eight pixels per iteration.
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
    const __m256i cbr = _mm256_set1_epi32(YCC_CBR);
    const __m256i cbg = _mm256_set1_epi32(YCC_CBG);
    const __m256i crr = _mm256_set1_epi32(YCC_CRR);
    const __m256i crg = _mm256_set1_epi32(YCC_CRG);
    const __m256i half   = _mm256_set1_epi32(YCC_HALF);
    const __m256i offset = _mm256_set1_epi32(YCC_OFFSET);

    long i = 0;
    // Stop early: the high lane reads twelve bytes ahead, so the tail
    // is finished by the scalar routine to stay inside the buffer.
    for (; i + 12 <= count; i += 8) {
        const unsigned char *p = rgb + i * 3;
        __m128i lo = _mm_loadu_si128((const __m128i *)(p));
        __m128i hi = _mm_loadu_si128((const __m128i *)(p + 12));
        __m256i packed = _mm256_set_m128i(hi, lo);

        __m256i r = _mm256_shuffle_epi8(packed, shuffle_r);
        __m256i g = _mm256_shuffle_epi8(packed, shuffle_g);
        __m256i b = _mm256_shuffle_epi8(packed, shuffle_b);
        __m256i rb = _mm256_sub_epi32(r, b);
        __m256i gb = _mm256_sub_epi32(g, b);

        // Every coefficient triplet sums to either 1.0 (Y) or zero
        // (chroma).  Factoring B out removes one 32-bit multiply per
        // channel while remaining exactly equivalent in integer arithmetic.
        __m256i luma = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_mullo_epi32(rb, yr),
                             _mm256_mullo_epi32(gb, yg)),
            _mm256_add_epi32(_mm256_slli_epi32(b, YCC_FIX_BITS), half));
        __m256i blue_diff = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_mullo_epi32(rb, cbr),
                             _mm256_mullo_epi32(gb, cbg)), offset);
        __m256i red_diff = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_mullo_epi32(rb, crr),
                             _mm256_mullo_epi32(gb, crg)), offset);

        luma      = _mm256_srai_epi32(luma,      YCC_FIX_BITS);
        blue_diff = _mm256_srai_epi32(blue_diff, YCC_FIX_BITS);
        red_diff  = _mm256_srai_epi32(red_diff,  YCC_FIX_BITS);

        // Packing with unsigned saturation performs the 0..255 clamp
        // for free, so no comparison is needed on the way out.
        __m128i y16 = _mm_packs_epi32(_mm256_castsi256_si128(luma),
                                      _mm256_extracti128_si256(luma, 1));
        __m128i b16 = _mm_packs_epi32(_mm256_castsi256_si128(blue_diff),
                                      _mm256_extracti128_si256(blue_diff, 1));
        __m128i r16 = _mm_packs_epi32(_mm256_castsi256_si128(red_diff),
                                      _mm256_extracti128_si256(red_diff, 1));

        _mm_storel_epi64((__m128i *)(y  + i), _mm_packus_epi16(y16, y16));
        _mm_storel_epi64((__m128i *)(cb + i), _mm_packus_epi16(b16, b16));
        _mm_storel_epi64((__m128i *)(cr + i), _mm_packus_epi16(r16, r16));
    }

    if (i < count)
        rgb_to_ycbcr_scalar(rgb + i*3, count - i, y + i, cb + i, cr + i);
}
#endif

typedef struct {
    const unsigned char *rgb;
    unsigned char *y, *cb, *cr;
    int width;
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

static void color_rows(long start, long end, int worker, void *context) {
    (void) worker;
    ColorJob *job = (ColorJob *) context;
    for (long row = start; row < end; row++) {
        long offset = row * job->width;
        rgb_to_ycbcr(job->rgb + offset * 3, job->width,
                     job->y + offset, job->cb + offset, job->cr + offset);
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
#endif

typedef struct {
    const Image *image;
    Plane *y, *cb, *cr;
    int use_avx2;
    int failed;
} Color420Job;

static void color_420_rows(long start, long end, int worker, void *context) {
    (void)worker;
    Color420Job *job = (Color420Job *)context;
    int width = job->image->width;
    int height = job->image->height;

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
        rgb_to_ycbcr(rgb0, width, job->y->data + (long)row0 * width, cb0, cr0);
        if (row1 < height) {
            const unsigned char *rgb1 = job->image->pixels + (long)row1 * width * 3;
            rgb_to_ycbcr(rgb1, width, job->y->data + (long)row1 * width, cb1, cr1);
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

int split_channels(Image *image, Plane *y, Plane *cb, Plane *cr,
                   int chroma_mode) {
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

    if (chroma_mode == CHROMA_444) {
        ColorJob job;
        job.rgb = image->pixels; job.y = y->data;
        job.cb = cb->data; job.cr = cr->data; job.width = width;
        n148_parallel_for(height, color_rows, &job);
        return 1;
    }

    if (chroma_mode == CHROMA_420) {
        Color420Job job;
        job.image = image; job.y = y; job.cb = cb; job.cr = cr;
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
    n148_parallel_for(height, color_rows, &job);

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

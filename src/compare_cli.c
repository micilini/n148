/*
 * In-memory N.148i vs libjpeg-turbo benchmark.
 *
 * Both codecs receive the same already-loaded RGB image.  Timings include
 * colour conversion, transform, entropy coding/decoding, allocation and
 * reconstruction, but exclude disk I/O and PSNR calculation.
 */
#define _GNU_SOURCE
#include <math.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jpeglib.h>

#include "decoder.h"
#include "encoder.h"
#include "header.h"
#include "ppm.h"
#include "parallel.h"

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} JpegError;

static void jpeg_error_exit(j_common_ptr common) {
    JpegError *error = (JpegError *) common->err;
    longjmp(error->jump, 1);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *) left;
    double b = *(const double *) right;
    return (a > b) - (a < b);
}

static double median(double *values, int count) {
    qsort(values, (size_t) count, sizeof(*values), compare_double);
    if (count & 1) return values[count / 2];
    return (values[count / 2 - 1] + values[count / 2]) * 0.5;
}

#if defined(__linux__)
typedef cpu_set_t CpuAffinity;

static int capture_affinity(CpuAffinity *mask) {
    return sched_getaffinity(0, sizeof(*mask), mask) == 0;
}

static int select_affinity_cpu(const CpuAffinity *allowed, int cpu,
                               CpuAffinity *single) {
    if (cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, allowed)) return 0;
    CPU_ZERO(single);
    CPU_SET(cpu, single);
    return 1;
}

static int apply_affinity(const CpuAffinity *mask) {
    return sched_setaffinity(0, sizeof(*mask), mask) == 0;
}
#else
typedef int CpuAffinity;

static int capture_affinity(CpuAffinity *mask) {
    *mask = 0;
    return 1;
}

static int select_affinity_cpu(const CpuAffinity *allowed, int cpu,
                               CpuAffinity *single) {
    (void) allowed;
    (void) cpu;
    (void) single;
    return 0;
}

static int apply_affinity(const CpuAffinity *mask) {
    (void) mask;
    return 1;
}
#endif

static double image_psnr(const Image *original, const unsigned char *decoded) {
    long count = (long) original->width * original->height * 3;
    double error = 0.0;
    for (long i = 0; i < count; i++) {
        double difference = (double) original->pixels[i] - decoded[i];
        error += difference * difference;
    }
    error /= count;
    return error == 0.0 ? 999.0 : 10.0 * log10(255.0 * 255.0 / error);
}

static int n148_encode(const Image *image, int quality, int chroma,
                       unsigned char **data, long *size,
                       HuffSpec specs[HUFFMAN_TABLE_COUNT]) {
    Plane y = {0}, cb = {0}, cr = {0};
    EncodeStats stats;
    if (!split_channels((Image *) image, &y, &cb, &cr, chroma)) return 0;
    int ok = encode_image(&y, &cb, &cr, quality, 1, specs, data, &stats);
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    if (!ok) return 0;
    *size = 21 + stats.table_size + stats.data_size;
    return 1;
}

static int n148_decode(const unsigned char *data, long payload_size,
                       int width, int height, int quality, int chroma,
                       const HuffSpec specs[HUFFMAN_TABLE_COUNT], Image *output,
                       double *plane_ms, double *merge_ms) {
    Plane y = {0}, cb = {0}, cr = {0};
    DecodeStats stats;
    double start = now_ms();
    int ok = decode_image((unsigned char *) data, payload_size, width, height,
                          quality, chroma, specs, &y, &cb, &cr, &stats);
    double split = now_ms();
    if (ok) ok = merge_channels(&y, &cb, &cr, 1, output);
    double merged = now_ms();
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    if (plane_ms) *plane_ms = split - start;
    if (merge_ms) *merge_ms = merged - split;
    return ok;
}

static void jpeg_sampling(struct jpeg_compress_struct *jpeg, int chroma) {
    jpeg->comp_info[0].h_samp_factor = chroma == CHROMA_444 ? 1 : 2;
    jpeg->comp_info[0].v_samp_factor = chroma == CHROMA_420 ? 2 : 1;
    jpeg->comp_info[1].h_samp_factor = 1;
    jpeg->comp_info[1].v_samp_factor = 1;
    jpeg->comp_info[2].h_samp_factor = 1;
    jpeg->comp_info[2].v_samp_factor = 1;
}

static int jpeg_encode(const Image *image, int quality, int chroma,
                       unsigned char **data, unsigned long *size) {
    struct jpeg_compress_struct jpeg;
    JpegError error;
    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&jpeg);
        free(*data);
        *data = NULL;
        return 0;
    }

    jpeg_create_compress(&jpeg);
    jpeg_mem_dest(&jpeg, data, size);
    jpeg.image_width = (JDIMENSION) image->width;
    jpeg.image_height = (JDIMENSION) image->height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, quality, TRUE);
    jpeg_sampling(&jpeg, chroma);
    jpeg.optimize_coding = TRUE;

    jpeg_start_compress(&jpeg, TRUE);
    while (jpeg.next_scanline < jpeg.image_height) {
        JSAMPROW row = image->pixels +
            (long) jpeg.next_scanline * image->width * 3;
        jpeg_write_scanlines(&jpeg, &row, 1);
    }
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    return 1;
}

static int jpeg_decode(const unsigned char *data, unsigned long size,
                       unsigned char **pixels, int *width, int *height) {
    struct jpeg_decompress_struct jpeg;
    JpegError error;
    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&jpeg);
        free(*pixels);
        *pixels = NULL;
        return 0;
    }

    jpeg_create_decompress(&jpeg);
    jpeg_mem_src(&jpeg, data, size);
    jpeg_read_header(&jpeg, TRUE);
    jpeg.out_color_space = JCS_RGB;
    jpeg_start_decompress(&jpeg);

    *width = (int) jpeg.output_width;
    *height = (int) jpeg.output_height;
    size_t stride = (size_t) *width * jpeg.output_components;
    *pixels = (unsigned char *) malloc(stride * (size_t) *height);
    if (!*pixels) {
        jpeg_destroy_decompress(&jpeg);
        return 0;
    }

    while (jpeg.output_scanline < jpeg.output_height) {
        JSAMPROW row = *pixels + (size_t) jpeg.output_scanline * stride;
        jpeg_read_scanlines(&jpeg, &row, 1);
    }
    jpeg_finish_decompress(&jpeg);
    jpeg_destroy_decompress(&jpeg);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s image.ppm quality [reps] [chroma] [threads] "
                "[jpeg_cpu]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[1];
    int quality = atoi(argv[2]);
    int reps = argc > 3 ? atoi(argv[3]) : 30;
    int chroma = argc > 4 ? atoi(argv[4]) : CHROMA_420;
    int jpeg_cpu = argc > 6 ? atoi(argv[6]) : -1;
    if (argc > 5) n148_set_thread_count(atoi(argv[5]));
    if (quality < 1 || quality > 100 || reps < 1 ||
        chroma < 0 || chroma > 2 || jpeg_cpu < -1) {
        fprintf(stderr, "invalid quality, repetition count or chroma mode\n");
        return EXIT_FAILURE;
    }

    Image original = {0};
    if (!load_ppm(path, &original)) return EXIT_FAILURE;

    CpuAffinity n148_affinity, jpeg_affinity;
    int pin_jpeg = jpeg_cpu >= 0;
    if (pin_jpeg &&
        (!capture_affinity(&n148_affinity) ||
         !select_affinity_cpu(&n148_affinity, jpeg_cpu, &jpeg_affinity))) {
        fprintf(stderr, "jpeg_cpu is unavailable or affinity control failed\n");
        return EXIT_FAILURE;
    }

    double *n148_enc = (double *) malloc((size_t) reps * sizeof(double));
    double *n148_dec = (double *) malloc((size_t) reps * sizeof(double));
    double *n148_planes = (double *) malloc((size_t) reps * sizeof(double));
    double *n148_merge = (double *) malloc((size_t) reps * sizeof(double));
    double *jpeg_enc = (double *) malloc((size_t) reps * sizeof(double));
    double *jpeg_dec = (double *) malloc((size_t) reps * sizeof(double));
    if (!n148_enc || !n148_dec || !n148_planes || !n148_merge ||
        !jpeg_enc || !jpeg_dec) return EXIT_FAILURE;

    unsigned char *n148_data = NULL, *jpeg_data = NULL;
    long n148_file_size = 0;
    unsigned long jpeg_size = 0;
    HuffSpec specs[HUFFMAN_TABLE_COUNT];
    Image n148_image = {0};
    unsigned char *jpeg_pixels = NULL;
    int jpeg_width = 0, jpeg_height = 0;
    const int warmups = reps < 5 ? 1 : 5;

    /* Alternate which codec runs first on every trial.  This keeps turbo
       frequency, cache temperature and slow thermal drift from favouring
       whichever implementation happened to be measured first. */
    for (int i = -warmups; i < reps; i++) {
        int n148_first = ((i + warmups) & 1) == 0;
        for (int pass = 0; pass < 2; pass++) {
            if ((pass == 0) == n148_first) {
                unsigned char *data = NULL;
                long size = 0;
                HuffSpec trial_specs[HUFFMAN_TABLE_COUNT];
                if (pin_jpeg && !apply_affinity(&n148_affinity)) {
                    fprintf(stderr, "could not restore N.148i CPU affinity\n");
                    return EXIT_FAILURE;
                }
                double start = now_ms();
                int ok = n148_encode(&original, quality, chroma, &data, &size,
                                     trial_specs);
                double elapsed = now_ms() - start;
                if (!ok) return EXIT_FAILURE;
                if (i >= 0) n148_enc[i] = elapsed;
                free(n148_data);
                n148_data = data;
                n148_file_size = size;
                memcpy(specs, trial_specs, sizeof(specs));
            } else {
                unsigned char *data = NULL;
                unsigned long size = 0;
                if (pin_jpeg && !apply_affinity(&jpeg_affinity)) {
                    fprintf(stderr, "could not pin JPEG CPU affinity\n");
                    return EXIT_FAILURE;
                }
                double start = now_ms();
                int ok = jpeg_encode(&original, quality, chroma, &data, &size);
                double elapsed = now_ms() - start;
                if (!ok) return EXIT_FAILURE;
                if (i >= 0) jpeg_enc[i] = elapsed;
                free(jpeg_data);
                jpeg_data = data;
                jpeg_size = size;
            }
        }
    }

    /* encode_image returns entropy bytes separately from the complete file. */
    long n148_payload_size = n148_file_size - 21 - huffman_tables_size(specs);
    for (int i = -warmups; i < reps; i++) {
        int n148_first = ((i + warmups) & 1) == 0;
        for (int pass = 0; pass < 2; pass++) {
            if ((pass == 0) == n148_first) {
                Image decoded = {0};
                double plane_ms, merge_ms;
                free_image(&n148_image);
                if (pin_jpeg && !apply_affinity(&n148_affinity)) {
                    fprintf(stderr, "could not restore N.148i CPU affinity\n");
                    return EXIT_FAILURE;
                }
                double start = now_ms();
                int ok = n148_decode(n148_data, n148_payload_size,
                                     original.width, original.height, quality,
                                     chroma, specs, &decoded,
                                     &plane_ms, &merge_ms);
                double elapsed = now_ms() - start;
                if (!ok) return EXIT_FAILURE;
                if (i >= 0) {
                    n148_dec[i] = elapsed;
                    n148_planes[i] = plane_ms;
                    n148_merge[i] = merge_ms;
                }
                n148_image = decoded;
            } else {
                unsigned char *pixels = NULL;
                int width = 0, height = 0;
                free(jpeg_pixels);
                jpeg_pixels = NULL;
                if (pin_jpeg && !apply_affinity(&jpeg_affinity)) {
                    fprintf(stderr, "could not pin JPEG CPU affinity\n");
                    return EXIT_FAILURE;
                }
                double start = now_ms();
                int ok = jpeg_decode(jpeg_data, jpeg_size, &pixels,
                                     &width, &height);
                double elapsed = now_ms() - start;
                if (!ok || width != original.width || height != original.height)
                    return EXIT_FAILURE;
                if (i >= 0) jpeg_dec[i] = elapsed;
                jpeg_pixels = pixels;
                jpeg_width = width;
                jpeg_height = height;
            }
        }
    }

    if (pin_jpeg && !apply_affinity(&n148_affinity)) {
        fprintf(stderr, "could not restore process CPU affinity\n");
        return EXIT_FAILURE;
    }

    double ne = median(n148_enc, reps), nd = median(n148_dec, reps);
    double np = median(n148_planes, reps), nm = median(n148_merge, reps);
    double je = median(jpeg_enc, reps), jd = median(jpeg_dec, reps);
    double npsnr = image_psnr(&original, n148_image.pixels);
    double jpsnr = image_psnr(&original, jpeg_pixels);

    printf("%s %dx%d q=%d chroma=%d threads=%d reps=%d\n",
           path, original.width, original.height, quality, chroma,
           n148_thread_count(), reps);
    printf("N148i  enc %8.3f ms  dec %8.3f ms  %8ld B  PSNR %7.3f dB\n",
           ne, nd, n148_file_size, npsnr);
    printf("JPEG   enc %8.3f ms  dec %8.3f ms  %8lu B  PSNR %7.3f dB\n",
           je, jd, jpeg_size, jpsnr);
    printf("N/J    enc %8.3fx    dec %8.3fx    %8.3fx size\n",
           ne / je, nd / jd, (double) n148_file_size / jpeg_size);
    printf("N148i decode stages: planes %8.3f ms  merge %8.3f ms  other %8.3f ms\n",
           np, nm, nd - np - nm);

    (void) jpeg_width;
    (void) jpeg_height;
    free(n148_enc); free(n148_dec); free(n148_planes); free(n148_merge);
    free(jpeg_enc); free(jpeg_dec);
    free(n148_data); free(jpeg_data); free(jpeg_pixels);
    free_image(&n148_image);
    free_image(&original);
    return EXIT_SUCCESS;
}

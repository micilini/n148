/*
 * In-memory final benchmark driver for N.148i, libjpeg-turbo, JPEG XL,
 * and WebP.
 *
 * Copyright (c) Micilini Roll. Licensed under the MIT License.
 *
 * This file is measurement-only instrumentation.  It deliberately calls the
 * codec libraries directly, keeps disk I/O outside timed regions, uses
 * persistent worker pools, alternates codec order, and emits every timing
 * sample so the statistical analysis is independently reproducible.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <jpeglib.h>
#ifndef N148_BENCH_NO_JXL
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>
#endif
#include <webp/decode.h>
#include <webp/encode.h>
#include <limits.h>
#include <math.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "n148i.h"
#include "ppm.h"

/* Compile this same timing harness against the frozen public V1 library.
   Its options structure ends at thread_count. No V2 compatibility path is
   used for the V1 benchmark. */
#ifdef N148_BENCH_V1
#define N148I_FORMAT_VERSION_1 1
#define N148I_DEFAULT_FEATURES 0u
#endif

enum {
    CODEC_N148_CURRENT = 0,
    CODEC_N148_FORMAT_1 = 1,
    CODEC_JPEG = 2,
    CODEC_WEBP_M4 = 3,
    CODEC_WEBP_M6 = 4,
    CODEC_JXL_E3 = 5,
    CODEC_JXL_E7 = 6,
    CODEC_COUNT = 7
};

static const char *const CODEC_NAMES[CODEC_COUNT] = {
    "n148i", "n148i_format_1", "jpeg", "webp_m4", "webp_m6",
    "jxl_e3", "jxl_e7"
};

#define ALL_CODEC_MASK ((1u << CODEC_COUNT) - 1u)

typedef struct {
    const char *input;
    const char *output_prefix;
    int n148_quality;
    int n148_chroma_quality;
    int n148_effort;
    uint32_t n148_feature_flags;
    int jpeg_quality;
    float webp_quality;
    float jxl_distance;
    int reps;
    int warmups;
    int chroma;
    int n148_threads;
    int jxl_threads;
    int webp_thread_level;
    int jpeg_cpu;
    double sample_ms;
    unsigned active_mask;
} Options;

typedef struct {
    unsigned char *compressed;
    size_t compressed_size;
    unsigned char *decoded;
    int width;
    int height;
    int encode_inner;
    int decode_inner;
} CodecState;

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} JpegError;

#if defined(__linux__)
typedef cpu_set_t CpuAffinity;

static int capture_affinity(CpuAffinity *mask) {
    return sched_getaffinity(0, sizeof(*mask), mask) == 0;
}

static int single_cpu_affinity(const CpuAffinity *allowed, int cpu,
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

static int single_cpu_affinity(const CpuAffinity *allowed, int cpu,
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

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void jpeg_error_exit(j_common_ptr common) {
    JpegError *error = (JpegError *) common->err;
    longjmp(error->jump, 1);
}

static int is_public_n148(int codec) {
    return codec == CODEC_N148_CURRENT || codec == CODEC_N148_FORMAT_1;
}

static void free_state(int codec, CodecState *state) {
    if (is_public_n148(codec)) {
        n148i_free_buffer(state->compressed);
        n148i_free_buffer(state->decoded);
    } else {
        free(state->compressed);
        free(state->decoded);
    }
    memset(state, 0, sizeof(*state));
}

static int n148_encode_once(const Image *image, const Options *options,
                            uint32_t format_version, CodecState *state) {
    n148i_image_t source = {
        image->pixels,
        (uint32_t) image->width,
        (uint32_t) image->height,
        (size_t) image->width * 3u,
    };
    n148i_encode_options_t settings;
    n148i_encode_options_init(&settings);
    settings.quality = options->n148_quality;
    settings.chroma = (n148i_chroma_t) options->chroma;
    settings.optimize_huffman = 1;
    settings.thread_count = options->n148_threads;
#ifndef N148_BENCH_V1
    settings.format_version = format_version;
    settings.feature_flags = format_version == N148I_FORMAT_VERSION_1 ?
        0u : options->n148_feature_flags;
    settings.effort = format_version == N148I_FORMAT_VERSION_1 ?
        0 : options->n148_effort;
    settings.chroma_quality = format_version == N148I_FORMAT_VERSION_1 ?
        0 : options->n148_chroma_quality;
#else
    (void)format_version;
#endif

    uint8_t *compressed = NULL;
    size_t compressed_size = 0;
    if (n148i_encode_memory(&source, &settings, &compressed,
                            &compressed_size) != N148I_OK) {
        n148i_free_buffer(compressed);
        return 0;
    }
    n148i_free_buffer(state->compressed);
    state->compressed = compressed;
    state->compressed_size = compressed_size;
    return 1;
}

static int n148_decode_once(CodecState *state) {
    n148i_image_t decoded = {0};
    if (n148i_decode_memory(state->compressed, state->compressed_size,
                            &decoded) != N148I_OK ||
        decoded.width != (uint32_t) state->width ||
        decoded.height != (uint32_t) state->height || !decoded.pixels) {
        n148i_free_image(&decoded);
        return 0;
    }
    n148i_free_buffer(state->decoded);
    state->decoded = decoded.pixels;
    decoded.pixels = NULL;
    return 1;
}

static void jpeg_sampling(struct jpeg_compress_struct *jpeg, int chroma) {
    jpeg->comp_info[0].h_samp_factor = chroma == N148I_CHROMA_444 ? 1 : 2;
    jpeg->comp_info[0].v_samp_factor = chroma == N148I_CHROMA_420 ? 2 : 1;
    jpeg->comp_info[1].h_samp_factor = 1;
    jpeg->comp_info[1].v_samp_factor = 1;
    jpeg->comp_info[2].h_samp_factor = 1;
    jpeg->comp_info[2].v_samp_factor = 1;
}

static int jpeg_encode_once(const Image *image, const Options *options,
                            CodecState *state) {
    struct jpeg_compress_struct jpeg;
    JpegError error;
    unsigned char *data = NULL;
    unsigned long size = 0;

    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&jpeg);
        free(data);
        return 0;
    }
    jpeg_create_compress(&jpeg);
    jpeg_mem_dest(&jpeg, &data, &size);
    jpeg.image_width = (JDIMENSION) image->width;
    jpeg.image_height = (JDIMENSION) image->height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, options->jpeg_quality, TRUE);
    jpeg_sampling(&jpeg, options->chroma);
    jpeg.optimize_coding = TRUE;
    jpeg_start_compress(&jpeg, TRUE);
    while (jpeg.next_scanline < jpeg.image_height) {
        JSAMPROW row = image->pixels +
            (size_t) jpeg.next_scanline * image->width * 3;
        jpeg_write_scanlines(&jpeg, &row, 1);
    }
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);

    free(state->compressed);
    state->compressed = data;
    state->compressed_size = (size_t) size;
    return 1;
}

static int jpeg_decode_once(CodecState *state) {
    struct jpeg_decompress_struct jpeg;
    JpegError error;
    unsigned char *pixels = NULL;
    int width = 0, height = 0;

    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&jpeg);
        free(pixels);
        return 0;
    }
    jpeg_create_decompress(&jpeg);
    jpeg_mem_src(&jpeg, state->compressed,
                 (unsigned long) state->compressed_size);
    jpeg_read_header(&jpeg, TRUE);
    jpeg.out_color_space = JCS_RGB;
    jpeg_start_decompress(&jpeg);
    width = (int) jpeg.output_width;
    height = (int) jpeg.output_height;
    size_t stride = (size_t) width * jpeg.output_components;
    pixels = (unsigned char *) malloc(stride * (size_t) height);
    if (!pixels) {
        jpeg_destroy_decompress(&jpeg);
        return 0;
    }
    while (jpeg.output_scanline < jpeg.output_height) {
        JSAMPROW row = pixels + (size_t) jpeg.output_scanline * stride;
        jpeg_read_scanlines(&jpeg, &row, 1);
    }
    jpeg_finish_decompress(&jpeg);
    jpeg_destroy_decompress(&jpeg);
    if (width != state->width || height != state->height) {
        free(pixels);
        return 0;
    }
    free(state->decoded);
    state->decoded = pixels;
    return 1;
}

#ifndef N148_BENCH_NO_JXL
static int jxl_encode_once(const Image *image, const Options *options,
                           int effort, void *runner, CodecState *state) {
    JxlEncoder *encoder = JxlEncoderCreate(NULL);
    JxlEncoderFrameSettings *frame = NULL;
    unsigned char *compressed = NULL;
    size_t capacity = 65536;
    size_t used = 0;
    int ok = 0;
    if (!encoder) goto cleanup;
    if (JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner) !=
        JXL_ENC_SUCCESS) goto cleanup;

    JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = (uint32_t) image->width;
    info.ysize = (uint32_t) image->height;
    info.bits_per_sample = 8;
    info.exponent_bits_per_sample = 0;
    info.num_color_channels = 3;
    info.num_extra_channels = 0;
    info.alpha_bits = 0;
    info.uses_original_profile = JXL_FALSE;
    if (JxlEncoderSetBasicInfo(encoder, &info) != JXL_ENC_SUCCESS)
        goto cleanup;

    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(encoder, &color) != JXL_ENC_SUCCESS)
        goto cleanup;
    frame = JxlEncoderFrameSettingsCreate(encoder, NULL);
    if (!frame ||
        JxlEncoderFrameSettingsSetOption(
            frame, JXL_ENC_FRAME_SETTING_EFFORT, effort) != JXL_ENC_SUCCESS ||
        JxlEncoderSetFrameDistance(frame, options->jxl_distance) !=
            JXL_ENC_SUCCESS ||
        JxlEncoderAddImageFrame(frame, &format, image->pixels,
            (size_t) image->width * image->height * 3) != JXL_ENC_SUCCESS)
        goto cleanup;
    JxlEncoderCloseInput(encoder);

    compressed = (unsigned char *) malloc(capacity);
    if (!compressed) goto cleanup;
    for (;;) {
        unsigned char *next = compressed + used;
        size_t available = capacity - used;
        JxlEncoderStatus status =
            JxlEncoderProcessOutput(encoder, &next, &available);
        used = (size_t) (next - compressed);
        if (status == JXL_ENC_SUCCESS) break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT) goto cleanup;
        if (capacity > SIZE_MAX / 2) goto cleanup;
        capacity *= 2;
        unsigned char *grown =
            (unsigned char *) realloc(compressed, capacity);
        if (!grown) goto cleanup;
        compressed = grown;
    }

    free(state->compressed);
    state->compressed = compressed;
    compressed = NULL;
    state->compressed_size = used;
    ok = 1;

cleanup:
    free(compressed);
    JxlEncoderDestroy(encoder);
    return ok;
}

static int jxl_decode_once(void *runner, CodecState *state) {
    JxlDecoder *decoder = JxlDecoderCreate(NULL);
    unsigned char *pixels = NULL;
    int got_info = 0;
    int got_image = 0;
    int ok = 0;
    JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (!decoder) goto cleanup;
    if (JxlDecoderSetParallelRunner(decoder, JxlThreadParallelRunner, runner) !=
            JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(decoder,
            JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS)
        goto cleanup;
    JxlDecoderSetInput(decoder, state->compressed, state->compressed_size);
    JxlDecoderCloseInput(decoder);

    for (;;) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
        if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS ||
                (int) info.xsize != state->width ||
                (int) info.ysize != state->height)
                goto cleanup;
            got_info = 1;
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t bytes = 0;
            if (!got_info ||
                JxlDecoderImageOutBufferSize(decoder, &format, &bytes) !=
                    JXL_DEC_SUCCESS ||
                bytes != (size_t) state->width * state->height * 3)
                goto cleanup;
            pixels = (unsigned char *) malloc(bytes);
            if (!pixels ||
                JxlDecoderSetImageOutBuffer(decoder, &format, pixels, bytes) !=
                    JXL_DEC_SUCCESS)
                goto cleanup;
        } else if (status == JXL_DEC_FULL_IMAGE) {
            got_image = 1;
        } else if (status == JXL_DEC_SUCCESS) {
            ok = got_info && got_image && pixels != NULL;
            break;
        } else if (status == JXL_DEC_NEED_MORE_INPUT ||
                   status == JXL_DEC_ERROR) {
            goto cleanup;
        }
    }

    if (ok) {
        free(state->decoded);
        state->decoded = pixels;
        pixels = NULL;
    }

cleanup:
    free(pixels);
    JxlDecoderDestroy(decoder);
    return ok;
}
#endif

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} WebPOutput;

static int webp_write(const uint8_t *data, size_t data_size,
                      const WebPPicture *picture) {
    WebPOutput *output = (WebPOutput *) picture->custom_ptr;
    if (!output || data_size > SIZE_MAX - output->size) return 0;
    size_t required = output->size + data_size;
    if (required > output->capacity) {
        size_t capacity = output->capacity ? output->capacity : 65536u;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2u) {
                capacity = required;
                break;
            }
            capacity *= 2u;
        }
        uint8_t *grown = (uint8_t *) realloc(output->data, capacity);
        if (!grown) return 0;
        output->data = grown;
        output->capacity = capacity;
    }
    memcpy(output->data + output->size, data, data_size);
    output->size = required;
    return 1;
}

static int webp_encode_once(const Image *image, const Options *options,
                            int method, CodecState *state) {
    WebPConfig config;
    WebPPicture picture;
    WebPOutput output = {0};
    int ok = 0;
    if (!WebPConfigInit(&config) || !WebPPictureInit(&picture)) return 0;
    config.lossless = 0;
    config.quality = options->webp_quality;
    config.method = method;
    config.thread_level = options->webp_thread_level;
    if (!WebPValidateConfig(&config)) goto cleanup;

    /* Lossy WebP's native input is YUV420. Leaving use_argb disabled makes
       WebPPictureImportRGB perform the documented RGB-to-YUV conversion. */
    picture.use_argb = 0;
    picture.width = image->width;
    picture.height = image->height;
    picture.writer = webp_write;
    picture.custom_ptr = &output;
    if (!WebPPictureImportRGB(&picture, image->pixels, image->width * 3) ||
        !WebPEncode(&config, &picture) || output.size == 0) goto cleanup;

    free(state->compressed);
    state->compressed = output.data;
    output.data = NULL;
    state->compressed_size = output.size;
    ok = 1;

cleanup:
    WebPPictureFree(&picture);
    free(output.data);
    return ok;
}

static int webp_decode_once(CodecState *state) {
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(state->compressed, state->compressed_size,
                     &width, &height) ||
        width != state->width || height != state->height ||
        (size_t) width > SIZE_MAX / 3u / (size_t) height) return 0;
    size_t stride = (size_t) width * 3u;
    size_t bytes = stride * (size_t) height;
    uint8_t *pixels = (uint8_t *) malloc(bytes);
    if (!pixels || !WebPDecodeRGBInto(
            state->compressed, state->compressed_size, pixels, bytes,
            (int) stride)) {
        free(pixels);
        return 0;
    }
    free(state->decoded);
    state->decoded = pixels;
    return 1;
}

static int encode_once(int codec, const Image *image, const Options *options,
                       void *jxl_runner, CodecState states[CODEC_COUNT]) {
    if (codec == CODEC_N148_CURRENT || codec == CODEC_N148_FORMAT_1)
        return n148_encode_once(
            image, options,
            codec == CODEC_N148_CURRENT ? N148I_FORMAT_VERSION :
                                          N148I_FORMAT_VERSION_1,
            &states[codec]);
    if (codec == CODEC_JPEG)
        return jpeg_encode_once(image, options, &states[codec]);
    if (codec == CODEC_JXL_E3 || codec == CODEC_JXL_E7) {
#ifdef N148_BENCH_NO_JXL
        return 0;
#else
        return jxl_encode_once(image, options,
                               codec == CODEC_JXL_E3 ? 3 : 7,
                               jxl_runner, &states[codec]);
#endif
    }
    return webp_encode_once(image, options,
                            codec == CODEC_WEBP_M4 ? 4 : 6,
                            &states[codec]);
}

static int decode_once(int codec, const Options *options, void *jxl_runner,
                       CodecState states[CODEC_COUNT]) {
    (void) options;
    if (codec == CODEC_N148_CURRENT || codec == CODEC_N148_FORMAT_1)
        return n148_decode_once(&states[codec]);
    if (codec == CODEC_JPEG)
        return jpeg_decode_once(&states[codec]);
    if (codec == CODEC_JXL_E3 || codec == CODEC_JXL_E7) {
#ifdef N148_BENCH_NO_JXL
        return 0;
#else
        return jxl_decode_once(jxl_runner, &states[codec]);
#endif
    }
    return webp_decode_once(&states[codec]);
}

static int codec_affinity(int codec, const CpuAffinity *all,
                          const CpuAffinity *jpeg, int pin_jpeg) {
    return apply_affinity(codec == CODEC_JPEG && pin_jpeg ? jpeg : all);
}

static int calibrated_inner(double elapsed, double target_ms) {
    if (!(elapsed > 0.0) || !isfinite(elapsed)) return 1;
    int inner = (int) ceil(target_ms / elapsed);
    if (inner < 1) inner = 1;
    if (inner > 256) inner = 256;
    return inner;
}

static int measure_encode(int codec, int inner, const Image *image,
                          const Options *options, void *jxl_runner,
                          CodecState states[CODEC_COUNT], double *elapsed) {
    double start = now_ms();
    for (int i = 0; i < inner; i++) {
        if (!encode_once(codec, image, options, jxl_runner, states)) return 0;
    }
    *elapsed = (now_ms() - start) / inner;
    return 1;
}

static int measure_decode(int codec, int inner, const Options *options,
                          void *jxl_runner, CodecState states[CODEC_COUNT],
                          double *elapsed) {
    double start = now_ms();
    for (int i = 0; i < inner; i++) {
        if (!decode_once(codec, options, jxl_runner, states)) return 0;
    }
    *elapsed = (now_ms() - start) / inner;
    return 1;
}

static int save_reconstruction(const char *prefix, const char *codec,
                               const CodecState *state) {
    if (!prefix) return 1;
    size_t length = strlen(prefix) + strlen(codec) + 6;
    char *path = (char *) malloc(length);
    if (!path) return 0;
    snprintf(path, length, "%s-%s.ppm", prefix, codec);
    Image output;
    output.width = state->width;
    output.height = state->height;
    output.pixels = state->decoded;
    int ok = save_ppm(path, &output);
    free(path);
    return ok;
}

static unsigned parse_codecs(const char *value) {
    if (strcmp(value, "all") == 0) return ALL_CODEC_MASK;
    if (strcmp(value, "n148i") == 0) return 1u << CODEC_N148_CURRENT;
    if (strcmp(value, "n148i_all") == 0)
        return (1u << CODEC_N148_CURRENT) | (1u << CODEC_N148_FORMAT_1);
    if (strcmp(value, "n148i_format_1") == 0)
        return 1u << CODEC_N148_FORMAT_1;
    if (strcmp(value, "jpeg") == 0) return 1u << CODEC_JPEG;
    if (strcmp(value, "jxl_e3") == 0) return 1u << CODEC_JXL_E3;
    if (strcmp(value, "jxl_e7") == 0) return 1u << CODEC_JXL_E7;
    if (strcmp(value, "webp_m4") == 0) return 1u << CODEC_WEBP_M4;
    if (strcmp(value, "webp_m6") == 0) return 1u << CODEC_WEBP_M6;
    return 0;
}

static void usage(const char *program) {
    fprintf(stderr,
        "usage: %s --input IMAGE.ppm [options]\n"
        "  --n148-quality N    N.148i quality, default 60\n"
        "  --n148-chroma-quality N  chroma quality, 0 = calibrated default\n"
        "  --n148-effort N     N.148i search effort, default 3\n"
        "  --n148-feature-flags N  N.148i feature mask, decimal or 0x hex\n"
        "  --jpeg-quality N    JPEG quality, default 60\n"
        "  --webp-quality X    WebP quality, default 60\n"
        "  --jxl-distance X    JPEG XL distance, default 2.5\n"
        "  --reps N            measured samples, default 15\n"
        "  --warmups N         warmup samples, default 3\n"
        "  --sample-ms X       target duration per batched sample, default 20\n"
        "  --chroma N          N.148i/JPEG chroma 0/1/2, default 2\n"
        "  --n148-threads N    total N.148i threads, default 1\n"
        "  --jxl-threads N     JPEG XL worker threads (0 = caller), default 0\n"
        "  --webp-thread-level N  WebP internal threading, 0 or 1\n"
        "  --jpeg-cpu N        pin JPEG to this logical CPU, default -1\n"
        "  --codecs LIST       all, n148i_all, or one codec name\n"
        "  --output-prefix P   write P-codec.ppm after timing\n",
        program);
}

static int parse_options(int argc, char **argv, Options *options) {
    *options = (Options) {
        .input = NULL,
        .output_prefix = NULL,
        .n148_quality = 60,
        .n148_chroma_quality = 0,
        .n148_effort = 3,
        .n148_feature_flags = N148I_DEFAULT_FEATURES,
        .jpeg_quality = 60,
        .webp_quality = 60.0f,
        .jxl_distance = 2.5f,
        .reps = 15,
        .warmups = 3,
        .chroma = N148I_CHROMA_420,
        .n148_threads = 1,
        .jxl_threads = 0,
        .webp_thread_level = 0,
        .jpeg_cpu = -1,
        .sample_ms = 20.0,
        .active_mask = ALL_CODEC_MASK,
    };
    static const struct option LONG_OPTIONS[] = {
        {"input", required_argument, NULL, 'i'},
        {"n148-quality", required_argument, NULL, 'n'},
        {"n148-chroma-quality", required_argument, NULL, 'C'},
        {"n148-effort", required_argument, NULL, 'e'},
        {"n148-feature-flags", required_argument, NULL, 'F'},
        {"jpeg-quality", required_argument, NULL, 'q'},
        {"webp-quality", required_argument, NULL, 'b'},
        {"jxl-distance", required_argument, NULL, 'd'},
        {"reps", required_argument, NULL, 'r'},
        {"warmups", required_argument, NULL, 'w'},
        {"sample-ms", required_argument, NULL, 's'},
        {"chroma", required_argument, NULL, 'c'},
        {"n148-threads", required_argument, NULL, 't'},
        {"jxl-threads", required_argument, NULL, 'j'},
        {"webp-thread-level", required_argument, NULL, 'u'},
        {"jpeg-cpu", required_argument, NULL, 'p'},
        {"codecs", required_argument, NULL, 'k'},
        {"output-prefix", required_argument, NULL, 'o'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;
    while ((option = getopt_long(argc, argv,
                                 "i:n:C:e:F:q:b:d:r:w:s:c:t:j:u:p:k:o:h",
                                 LONG_OPTIONS, NULL)) != -1) {
        switch (option) {
            case 'i': options->input = optarg; break;
            case 'n': options->n148_quality = atoi(optarg); break;
            case 'C': options->n148_chroma_quality = atoi(optarg); break;
            case 'e': options->n148_effort = atoi(optarg); break;
            case 'F': {
                char *end = NULL;
                errno = 0;
                unsigned long value = strtoul(optarg, &end, 0);
                if (errno || !end || *end || value > UINT32_MAX) return -1;
                options->n148_feature_flags = (uint32_t) value;
                break;
            }
            case 'q': options->jpeg_quality = atoi(optarg); break;
            case 'b': options->webp_quality = strtof(optarg, NULL); break;
            case 'd': options->jxl_distance = strtof(optarg, NULL); break;
            case 'r': options->reps = atoi(optarg); break;
            case 'w': options->warmups = atoi(optarg); break;
            case 's': options->sample_ms = strtod(optarg, NULL); break;
            case 'c': options->chroma = atoi(optarg); break;
            case 't': options->n148_threads = atoi(optarg); break;
            case 'j': options->jxl_threads = atoi(optarg); break;
            case 'u': options->webp_thread_level = atoi(optarg); break;
            case 'p': options->jpeg_cpu = atoi(optarg); break;
            case 'k': options->active_mask = parse_codecs(optarg); break;
            case 'o': options->output_prefix = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default: return -1;
        }
    }
    if (!options->input || options->active_mask == 0 ||
        options->n148_quality < 1 || options->n148_quality > 100 ||
        options->n148_chroma_quality < 0 ||
        options->n148_chroma_quality > 100 ||
        options->n148_effort < 0 || options->n148_effort > 9 ||
        options->jpeg_quality < 1 || options->jpeg_quality > 100 ||
        !(options->webp_quality >= 0.0f) || options->webp_quality > 100.0f ||
        !(options->jxl_distance > 0.0f) || options->jxl_distance > 25.0f ||
        options->reps < 1 || options->warmups < 0 ||
        !(options->sample_ms > 0.0) || options->chroma < 0 ||
        options->chroma > 2 || options->n148_threads < 1 ||
        options->n148_threads > 32 || options->jxl_threads < 0 ||
        options->webp_thread_level < 0 || options->webp_thread_level > 1 ||
        options->jpeg_cpu < -1) {
        usage(argv[0]);
        return -1;
    }
    return 1;
}

int main(int argc, char **argv) {
    Options options;
    int parsed = parse_options(argc, argv, &options);
    if (parsed <= 0) return parsed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    Image original = {0};
    CodecState states[CODEC_COUNT] = {{0}};
    CpuAffinity all_affinity, jpeg_affinity;
    int affinity_captured = 0;
    int pin_jpeg = options.jpeg_cpu >= 0;
    void *jxl_runner = NULL;
    int active[CODEC_COUNT];
    int active_count = 0;
    int result = EXIT_FAILURE;

    if (!load_ppm(options.input, &original)) goto cleanup;
    if (!capture_affinity(&all_affinity)) {
        fprintf(stderr, "could not configure CPU affinity: %s\n",
                strerror(errno));
        goto cleanup;
    }
    affinity_captured = 1;
    if (pin_jpeg && !single_cpu_affinity(&all_affinity, options.jpeg_cpu,
                                         &jpeg_affinity)) {
        fprintf(stderr, "could not configure CPU affinity: %s\n",
                strerror(errno));
        goto cleanup;
    }
    if (options.active_mask & ((1u << CODEC_JXL_E3) | (1u << CODEC_JXL_E7))) {
#ifdef N148_BENCH_NO_JXL
        fprintf(stderr, "JPEG XL is unavailable in this benchmark build\n");
        goto cleanup;
#else
        jxl_runner = JxlThreadParallelRunnerCreate(NULL,
                                                   (size_t) options.jxl_threads);
        if (!jxl_runner) {
            fprintf(stderr, "could not create JPEG XL thread runner\n");
            goto cleanup;
        }
#endif
    }
    for (int codec = 0; codec < CODEC_COUNT; codec++) {
        if (options.active_mask & (1u << codec)) active[active_count++] = codec;
        states[codec].width = original.width;
        states[codec].height = original.height;
    }

    /* One unmeasured call calibrates each batch and verifies every path. */
    for (int index = 0; index < active_count; index++) {
        int codec = active[index];
        if (!codec_affinity(codec, &all_affinity, &jpeg_affinity, pin_jpeg))
            goto cleanup;
        double start = now_ms();
        if (!encode_once(codec, &original, &options, jxl_runner, states)) {
            fprintf(stderr, "%s encode calibration failed\n", CODEC_NAMES[codec]);
            goto cleanup;
        }
        states[codec].encode_inner =
            calibrated_inner(now_ms() - start, options.sample_ms);

        start = now_ms();
        if (!decode_once(codec, &options, jxl_runner, states)) {
            fprintf(stderr, "%s decode calibration failed\n", CODEC_NAMES[codec]);
            goto cleanup;
        }
        states[codec].decode_inner =
            calibrated_inner(now_ms() - start, options.sample_ms);
    }

    printf("meta,%d,%d,%d,%d,%.9g,%d,%d,%d,%d,%u,%d\n",
           original.width, original.height, options.n148_quality,
           options.jpeg_quality, options.jxl_distance, options.reps,
           options.warmups, options.chroma, options.n148_threads,
#ifdef N148_BENCH_NO_JXL
           0u, options.jxl_threads);
#else
           (unsigned) JxlEncoderVersion(), options.jxl_threads);
#endif
    for (int index = 0; index < active_count; index++) {
        int codec = active[index];
        printf("calibration,%s,%d,%d\n", CODEC_NAMES[codec],
               states[codec].encode_inner, states[codec].decode_inner);
    }

    for (int rep = -options.warmups; rep < options.reps; rep++) {
        double encode_times[CODEC_COUNT] = {0};
        double decode_times[CODEC_COUNT] = {0};
        int rotation = (rep + options.warmups) % active_count;
        for (int pass = 0; pass < active_count; pass++) {
            int codec = active[(pass + rotation) % active_count];
            if (!codec_affinity(codec, &all_affinity, &jpeg_affinity,
                                pin_jpeg) ||
                !measure_encode(codec, states[codec].encode_inner, &original,
                                &options, jxl_runner, states,
                                &encode_times[codec])) {
                fprintf(stderr, "%s encode sample failed\n", CODEC_NAMES[codec]);
                goto cleanup;
            }
        }
        for (int pass = 0; pass < active_count; pass++) {
            int codec = active[(active_count - 1 - pass + rotation) %
                               active_count];
            if (!codec_affinity(codec, &all_affinity, &jpeg_affinity,
                                pin_jpeg) ||
                !measure_decode(codec, states[codec].decode_inner, &options,
                                jxl_runner, states, &decode_times[codec])) {
                fprintf(stderr, "%s decode sample failed\n", CODEC_NAMES[codec]);
                goto cleanup;
            }
        }
        if (rep >= 0) {
            for (int index = 0; index < active_count; index++) {
                int codec = active[index];
                printf("sample,%s,%d,%.12f,%.12f\n", CODEC_NAMES[codec], rep,
                       encode_times[codec], decode_times[codec]);
            }
        }
    }

    if (!apply_affinity(&all_affinity)) goto cleanup;
    for (int index = 0; index < active_count; index++) {
        int codec = active[index];
        if (!states[codec].decoded || states[codec].compressed_size == 0 ||
            !save_reconstruction(options.output_prefix, CODEC_NAMES[codec],
                                 &states[codec])) {
            fprintf(stderr, "%s output validation failed\n", CODEC_NAMES[codec]);
            goto cleanup;
        }
        printf("result,%s,%zu,%s\n", CODEC_NAMES[codec],
               states[codec].compressed_size,
               options.output_prefix ? "reconstruction_written" : "validated");
    }
    fflush(stdout);
    result = EXIT_SUCCESS;

cleanup:
    if (affinity_captured) apply_affinity(&all_affinity);
    for (int codec = 0; codec < CODEC_COUNT; codec++)
        free_state(codec, &states[codec]);
#ifndef N148_BENCH_NO_JXL
    JxlThreadParallelRunnerDestroy(jxl_runner);
#endif
    free_image(&original);
    return result;
}

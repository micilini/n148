/*
 * N.148i public library API
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148I_H
#define N148I_H

#include <stddef.h>
#include <stdint.h>

#define N148I_VERSION_MAJOR 1
#define N148I_VERSION_MINOR 0
#define N148I_VERSION_PATCH 0
#define N148I_VERSION_STRING "1.0.0"

/* The file format has its own version and can evolve independently. */
#define N148I_FORMAT_VERSION 1
#define N148I_MAX_THREADS 32

#if defined(N148I_STATIC_DEFINE)
#define N148I_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(N148I_BUILDING_LIBRARY)
#define N148I_API __declspec(dllexport)
#else
#define N148I_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define N148I_API __attribute__((visibility("default")))
#else
#define N148I_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum n148i_result {
    N148I_OK = 0,
    N148I_ERROR_INVALID_ARGUMENT = 1,
    N148I_ERROR_OUT_OF_MEMORY = 2,
    N148I_ERROR_INVALID_FORMAT = 3,
    N148I_ERROR_UNSUPPORTED_VERSION = 4,
    N148I_ERROR_TRUNCATED_DATA = 5,
    N148I_ERROR_CORRUPT_DATA = 6,
    N148I_ERROR_SIZE_OVERFLOW = 7,
    N148I_ERROR_ENCODE_FAILED = 8,
    N148I_ERROR_DECODE_FAILED = 9,
    N148I_ERROR_UNSUPPORTED_SIMD = 10
} n148i_result_t;

typedef enum n148i_chroma {
    N148I_CHROMA_444 = 0,
    N148I_CHROMA_422 = 1,
    N148I_CHROMA_420 = 2
} n148i_chroma_t;

typedef enum n148i_simd_level {
    N148I_SIMD_AUTO = -1,
    N148I_SIMD_SCALAR = 0,
    N148I_SIMD_SSE2 = 1,
    N148I_SIMD_AVX2 = 2,
    N148I_SIMD_AVX2_FMA = 3
} n148i_simd_level_t;

/* Interleaved 8-bit RGB pixels. A zero stride means width * 3 bytes. */
typedef struct n148i_image {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    size_t stride;
} n148i_image_t;

/*
 * Initialize this structure with n148i_encode_options_init(). The struct_size
 * member lets future library versions append fields without breaking callers.
 */
typedef struct n148i_encode_options {
    size_t struct_size;
    int quality;
    n148i_chroma_t chroma;
    int optimize_huffman;
    int thread_count;
} n148i_encode_options_t;

typedef struct n148i_image_info {
    uint32_t width;
    uint32_t height;
    uint32_t payload_size;
    size_t encoded_header_size;
    uint8_t format_version;
    uint8_t quality;
    n148i_chroma_t chroma;
    int optimized_huffman;
} n148i_image_info_t;

/* Fill options with the v1 defaults: quality 50, 4:2:0, optimized Huffman. */
N148I_API void n148i_encode_options_init(n148i_encode_options_t *options);

/*
 * Encode an RGB image into a newly allocated N.148i buffer. Pass NULL options
 * to use the defaults. Release the returned buffer with n148i_free_buffer().
 */
N148I_API n148i_result_t n148i_encode_memory(
    const n148i_image_t *image,
    const n148i_encode_options_t *options,
    uint8_t **encoded,
    size_t *encoded_size);

/*
 * Decode a complete N.148i buffer into newly allocated, tightly packed RGB
 * pixels. Release the returned image with n148i_free_image().
 */
N148I_API n148i_result_t n148i_decode_memory(
    const uint8_t *encoded,
    size_t encoded_size,
    n148i_image_t *image);

/* Parse and validate the container header without decoding the pixels. */
N148I_API n148i_result_t n148i_read_header(
    const uint8_t *encoded,
    size_t encoded_size,
    n148i_image_info_t *info);

/* Memory returned by this library must be released by this library. */
N148I_API void n148i_free_buffer(void *buffer);
N148I_API void n148i_free_image(n148i_image_t *image);

N148I_API const char *n148i_result_string(n148i_result_t result);
N148I_API const char *n148i_library_version(void);
N148I_API uint32_t n148i_format_version(void);

/* Query or override runtime SIMD dispatch. Configure it before worker calls. */
N148I_API n148i_simd_level_t n148i_simd_level(void);
N148I_API const char *n148i_simd_name(n148i_simd_level_t level);
N148I_API n148i_result_t n148i_simd_force(n148i_simd_level_t level);

#ifdef __cplusplus
}
#endif

#endif

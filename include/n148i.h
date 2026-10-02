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

#define N148I_VERSION_MAJOR 2
#define N148I_VERSION_MINOR 0
#define N148I_VERSION_PATCH 0
#define N148I_VERSION_STRING "V2"

/* The file format has its own version and can evolve independently. */
#define N148I_FORMAT_VERSION_1 1
#define N148I_FORMAT_VERSION_2 2
#define N148I_FORMAT_VERSION_3 3
#define N148I_FORMAT_VERSION_4 4
#define N148I_FORMAT_VERSION_5 5
#define N148I_FORMAT_VERSION_6 6
#define N148I_FORMAT_VERSION_7 7
#define N148I_FORMAT_VERSION N148I_FORMAT_VERSION_7
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
    N148I_ERROR_UNSUPPORTED_SIMD = 10,
    N148I_ERROR_UNSUPPORTED_FEATURE = 11
} n148i_result_t;

typedef enum n148i_chroma {
    N148I_CHROMA_444 = 0,
    N148I_CHROMA_422 = 1,
    N148I_CHROMA_420 = 2,
    /* Encoder-only automatic-chroma request. Streams always store one of the three
       concrete layouts above. Effort 0 resolves AUTO directly to 4:2:0. */
    N148I_CHROMA_AUTO = 3
} n148i_chroma_t;

typedef enum n148i_simd_level {
    N148I_SIMD_AUTO = -1,
    N148I_SIMD_SCALAR = 0,
    N148I_SIMD_SSE2 = 1,
    N148I_SIMD_AVX2 = 2,
    N148I_SIMD_AVX2_FMA = 3
} n148i_simd_level_t;

typedef enum n148i_feature {
    N148I_FEATURE_RANS = 1u << 0,
    N148I_FEATURE_CONTEXT = 1u << 1,
    N148I_FEATURE_INTRA = 1u << 2,
    N148I_FEATURE_PERCEPTUAL_COLOR = 1u << 3, /* format 2 only */
    N148I_FEATURE_ADAPTIVE_QUANT = 1u << 4,
    N148I_FEATURE_VARIABLE_TRANSFORM = 1u << 5,
    N148I_FEATURE_RDO = 1u << 6,
    N148I_FEATURE_LOOP_FILTER = 1u << 7,
    N148I_FEATURE_DIRECTIONAL_INTRA = 1u << 8,
    N148I_FEATURE_INTRA_4X4 = 1u << 9,
    N148I_FEATURE_CONTEXTUAL_RDO = 1u << 10,
    N148I_FEATURE_PERCEPTUAL_TRELLIS = 1u << 11,
    N148I_FEATURE_RECONSTRUCTION_AWARE_CHROMA = 1u << 12,
    N148I_FEATURE_JOINT_CHROMA_INTRA = 1u << 13,
    N148I_FEATURE_CALIBRATED_CHROMA_QUANT = 1u << 14,
    N148I_FEATURE_SEGMENTATION = 1u << 15,
    N148I_FEATURE_ADAPTIVE_LOOP_FILTER = 1u << 16,
    N148I_FEATURE_CALIBRATED_LUMA_QUANT = 1u << 17,
    N148I_FEATURE_LUMA_TRANSFORM_16X16 = 1u << 18,
    N148I_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER = 1u << 19,
    N148I_FEATURE_FILTERED_INTRA_REFERENCES = 1u << 20,
    N148I_FEATURE_PLANAR_INTRA_PREDICTION = 1u << 21,
    N148I_FEATURE_STRUCTURAL_RDO = 1u << 22,
    N148I_FEATURE_QUALITY_LAMBDA = 1u << 23,
    N148I_FEATURE_LUMA_TRANSFORM_32X32 = 1u << 24,
    N148I_FEATURE_SECOND_ORDER_DC = 1u << 25,
    N148I_FEATURE_FINE_INTRA_DIRECTIONS = 1u << 26,
    N148I_FEATURE_ADAPTIVE_ENTROPY = 1u << 27,
    N148I_FEATURE_RICH_COEFFICIENT_CONTEXTS = 1u << 28,
    N148I_FEATURE_MULTIPLE_INTRA_REFERENCES = 1u << 29,
    N148I_FEATURE_DIRECTIONAL_SCAN = 1u << 30
} n148i_feature_t;

/* Kept as a macro because ISO C requires enum values to fit in signed int. */
#define N148I_FEATURE_DIRECTIONAL_TRANSFORM ((uint32_t) 1u << 31)

/* N148I_FEATURE_ALL retains the base-feature mask for source compatibility.
   Later formats use their own masks because rejected experiments remain opt-in. */
#define N148I_FEATURE_ALL ((uint32_t) 0xffu)
#define N148I_FORMAT_2_DEFAULT_FEATURES ((uint32_t) 0xc7u)
#define N148I_FORMAT_2_FEATURE_MASK N148I_FEATURE_ALL
#define N148I_PROFILE_DIRECTIONAL_INTRA ((uint32_t) 0x3c7u)
#define N148I_PROFILE_CONTEXTUAL_RDO ((uint32_t) 0x7c7u)
#define N148I_PROFILE_PERCEPTUAL_TRELLIS ((uint32_t) 0xfc7u)
#define N148I_FORMAT_3_DEFAULT_FEATURES N148I_PROFILE_CONTEXTUAL_RDO
#define N148I_FORMAT_3_FEATURE_MASK N148I_PROFILE_PERCEPTUAL_TRELLIS
#define N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA ((uint32_t) 0x17c7u)
#define N148I_PROFILE_JOINT_CHROMA_INTRA ((uint32_t) 0x37c7u)
#define N148I_PROFILE_CALIBRATED_CHROMA_QUANT ((uint32_t) 0x57c7u)
#define N148I_PROFILE_CHROMA_SEGMENTATION ((uint32_t) 0xd7c7u)
#define N148I_PROFILE_ADAPTIVE_CHROMA_FILTER ((uint32_t) 0x1d7c7u)
#define N148I_FORMAT_4_DEFAULT_FEATURES N148I_PROFILE_CALIBRATED_CHROMA_QUANT
#define N148I_FORMAT_4_FEATURE_MASK ((uint32_t) 0x1f7c7u)
#define N148I_PROFILE_CALIBRATED_LUMA_QUANT ((uint32_t) 0x257c7u)
#define N148I_PROFILE_VARIABLE_LUMA_TRANSFORM ((uint32_t) 0x657c7u)
#define N148I_PROFILE_PERCEPTUAL_LUMA_TRELLIS ((uint32_t) 0x65fc7u)
#define N148I_PROFILE_QUANT_ADAPTIVE_LUMA_FILTER ((uint32_t) 0xe57c7u)
#define N148I_FORMAT_5_DEFAULT_FEATURES N148I_PROFILE_VARIABLE_LUMA_TRANSFORM
#define N148I_FORMAT_5_FEATURE_MASK ((uint32_t) 0xff7c7u)
#define N148I_PROFILE_LUMA_TRANSFORM_32X32 ((uint32_t) 0x10657c7u)
#define N148I_PROFILE_FILTERED_INTRA_REFERENCES ((uint32_t) 0x1657c7u)
#define N148I_PROFILE_PLANAR_INTRA_PREDICTION ((uint32_t) 0x2657c7u)
#define N148I_PROFILE_COMBINED_INTRA_PREDICTION ((uint32_t) 0x3657c7u)
#define N148I_PROFILE_STRUCTURAL_RDO ((uint32_t) 0x6657c7u)
#define N148I_PROFILE_QUALITY_LAMBDA ((uint32_t) 0xa657c7u)
#define N148I_FORMAT_6_DEFAULT_FEATURES N148I_PROFILE_PLANAR_INTRA_PREDICTION
#define N148I_FORMAT_6_FEATURE_MASK ((uint32_t) 0x7fff7c7u)
#define N148I_PROFILE_ADAPTIVE_ENTROPY ((uint32_t) 0x82657c7u)
#define N148I_PROFILE_RICH_COEFFICIENT_CONTEXTS ((uint32_t) 0x182657c7u)
#define N148I_PROFILE_MULTIPLE_INTRA_REFERENCES ((uint32_t) 0x382657c7u)
#define N148I_PROFILE_DIRECTIONAL_SCAN ((uint32_t) 0x782657c7u)
#define N148I_PROFILE_DIRECTIONAL_TRANSFORM ((uint32_t) 0xf82657c7u)
#define N148I_PROFILE_RATE_CALIBRATED_SEARCH \
    (N148I_PROFILE_RICH_COEFFICIENT_CONTEXTS | N148I_FEATURE_QUALITY_LAMBDA)
#define N148I_FORMAT_7_DEFAULT_FEATURES N148I_PROFILE_RATE_CALIBRATED_SEARCH
/* Format 7 alone uses bit 3 for the spectral luma table. In format 2 the
   same bit retains its historical perceptual-colour meaning. The file-format
   version disambiguates them; existing format 7 readers reject this bit. */
#define N148I_FORMAT_7_SPECTRAL_LUMA_QUANT ((uint32_t) 1u << 3)
#define N148I_PROFILE_SPECTRAL_LUMA_QUANT \
    (N148I_PROFILE_RATE_CALIBRATED_SEARCH | \
     N148I_FORMAT_7_SPECTRAL_LUMA_QUANT)
/* Format 7 alone uses bit 4 to select the refined spectral table, together
   with bit 3. In format 2, bit 4 retains its adaptive-quantization meaning. */
#define N148I_FORMAT_7_REFINED_SPECTRAL_LUMA_QUANT ((uint32_t) 1u << 4)
#define N148I_PROFILE_REFINED_SPECTRAL_LUMA_QUANT \
    (N148I_PROFILE_SPECTRAL_LUMA_QUANT | \
     N148I_FORMAT_7_REFINED_SPECTRAL_LUMA_QUANT)
#define N148I_FORMAT_7_FULL_TRELLIS_FEATURES \
    (N148I_FORMAT_7_DEFAULT_FEATURES | N148I_FEATURE_PERCEPTUAL_TRELLIS)
/* Format 7 alone uses bit 5 for the structural luma table. Format 2 keeps
   its original variable-transform meaning. This profile adds segmentation
   and encoder preparation of luma from reconstructed chroma. */
#define N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT ((uint32_t) 1u << 5)
#define N148I_PROFILE_STRUCTURAL_LUMA_QUANT \
    (N148I_PROFILE_REFINED_SPECTRAL_LUMA_QUANT | \
     N148I_FEATURE_SEGMENTATION | N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT)
/* Format 7 bit 31 identifies the balanced table and output chroma
   reconstruction. The former planned directional profiles were never
   supported and lack the required structural reconstruction features. */
#define N148I_FORMAT_7_BALANCED_RECONSTRUCTION ((uint32_t) 1u << 31)
#define N148I_PROFILE_BALANCED_RECONSTRUCTION \
    (N148I_PROFILE_STRUCTURAL_LUMA_QUANT | \
     N148I_FORMAT_7_BALANCED_RECONSTRUCTION)
/* Format 7 bit 30 identifies the detail reconstruction profile. It keeps
   balanced chroma reconstruction and uses a 100% DCT16 quantizer scale; older
   profiles retain their 115% scale. Readers without this profile reject bit 30. */
#define N148I_FORMAT_7_DETAIL_RECONSTRUCTION ((uint32_t) 1u << 30)
#define N148I_PROFILE_DETAIL_RECONSTRUCTION \
    (N148I_PROFILE_BALANCED_RECONSTRUCTION | \
     N148I_FORMAT_7_DETAIL_RECONSTRUCTION)
#define N148I_FORMAT_7_FEATURE_MASK ((uint32_t) 0xffffffffu)

/* V2's public default. Numbered format presets above retain their original
   feature masks for applications that explicitly select a legacy profile. */
#define N148I_DEFAULT_FEATURES N148I_PROFILE_DETAIL_RECONSTRUCTION

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
    uint32_t format_version;
    uint32_t feature_flags;
    /* Default 3. The structural profile uses six quantized luma candidates
       and contextual coefficient refinement at effort 3. The refined
       spectral profile selectively tests a second luma mode; effort 4
       enables broader selective search on format-7 profiles with rich
       contexts and quality-lambda search. */
    int effort;
    /* Zero selects quality for the structural profile, or the calibrated
       default (quality + 5, clamped to 100) for older profiles.
       A separate 1..100 value requires the calibrated-chroma-quantization
       feature and is stored in the stream. Older formats require zero. */
    int chroma_quality;
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
    uint32_t feature_flags;
    uint8_t effort;
    uint32_t metadata_size;
    uint8_t chroma_quality;
} n148i_image_info_t;

/*
 * Thread safety:
 * - version/error queries, header parsing, and deallocation may run together;
 * - encode and decode calls require external serialization because the codec
 *   shares dispatch, thread configuration, histogram scratch, and a pool;
 * - configure SIMD and nonzero thread_count values before codec work starts.
 * Returned buffers are independently owned after an operation completes.
 */

/* Fill options with V2 defaults: quality 50, 4:2:0, detail reconstruction,
   chroma quality equal to luma, adaptive contextual rANS, intra prediction,
   variable transforms and effort 3. When forcing an older format, also
   select that format's default feature mask and leave chroma_quality at
   zero. */
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

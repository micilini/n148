/*
 * N.148i public memory-to-memory API
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include "n148i.h"

#include <float.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "decoder.h"
#include "encoder.h"
#include "header.h"
#include "huffman.h"
#include "parallel.h"
#include "ppm.h"
#include "tables.h"
#include "entropy_payload.h"
#include "predictive_codec.h"
#include "chroma_profile.h"
#include "luma_profile.h"
#include "fidelity_profile.h"
#include "entropy_profile.h" /* explicit detail-profile marker */
#include "directional_intra.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t payload_size;
    size_t payload_offset;
    size_t metadata_offset;
    uint32_t metadata_size;
    uint32_t feature_flags;
    uint8_t version;
    uint8_t quality;
    uint8_t chroma_quality;
    uint8_t chroma;
    uint8_t optimized;
    uint8_t effort;
    HuffSpec specs[HUFFMAN_TABLE_COUNT];
} ParsedHeader;

static void memory_write_u16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8);
}

static void memory_write_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t) (value & 0xffu);
    destination[1] = (uint8_t) ((value >> 8) & 0xffu);
    destination[2] = (uint8_t) ((value >> 16) & 0xffu);
    destination[3] = (uint8_t) ((value >> 24) & 0xffu);
}

static uint32_t memory_read_u32(const uint8_t *source) {
    return (uint32_t) source[0] |
        ((uint32_t) source[1] << 8) |
        ((uint32_t) source[2] << 16) |
        ((uint32_t) source[3] << 24);
}

static uint16_t memory_read_u16(const uint8_t *source) {
    return (uint16_t)(source[0] | ((uint16_t) source[1] << 8));
}

static int valid_stored_chroma(int chroma) {
    return chroma == CHROMA_444 || chroma == CHROMA_422 ||
        chroma == CHROMA_420;
}

static int valid_encoder_chroma(int chroma) {
    return valid_stored_chroma(chroma) || chroma == N148I_CHROMA_AUTO;
}

static int multiplication_overflows(size_t first, size_t second) {
    return second != 0 && first > SIZE_MAX / second;
}

static int effective_chroma_quality(
    const n148i_encode_options_t *settings) {
    if (!(settings->feature_flags &
          N148_FEATURE_CALIBRATED_CHROMA_QUANT))
        return settings->quality;
    if (settings->chroma_quality)
        return settings->chroma_quality;
    if (settings->format_version == N148I_FORMAT_VERSION_7 &&
        (settings->feature_flags & N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT))
        return settings->quality;
    return settings->quality > 95 ? 100 : settings->quality + 5;
}

static n148i_result_t parse_header(const uint8_t *encoded,
                                   size_t encoded_size,
                                   ParsedHeader *parsed) {
    if (!encoded || !parsed) return N148I_ERROR_INVALID_ARGUMENT;
    if (encoded_size < N148I_MAGIC_LEN + 1u)
        return N148I_ERROR_TRUNCATED_DATA;
    if (memcmp(encoded, N148I_MAGIC, N148I_MAGIC_LEN) != 0)
        return N148I_ERROR_INVALID_FORMAT;

    memset(parsed, 0, sizeof(*parsed));
    parsed->version = encoded[5];
    if (parsed->version != N148I_FORMAT_VERSION_1 &&
        parsed->version != N148I_FORMAT_VERSION_2 &&
        parsed->version != N148I_FORMAT_VERSION_3 &&
        parsed->version != N148I_FORMAT_VERSION_4 &&
        parsed->version != N148I_FORMAT_VERSION_5 &&
        parsed->version != N148I_FORMAT_VERSION_6 &&
        parsed->version != N148I_FORMAT_VERSION_7)
        return N148I_ERROR_UNSUPPORTED_VERSION;

    if (parsed->version == N148I_FORMAT_VERSION_2 ||
        parsed->version == N148I_FORMAT_VERSION_3 ||
        parsed->version == N148I_FORMAT_VERSION_4 ||
        parsed->version == N148I_FORMAT_VERSION_5 ||
        parsed->version == N148I_FORMAT_VERSION_6 ||
        parsed->version == N148I_FORMAT_VERSION_7) {
        if (encoded_size < N148_EXTENDED_HEADER_SIZE)
            return N148I_ERROR_TRUNCATED_DATA;
        uint16_t header_size = memory_read_u16(encoded + 6);
        parsed->width = memory_read_u32(encoded + 8);
        parsed->height = memory_read_u32(encoded + 12);
        parsed->payload_size = memory_read_u32(encoded + 16);
        parsed->feature_flags = memory_read_u32(encoded + 20);
        parsed->quality = encoded[24];
        parsed->chroma = encoded[25];
        parsed->effort = encoded[26];
        parsed->chroma_quality = encoded[27];
        parsed->metadata_size = memory_read_u32(encoded + 28);
        int unsupported_features = parsed->version == N148I_FORMAT_VERSION_2 ?
            (parsed->feature_flags & ~N148_FORMAT_2_SUPPORTED_FEATURES) != 0 :
            parsed->version == N148I_FORMAT_VERSION_3 ?
            (parsed->feature_flags & ~N148_FORMAT_3_SUPPORTED_FEATURES) != 0 :
            parsed->version == N148I_FORMAT_VERSION_4 ?
            (parsed->feature_flags & ~N148_FORMAT_4_SUPPORTED_FEATURES) != 0 :
            parsed->version == N148I_FORMAT_VERSION_5 ?
            (parsed->feature_flags & ~N148_FORMAT_5_SUPPORTED_FEATURES) != 0 :
            parsed->version == N148I_FORMAT_VERSION_6 ?
            (parsed->feature_flags & ~N148_FORMAT_6_SUPPORTED_FEATURES) != 0 :
            (parsed->feature_flags & ~N148_FORMAT_7_SUPPORTED_FEATURES) != 0;
        int valid_features = parsed->version == N148I_FORMAT_VERSION_2 ?
            !unsupported_features &&
            !((parsed->feature_flags &
               (N148_FEATURE_CONTEXT | N148_FEATURE_INTRA |
                N148_FEATURE_PERCEPTUAL_COLOR |
                N148_FEATURE_ADAPTIVE_QUANT |
                N148_FEATURE_VARIABLE_TRANSFORM |
                N148_FEATURE_RDO | N148_FEATURE_LOOP_FILTER)) &&
              !(parsed->feature_flags & N148_FEATURE_RANS)) &&
            !((parsed->feature_flags & N148_FEATURE_ADAPTIVE_QUANT) &&
              !(parsed->feature_flags & N148_FEATURE_INTRA)) &&
            !((parsed->feature_flags & N148_FEATURE_VARIABLE_TRANSFORM) &&
              !(parsed->feature_flags & N148_FEATURE_INTRA)) &&
            !((parsed->feature_flags & N148_FEATURE_RDO) &&
              !(parsed->feature_flags & N148_FEATURE_INTRA)) &&
            !((parsed->feature_flags & N148_FEATURE_LOOP_FILTER) &&
              !(parsed->feature_flags & N148_FEATURE_INTRA)) :
            parsed->version == N148I_FORMAT_VERSION_3 ?
            n148_format_3_features_valid(parsed->feature_flags) :
            parsed->version == N148I_FORMAT_VERSION_4 ?
            n148_format_4_features_valid(parsed->feature_flags) :
            parsed->version == N148I_FORMAT_VERSION_5 ?
            n148_format_5_features_valid(parsed->feature_flags) :
            parsed->version == N148I_FORMAT_VERSION_6 ?
            n148_format_6_features_valid(parsed->feature_flags) :
            n148_format_7_features_valid(parsed->feature_flags);
        if (parsed->width == 0 || parsed->width > INT_MAX ||
            parsed->height == 0 || parsed->height > INT_MAX ||
            parsed->payload_size == 0 || parsed->metadata_size == 0 ||
            parsed->quality < 1 || parsed->quality > 100 ||
            !valid_stored_chroma(parsed->chroma) || parsed->effort > 9 ||
            ((parsed->version == N148I_FORMAT_VERSION_4 ||
              parsed->version == N148I_FORMAT_VERSION_5 ||
              parsed->version == N148I_FORMAT_VERSION_6 ||
              parsed->version == N148I_FORMAT_VERSION_7) &&
             (parsed->feature_flags &
              N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA) &&
             parsed->chroma == CHROMA_444) ||
            (((parsed->version == N148I_FORMAT_VERSION_4 ||
               parsed->version == N148I_FORMAT_VERSION_5 ||
               parsed->version == N148I_FORMAT_VERSION_6 ||
               parsed->version == N148I_FORMAT_VERSION_7) &&
              (parsed->feature_flags &
               N148_FEATURE_CALIBRATED_CHROMA_QUANT)) ?
                (parsed->chroma_quality < 1 ||
                 parsed->chroma_quality > 100) :
                parsed->chroma_quality != 0) ||
            !valid_features) {
            return unsupported_features ?
                N148I_ERROR_UNSUPPORTED_FEATURE : N148I_ERROR_INVALID_FORMAT;
        }
        if (parsed->metadata_size > UINT16_MAX - N148_EXTENDED_HEADER_SIZE ||
            header_size != N148_EXTENDED_HEADER_SIZE + parsed->metadata_size)
            return N148I_ERROR_INVALID_FORMAT;
        if (header_size > encoded_size ||
            parsed->payload_size > encoded_size - header_size)
            return N148I_ERROR_TRUNCATED_DATA;
        if (parsed->payload_size != encoded_size - header_size)
            return N148I_ERROR_CORRUPT_DATA;
        parsed->metadata_offset = N148_EXTENDED_HEADER_SIZE;
        parsed->payload_offset = header_size;
        if (parsed->chroma_quality == 0)
            parsed->chroma_quality = parsed->quality;
        return N148I_OK;
    }

    if (encoded_size < N148_FORMAT_1_HEADER_SIZE)
        return N148I_ERROR_TRUNCATED_DATA;

    parsed->width = memory_read_u32(encoded + 6);
    parsed->height = memory_read_u32(encoded + 10);
    parsed->quality = encoded[14];
    parsed->chroma_quality = parsed->quality;
    parsed->chroma = encoded[15];
    parsed->optimized = encoded[16];
    parsed->payload_size = memory_read_u32(encoded + 17);

    if (parsed->width == 0 || parsed->width > INT_MAX ||
        parsed->height == 0 || parsed->height > INT_MAX ||
        parsed->quality < 1 || parsed->quality > 100 ||
        !valid_stored_chroma(parsed->chroma) || parsed->optimized > 1 ||
        parsed->payload_size == 0) {
        return N148I_ERROR_INVALID_FORMAT;
    }
#if LONG_MAX < UINT32_MAX
    if (parsed->payload_size > (uint32_t) LONG_MAX)
        return N148I_ERROR_SIZE_OVERFLOW;
#endif

    size_t offset = N148_FORMAT_1_HEADER_SIZE;
    if (parsed->optimized) {
        for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
            HuffSpec *spec = &parsed->specs[table];
            if (encoded_size - offset < 16)
                return N148I_ERROR_TRUNCATED_DATA;
            for (int length = 1; length <= 16; length++) {
                int count = encoded[offset++];
                spec->bits[length] = count;
                spec->value_count += count;
                if (spec->value_count > HUFFMAN_MAX_SYMBOLS)
                    return N148I_ERROR_CORRUPT_DATA;
            }
            if (spec->value_count == 0)
                return N148I_ERROR_CORRUPT_DATA;
            if ((size_t) spec->value_count > encoded_size - offset)
                return N148I_ERROR_TRUNCATED_DATA;
            memcpy(spec->values, encoded + offset,
                   (size_t) spec->value_count);
            offset += (size_t) spec->value_count;
            if (!huffman_spec_is_valid(spec))
                return N148I_ERROR_CORRUPT_DATA;
        }
    } else {
        huffman_default_specs(parsed->specs);
    }

    if ((size_t) parsed->payload_size > encoded_size - offset)
        return N148I_ERROR_TRUNCATED_DATA;
    if ((size_t) parsed->payload_size != encoded_size - offset)
        return N148I_ERROR_CORRUPT_DATA;
    parsed->payload_offset = offset;
    parsed->metadata_offset = N148_FORMAT_1_HEADER_SIZE;
    parsed->metadata_size = (uint32_t)(offset - N148_FORMAT_1_HEADER_SIZE);
    return N148I_OK;
}

static n148i_result_t normalize_options(
    const n148i_encode_options_t *input,
    n148i_encode_options_t *output) {
    n148i_encode_options_init(output);
    if (!input) return N148I_OK;

    if (input->struct_size <
        offsetof(n148i_encode_options_t, quality) + sizeof(input->quality)) {
        return N148I_ERROR_INVALID_ARGUMENT;
    }
    size_t copy_size = input->struct_size;
    if (copy_size > sizeof(*output)) copy_size = sizeof(*output);
    memcpy(output, input, copy_size);
    output->struct_size = sizeof(*output);

    if (output->quality < 1 || output->quality > 100 ||
        !valid_encoder_chroma(output->chroma) ||
        (output->optimize_huffman != 0 && output->optimize_huffman != 1) ||
        output->thread_count < 0 || output->thread_count > N148I_MAX_THREADS ||
        (output->format_version != N148I_FORMAT_VERSION_1 &&
         output->format_version != N148I_FORMAT_VERSION_2 &&
         output->format_version != N148I_FORMAT_VERSION_3 &&
         output->format_version != N148I_FORMAT_VERSION_4 &&
         output->format_version != N148I_FORMAT_VERSION_5 &&
         output->format_version != N148I_FORMAT_VERSION_6 &&
         output->format_version != N148I_FORMAT_VERSION_7) ||
        output->effort < 0 || output->effort > 9 ||
        output->chroma_quality < 0 || output->chroma_quality > 100) {
        return N148I_ERROR_INVALID_ARGUMENT;
    }
    if (output->chroma == N148I_CHROMA_AUTO &&
        output->format_version != N148I_FORMAT_VERSION_5 &&
        output->format_version != N148I_FORMAT_VERSION_6 &&
        output->format_version != N148I_FORMAT_VERSION_7)
        return N148I_ERROR_INVALID_ARGUMENT;
    if (output->format_version != N148I_FORMAT_VERSION_4 &&
        output->format_version != N148I_FORMAT_VERSION_5 &&
        output->format_version != N148I_FORMAT_VERSION_6 &&
        output->format_version != N148I_FORMAT_VERSION_7 &&
        output->chroma_quality != 0)
        return N148I_ERROR_INVALID_ARGUMENT;
    if (output->format_version == N148I_FORMAT_VERSION_2) {
        if (output->feature_flags & ~N148_FORMAT_2_SUPPORTED_FEATURES)
            return N148I_ERROR_UNSUPPORTED_FEATURE;
        if ((output->feature_flags &
             (N148_FEATURE_CONTEXT | N148_FEATURE_INTRA |
              N148_FEATURE_PERCEPTUAL_COLOR |
              N148_FEATURE_ADAPTIVE_QUANT |
              N148_FEATURE_VARIABLE_TRANSFORM |
              N148_FEATURE_RDO |
              N148_FEATURE_LOOP_FILTER)) &&
            !(output->feature_flags & N148_FEATURE_RANS))
            return N148I_ERROR_INVALID_ARGUMENT;
        if ((output->feature_flags & N148_FEATURE_ADAPTIVE_QUANT) &&
            !(output->feature_flags & N148_FEATURE_INTRA))
            return N148I_ERROR_INVALID_ARGUMENT;
        if ((output->feature_flags & N148_FEATURE_VARIABLE_TRANSFORM) &&
            !(output->feature_flags & N148_FEATURE_INTRA))
            return N148I_ERROR_INVALID_ARGUMENT;
        if ((output->feature_flags & N148_FEATURE_RDO) &&
            !(output->feature_flags & N148_FEATURE_INTRA))
            return N148I_ERROR_INVALID_ARGUMENT;
        if ((output->feature_flags & N148_FEATURE_LOOP_FILTER) &&
            !(output->feature_flags & N148_FEATURE_INTRA))
            return N148I_ERROR_INVALID_ARGUMENT;
    } else if (output->format_version == N148I_FORMAT_VERSION_3) {
        if (output->feature_flags & ~N148_FORMAT_3_SUPPORTED_FEATURES)
            return N148I_ERROR_UNSUPPORTED_FEATURE;
        if (!n148_format_3_features_valid(output->feature_flags))
            return N148I_ERROR_INVALID_ARGUMENT;
    } else if (output->format_version == N148I_FORMAT_VERSION_4) {
        if (output->feature_flags & ~N148_FORMAT_4_SUPPORTED_FEATURES)
            return N148I_ERROR_UNSUPPORTED_FEATURE;
        if (!n148_format_4_features_valid(output->feature_flags) ||
            (output->chroma_quality != 0 &&
             !(output->feature_flags &
               N148_FEATURE_CALIBRATED_CHROMA_QUANT)))
            return N148I_ERROR_INVALID_ARGUMENT;
        /* There is no reduction to optimize in 4:4:4. Canonicalize the
           encoder request instead of rejecting the otherwise valid default
           profile or writing a meaningless feature bit into the stream. */
        if (output->chroma == N148I_CHROMA_444)
            output->feature_flags &=
                ~N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA;
    } else if (output->format_version == N148I_FORMAT_VERSION_5) {
        if (output->feature_flags & ~N148_FORMAT_5_SUPPORTED_FEATURES)
            return N148I_ERROR_UNSUPPORTED_FEATURE;
        if (!n148_format_5_features_valid(output->feature_flags) ||
            (output->chroma_quality != 0 &&
             !(output->feature_flags &
               N148_FEATURE_CALIBRATED_CHROMA_QUANT)))
            return N148I_ERROR_INVALID_ARGUMENT;
        if (output->chroma == N148I_CHROMA_444)
            output->feature_flags &=
                ~N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA;
    } else if (output->format_version == N148I_FORMAT_VERSION_6) {
        if (output->feature_flags & ~N148_FORMAT_6_SUPPORTED_FEATURES)
            return N148I_ERROR_UNSUPPORTED_FEATURE;
        if (!n148_format_6_features_valid(output->feature_flags) ||
            (output->chroma_quality != 0 &&
             !(output->feature_flags &
               N148_FEATURE_CALIBRATED_CHROMA_QUANT)))
            return N148I_ERROR_INVALID_ARGUMENT;
        if (output->chroma == N148I_CHROMA_444)
            output->feature_flags &=
                ~N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA;
    } else if (output->format_version == N148I_FORMAT_VERSION_7) {
        /* These public placeholders never had an implementation. Preserve
           their rejection class after assigning bit 30 to a different profile. */
        if (output->feature_flags == N148I_PROFILE_DIRECTIONAL_SCAN ||
            output->feature_flags == N148I_PROFILE_DIRECTIONAL_TRANSFORM)
            return N148I_ERROR_UNSUPPORTED_FEATURE;
        if (output->feature_flags & ~N148_FORMAT_7_SUPPORTED_FEATURES)
            return N148I_ERROR_UNSUPPORTED_FEATURE;
        if (!n148_format_7_features_valid(output->feature_flags) ||
            (output->chroma_quality != 0 &&
             !(output->feature_flags &
               N148_FEATURE_CALIBRATED_CHROMA_QUANT)))
            return N148I_ERROR_INVALID_ARGUMENT;
        if (output->chroma == N148I_CHROMA_444)
            output->feature_flags &=
                ~N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA;
    }
    return N148I_OK;
}

void n148i_encode_options_init(n148i_encode_options_t *options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->quality = 50;
    options->chroma = N148I_CHROMA_420;
    options->optimize_huffman = 1;
    options->thread_count = 0;
    options->format_version = N148I_FORMAT_VERSION_7;
    options->feature_flags = N148I_DEFAULT_FEATURES;
    options->effort = 3;
    options->chroma_quality = 0;
}

static n148i_result_t encode_modern_complete(
    const n148i_image_t *image, const n148i_encode_options_t *settings,
    Plane *y, Plane *cb, Plane *cr,
    uint8_t **encoded, size_t *encoded_size) {
    N148EncodedPayload sections = {0};
    int ok;
    if (settings->format_version == N148I_FORMAT_VERSION_7) {
        int chroma_quality = effective_chroma_quality(settings);
        if (settings->feature_flags & N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT)
            n148_fidelity_rgb_reference(image->pixels,
                                        image->stride ? image->stride :
                                            (size_t)image->width * 3,
                                        (int)image->width, (int)image->height);
        ok = n148_encode_format_7(y, cb, cr, settings->quality, chroma_quality,
                            settings->feature_flags, settings->effort,
                            &sections);
        n148_fidelity_rgb_reference(NULL, 0, 0, 0);
    } else if (settings->format_version == N148I_FORMAT_VERSION_6) {
        int chroma_quality = effective_chroma_quality(settings);
        ok = n148_encode_format_6(y, cb, cr, settings->quality, chroma_quality,
                            settings->feature_flags, settings->effort,
                            &sections);
    } else if (settings->format_version == N148I_FORMAT_VERSION_5) {
        int chroma_quality = effective_chroma_quality(settings);
        ok = n148_encode_format_5(y, cb, cr, settings->quality, chroma_quality,
                            settings->feature_flags, settings->effort,
                            &sections);
    } else if (settings->format_version == N148I_FORMAT_VERSION_4) {
        int chroma_quality = effective_chroma_quality(settings);
        ok = n148_encode_format_4(y, cb, cr, settings->quality, chroma_quality,
                            settings->feature_flags, settings->effort,
                            &sections);
    } else if (settings->format_version == N148I_FORMAT_VERSION_3) {
        ok = n148_encode_format_3(y, cb, cr, settings->quality,
                            settings->feature_flags, settings->effort,
                            &sections);
    } else {
        ok = n148_encode_format_2(y, cb, cr, settings->quality,
                            settings->feature_flags, settings->effort,
                            &sections);
    }
    if (!ok) {
        return N148I_ERROR_ENCODE_FAILED;
    }
    if (sections.metadata_size > UINT16_MAX - N148_EXTENDED_HEADER_SIZE ||
        sections.metadata_size > UINT32_MAX ||
        sections.payload_size == 0 || sections.payload_size > UINT32_MAX ||
        sections.metadata_size > SIZE_MAX - N148_EXTENDED_HEADER_SIZE ||
        sections.payload_size > SIZE_MAX - N148_EXTENDED_HEADER_SIZE -
            sections.metadata_size) {
        n148_encoded_payload_release(&sections);
        return N148I_ERROR_SIZE_OVERFLOW;
    }

    size_t header_size = N148_EXTENDED_HEADER_SIZE + sections.metadata_size;
    size_t complete_size = header_size + sections.payload_size;
    uint8_t *complete = (uint8_t *) malloc(complete_size);
    if (!complete) {
        n148_encoded_payload_release(&sections);
        return N148I_ERROR_OUT_OF_MEMORY;
    }
    memcpy(complete, N148I_MAGIC, N148I_MAGIC_LEN);
    complete[5] = (uint8_t) settings->format_version;
    memory_write_u16(complete + 6, (uint16_t) header_size);
    memory_write_u32(complete + 8, image->width);
    memory_write_u32(complete + 12, image->height);
    memory_write_u32(complete + 16, (uint32_t) sections.payload_size);
    memory_write_u32(complete + 20, settings->feature_flags);
    complete[24] = (uint8_t) settings->quality;
    complete[25] = (uint8_t) settings->chroma;
    complete[26] = (uint8_t) settings->effort;
    complete[27] = (settings->format_version == N148I_FORMAT_VERSION_4 ||
                    settings->format_version == N148I_FORMAT_VERSION_5 ||
                    settings->format_version == N148I_FORMAT_VERSION_6 ||
                    settings->format_version == N148I_FORMAT_VERSION_7) &&
        (settings->feature_flags &
         N148_FEATURE_CALIBRATED_CHROMA_QUANT) ?
        (uint8_t) effective_chroma_quality(settings) : 0;
    memory_write_u32(complete + 28, (uint32_t) sections.metadata_size);
    memcpy(complete + N148_EXTENDED_HEADER_SIZE,
           sections.metadata, sections.metadata_size);
    memcpy(complete + header_size, sections.payload, sections.payload_size);
    n148_encoded_payload_release(&sections);
    *encoded = complete;
    *encoded_size = complete_size;
    return N148I_OK;
}

static n148i_result_t encode_chroma_layout(
    Image *source, const n148i_image_t *public_image,
    const n148i_encode_options_t *settings, n148i_chroma_t chroma,
    uint8_t **encoded, size_t *encoded_size) {
    n148i_encode_options_t concrete = *settings;
    concrete.chroma = chroma;
    if (chroma == N148I_CHROMA_444)
        concrete.feature_flags &=
            ~N148_FEATURE_RECONSTRUCTION_AWARE_CHROMA;
    int reconstruction_aware =
        (concrete.feature_flags &
         N148I_FEATURE_RECONSTRUCTION_AWARE_CHROMA) != 0;
    Plane y = {0}, cb = {0}, cr = {0};
    int ok = n148_split_channels_reconstruction_aware(
        source, &y, &cb, &cr, chroma, reconstruction_aware);
    n148i_result_t result = N148I_ERROR_ENCODE_FAILED;
    if (ok)
        result = encode_modern_complete(
            public_image, &concrete, &y, &cb, &cr, encoded, encoded_size);
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    return result;
}

/* The image-level objective is the same SSE + lambda * bits form used by the
   block RDO. RGB SSE is divided by three to keep it on a one-plane scale;
   multiplying the complete expression by 300 avoids a division here. */
static long double automatic_chroma_cost(
    const Image *source, const n148i_image_t *decoded, size_t encoded_size,
    const n148i_encode_options_t *settings) {
    if (!source || !source->pixels || !decoded || !decoded->pixels ||
        decoded->width != (uint32_t) source->width ||
        decoded->height != (uint32_t) source->height)
        return LDBL_MAX;
    long double squared_error = 0.0L;
    size_t value_count = (size_t) source->width * source->height * 3u;
    for (size_t index = 0; index < value_count; index++) {
        int difference =
            (int) source->pixels[index] - (int) decoded->pixels[index];
        squared_error += (long double) difference * difference;
    }
    int quant[8][8];
    const int (*base)[8] =
        !(settings->feature_flags & N148I_FEATURE_CALIBRATED_LUMA_QUANT) ?
        Q_LUMA_BASE :
        (settings->format_version == N148I_FORMAT_VERSION_7 &&
         (settings->feature_flags & N148I_FORMAT_7_BALANCED_RECONSTRUCTION)) ?
        N148_BALANCED_LUMA_QUANT_BASE :
        (settings->format_version == N148I_FORMAT_VERSION_7 &&
         (settings->feature_flags & N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT)) ?
        N148_STRUCTURAL_LUMA_QUANT_BASE :
        (settings->format_version == N148I_FORMAT_VERSION_7 &&
         (settings->feature_flags &
          N148I_FORMAT_7_REFINED_SPECTRAL_LUMA_QUANT)) ?
        N148_REFINED_SPECTRAL_LUMA_QUANT_BASE :
        (settings->format_version == N148I_FORMAT_VERSION_7 &&
         (settings->feature_flags & N148I_FORMAT_7_SPECTRAL_LUMA_QUANT)) ?
        N148_SPECTRAL_LUMA_QUANT_BASE : FORMAT_5_LUMA_QUANT_BASE;
    scale_table(base, settings->quality, quant);
    long double step = quant[0][0];
    long double bits = (long double) encoded_size * 8.0L;
    return 100.0L * squared_error + 255.0L * step * step * bits;
}

static n148i_result_t encode_automatic_chroma(
    Image *source, const n148i_image_t *public_image,
    const n148i_encode_options_t *settings,
    uint8_t **encoded, size_t *encoded_size) {
    if (settings->effort == 0)
        return encode_chroma_layout(
            source, public_image, settings, N148I_CHROMA_420,
            encoded, encoded_size);

    static const n148i_chroma_t layouts[] = {
        N148I_CHROMA_420, N148I_CHROMA_422, N148I_CHROMA_444,
    };
    uint8_t *best = NULL;
    size_t best_size = 0;
    long double best_cost = LDBL_MAX;
    for (size_t index = 0; index < sizeof(layouts) / sizeof(layouts[0]);
         index++) {
        uint8_t *candidate = NULL;
        size_t candidate_size = 0;
        n148i_result_t result = encode_chroma_layout(
            source, public_image, settings, layouts[index],
            &candidate, &candidate_size);
        if (result != N148I_OK) {
            free(best);
            return result;
        }
        n148i_image_t decoded = {0};
        result = n148i_decode_memory(candidate, candidate_size, &decoded);
        if (result != N148I_OK) {
            free(candidate);
            free(best);
            return result;
        }
        long double cost = automatic_chroma_cost(
            source, &decoded, candidate_size, settings);
        n148i_free_image(&decoded);
        if (!best || cost < best_cost ||
            (cost == best_cost && candidate_size < best_size)) {
            free(best);
            best = candidate;
            best_size = candidate_size;
            best_cost = cost;
        } else {
            free(candidate);
        }
    }
    if (!best) return N148I_ERROR_ENCODE_FAILED;
    *encoded = best;
    *encoded_size = best_size;
    return N148I_OK;
}

n148i_result_t n148i_encode_memory(const n148i_image_t *image,
                                   const n148i_encode_options_t *options,
                                   uint8_t **encoded,
                                   size_t *encoded_size) {
    if (!image || !image->pixels || !encoded || !encoded_size ||
        image->width == 0 || image->width > INT_MAX ||
        image->height == 0 || image->height > INT_MAX) {
        return N148I_ERROR_INVALID_ARGUMENT;
    }
    *encoded = NULL;
    *encoded_size = 0;

    n148i_encode_options_t settings;
    n148i_result_t result = normalize_options(options, &settings);
    if (result != N148I_OK) return result;

    size_t width = image->width;
    size_t height = image->height;
    if (multiplication_overflows(width, 3))
        return N148I_ERROR_SIZE_OVERFLOW;
    size_t tight_stride = width * 3;
    size_t source_stride = image->stride ? image->stride : tight_stride;
    if (source_stride < tight_stride ||
        (height > 1 &&
         source_stride > (SIZE_MAX - tight_stride) / (height - 1)) ||
        multiplication_overflows(tight_stride, height) ||
        multiplication_overflows(width, height)) {
        return N148I_ERROR_SIZE_OVERFLOW;
    }
    size_t pixel_count = width * height;
#if LONG_MAX < SIZE_MAX
    if (pixel_count > (size_t) LONG_MAX)
        return N148I_ERROR_SIZE_OVERFLOW;
#endif

    uint8_t *compact = NULL;
    Image internal_image;
    internal_image.width = (int) image->width;
    internal_image.height = (int) image->height;
    internal_image.pixels = image->pixels;
    if (source_stride != tight_stride) {
        compact = (uint8_t *) malloc(tight_stride * height);
        if (!compact) return N148I_ERROR_OUT_OF_MEMORY;
        for (size_t row = 0; row < height; row++) {
            memcpy(compact + row * tight_stride,
                   image->pixels + row * source_stride, tight_stride);
        }
        internal_image.pixels = compact;
    }

    if (settings.thread_count > 0)
        n148_set_thread_count(settings.thread_count);

    if (settings.chroma == N148I_CHROMA_AUTO) {
        result = encode_automatic_chroma(
            &internal_image, image, &settings, encoded, encoded_size);
        free(compact);
        return result;
    }

    Plane y = {0}, cb = {0}, cr = {0};
    HuffSpec specs[HUFFMAN_TABLE_COUNT];
    EncodeStats stats;
    uint8_t *payload = NULL;
    int perceptual_color = settings.format_version == N148I_FORMAT_VERSION_2 &&
        (settings.feature_flags & N148I_FEATURE_PERCEPTUAL_COLOR) != 0;
    int reconstruction_aware =
        (settings.format_version == N148I_FORMAT_VERSION_4 ||
         settings.format_version == N148I_FORMAT_VERSION_5 ||
         settings.format_version == N148I_FORMAT_VERSION_6 ||
         settings.format_version == N148I_FORMAT_VERSION_7) &&
        (settings.feature_flags &
         N148I_FEATURE_RECONSTRUCTION_AWARE_CHROMA) != 0;
    int ok = (settings.format_version == N148I_FORMAT_VERSION_4 ||
              settings.format_version == N148I_FORMAT_VERSION_5 ||
              settings.format_version == N148I_FORMAT_VERSION_6 ||
              settings.format_version == N148I_FORMAT_VERSION_7) ?
        n148_split_channels_reconstruction_aware(&internal_image, &y, &cb, &cr,
                               settings.chroma, reconstruction_aware) :
        n148_split_channels_ex(&internal_image, &y, &cb, &cr,
                               settings.chroma, perceptual_color);
    free(compact);
    if (!ok) {
        free_plane(&y);
        free_plane(&cb);
        free_plane(&cr);
        return N148I_ERROR_ENCODE_FAILED;
    }
    if (settings.format_version == N148I_FORMAT_VERSION_2 ||
        settings.format_version == N148I_FORMAT_VERSION_3 ||
        settings.format_version == N148I_FORMAT_VERSION_4 ||
        settings.format_version == N148I_FORMAT_VERSION_5 ||
        settings.format_version == N148I_FORMAT_VERSION_6 ||
        settings.format_version == N148I_FORMAT_VERSION_7) {
        result = encode_modern_complete(image, &settings, &y, &cb, &cr,
                                        encoded, encoded_size);
        free_plane(&y);
        free_plane(&cb);
        free_plane(&cr);
        return result;
    }
    if (ok) {
        ok = encode_image(&y, &cb, &cr, settings.quality,
                          settings.optimize_huffman, specs, &payload, &stats);
    }
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    if (!ok) {
        free(payload);
        return N148I_ERROR_ENCODE_FAILED;
    }

    if (stats.data_size <= 0 || stats.data_size > UINT32_MAX ||
        stats.table_size < 0) {
        free(payload);
        return N148I_ERROR_SIZE_OVERFLOW;
    }
#if LONG_MAX > UINT32_MAX
    if ((unsigned long) stats.data_size > UINT32_MAX) {
        free(payload);
        return N148I_ERROR_SIZE_OVERFLOW;
    }
#endif
    size_t table_size = (size_t) stats.table_size;
    size_t payload_size = (size_t) stats.data_size;
    if (table_size > SIZE_MAX - N148_FORMAT_1_HEADER_SIZE ||
        payload_size > SIZE_MAX - N148_FORMAT_1_HEADER_SIZE - table_size) {
        free(payload);
        return N148I_ERROR_SIZE_OVERFLOW;
    }
    size_t complete_size =
        N148_FORMAT_1_HEADER_SIZE + table_size + payload_size;
    uint8_t *complete = (uint8_t *) malloc(complete_size);
    if (!complete) {
        free(payload);
        return N148I_ERROR_OUT_OF_MEMORY;
    }

    memcpy(complete, N148I_MAGIC, N148I_MAGIC_LEN);
    complete[5] = N148_FORMAT_VERSION_1;
    memory_write_u32(complete + 6, image->width);
    memory_write_u32(complete + 10, image->height);
    complete[14] = (uint8_t) settings.quality;
    complete[15] = (uint8_t) settings.chroma;
    complete[16] = (uint8_t) settings.optimize_huffman;
    memory_write_u32(complete + 17, (uint32_t) stats.data_size);

    size_t offset = N148_FORMAT_1_HEADER_SIZE;
    if (settings.optimize_huffman) {
        for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
            if (!huffman_spec_is_valid(&specs[table])) {
                free(payload);
                free(complete);
                return N148I_ERROR_ENCODE_FAILED;
            }
            for (int length = 1; length <= 16; length++)
                complete[offset++] = (uint8_t) specs[table].bits[length];
            memcpy(complete + offset, specs[table].values,
                   (size_t) specs[table].value_count);
            offset += (size_t) specs[table].value_count;
        }
    }
    if (offset != N148_FORMAT_1_HEADER_SIZE + table_size) {
        free(payload);
        free(complete);
        return N148I_ERROR_ENCODE_FAILED;
    }
    memcpy(complete + offset, payload, payload_size);
    free(payload);

    *encoded = complete;
    *encoded_size = complete_size;
    return N148I_OK;
}

n148i_result_t n148i_decode_memory(const uint8_t *encoded,
                                   size_t encoded_size,
                                   n148i_image_t *image) {
    if (!image) return N148I_ERROR_INVALID_ARGUMENT;
    ParsedHeader parsed;
    n148i_result_t result = parse_header(encoded, encoded_size, &parsed);
    if (result != N148I_OK) return result;

    size_t width = parsed.width;
    size_t height = parsed.height;
    if (multiplication_overflows(width, 3) ||
        multiplication_overflows(width * 3, height) ||
        multiplication_overflows(width, height)) {
        return N148I_ERROR_SIZE_OVERFLOW;
    }
    size_t pixel_count = width * height;
#if LONG_MAX < SIZE_MAX
    if (pixel_count > (size_t) LONG_MAX)
        return N148I_ERROR_SIZE_OVERFLOW;
#endif

    Plane y = {0}, cb = {0}, cr = {0};
    DecodeStats stats;
    Image decoded = {0};
    int ok;
    if (parsed.version == N148I_FORMAT_VERSION_1) {
        ok = decode_image((uint8_t *) encoded + parsed.payload_offset,
                          (long) parsed.payload_size,
                          (int) parsed.width, (int) parsed.height,
                          parsed.quality, parsed.chroma, parsed.specs,
                          &y, &cb, &cr, &stats);
        if (ok && stats.bytes_consumed != (long) parsed.payload_size)
            ok = 0;
    } else if (parsed.version == N148I_FORMAT_VERSION_2) {
        ok = n148_decode_format_2(encoded + parsed.metadata_offset,
                            parsed.metadata_size,
                            encoded + parsed.payload_offset,
                            parsed.payload_size,
                            (int) parsed.width, (int) parsed.height,
                            parsed.quality, parsed.chroma,
                            parsed.feature_flags, &y, &cb, &cr);
    } else if (parsed.version == N148I_FORMAT_VERSION_3) {
        ok = n148_decode_format_3(encoded + parsed.metadata_offset,
                            parsed.metadata_size,
                            encoded + parsed.payload_offset,
                            parsed.payload_size,
                            (int) parsed.width, (int) parsed.height,
                            parsed.quality, parsed.chroma,
                            parsed.feature_flags, &y, &cb, &cr);
    } else if (parsed.version == N148I_FORMAT_VERSION_4) {
        ok = n148_decode_format_4(encoded + parsed.metadata_offset,
                            parsed.metadata_size,
                            encoded + parsed.payload_offset,
                            parsed.payload_size,
                            (int) parsed.width, (int) parsed.height,
                            parsed.quality, parsed.chroma_quality,
                            parsed.chroma,
                            parsed.feature_flags, &y, &cb, &cr);
    } else if (parsed.version == N148I_FORMAT_VERSION_5) {
        ok = n148_decode_format_5(encoded + parsed.metadata_offset,
                            parsed.metadata_size,
                            encoded + parsed.payload_offset,
                            parsed.payload_size,
                            (int) parsed.width, (int) parsed.height,
                            parsed.quality, parsed.chroma_quality,
                            parsed.chroma,
                            parsed.feature_flags, &y, &cb, &cr);
    } else if (parsed.version == N148I_FORMAT_VERSION_6) {
        ok = n148_decode_format_6(encoded + parsed.metadata_offset,
                            parsed.metadata_size,
                            encoded + parsed.payload_offset,
                            parsed.payload_size,
                            (int) parsed.width, (int) parsed.height,
                            parsed.quality, parsed.chroma_quality,
                            parsed.chroma,
                            parsed.feature_flags, &y, &cb, &cr);
    } else {
        ok = n148_decode_format_7(encoded + parsed.metadata_offset,
                            parsed.metadata_size,
                            encoded + parsed.payload_offset,
                            parsed.payload_size,
                            (int) parsed.width, (int) parsed.height,
                            parsed.quality, parsed.chroma_quality,
                            parsed.chroma,
                            parsed.feature_flags, &y, &cb, &cr);
    }
    int perceptual_color = parsed.version == N148I_FORMAT_VERSION_2 &&
        (parsed.feature_flags & N148I_FEATURE_PERCEPTUAL_COLOR) != 0;
    if (ok) ok = n148_merge_channels_ex(&y, &cb, &cr, 1,
                                         perceptual_color, &decoded);
    free_plane(&y);
    free_plane(&cb);
    free_plane(&cr);
    if (!ok) {
        free_image(&decoded);
        return N148I_ERROR_DECODE_FAILED;
    }
    if (decoded.width != (int) parsed.width ||
        decoded.height != (int) parsed.height || !decoded.pixels) {
        free_image(&decoded);
        return N148I_ERROR_CORRUPT_DATA;
    }

    image->pixels = decoded.pixels;
    image->width = parsed.width;
    image->height = parsed.height;
    image->stride = width * 3;
    return N148I_OK;
}

n148i_result_t n148i_read_header(const uint8_t *encoded,
                                 size_t encoded_size,
                                 n148i_image_info_t *info) {
    if (!info) return N148I_ERROR_INVALID_ARGUMENT;
    ParsedHeader parsed;
    n148i_result_t result = parse_header(encoded, encoded_size, &parsed);
    if (result != N148I_OK) return result;

    info->width = parsed.width;
    info->height = parsed.height;
    info->payload_size = parsed.payload_size;
    info->encoded_header_size = parsed.payload_offset;
    info->format_version = parsed.version;
    info->quality = parsed.quality;
    info->chroma_quality = parsed.chroma_quality;
    info->chroma = (n148i_chroma_t) parsed.chroma;
    info->optimized_huffman = parsed.optimized;
    info->feature_flags = parsed.feature_flags;
    info->effort = parsed.effort;
    info->metadata_size = parsed.metadata_size;
    return N148I_OK;
}

void n148i_free_buffer(void *buffer) {
    free(buffer);
}

void n148i_free_image(n148i_image_t *image) {
    if (!image) return;
    free(image->pixels);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;
    image->stride = 0;
}

const char *n148i_result_string(n148i_result_t result) {
    switch (result) {
        case N148I_OK: return "success";
        case N148I_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case N148I_ERROR_OUT_OF_MEMORY: return "out of memory";
        case N148I_ERROR_INVALID_FORMAT: return "invalid N.148i format";
        case N148I_ERROR_UNSUPPORTED_VERSION: return "unsupported format version";
        case N148I_ERROR_TRUNCATED_DATA: return "truncated data";
        case N148I_ERROR_CORRUPT_DATA: return "corrupt data";
        case N148I_ERROR_SIZE_OVERFLOW: return "image or buffer is too large";
        case N148I_ERROR_ENCODE_FAILED: return "encoding failed";
        case N148I_ERROR_DECODE_FAILED: return "decoding failed";
        case N148I_ERROR_UNSUPPORTED_SIMD: return "SIMD level is not supported";
        case N148I_ERROR_UNSUPPORTED_FEATURE: return "unsupported format feature";
        default: return "unknown N.148i error";
    }
}

const char *n148i_library_version(void) {
    return N148I_VERSION_STRING;
}

uint32_t n148i_format_version(void) {
    return N148I_FORMAT_VERSION;
}

n148i_simd_level_t n148i_simd_level(void) {
    return (n148i_simd_level_t) n148_cpu_level();
}

const char *n148i_simd_name(n148i_simd_level_t level) {
    switch (level) {
        case N148I_SIMD_AUTO: return "automatic";
        case N148I_SIMD_SCALAR: return "scalar";
        case N148I_SIMD_SSE2: return "SSE2";
        case N148I_SIMD_AVX2: return "AVX2";
        case N148I_SIMD_AVX2_FMA: return "AVX2+FMA";
        default: return "unknown";
    }
}

n148i_result_t n148i_simd_force(n148i_simd_level_t level) {
    if (level == N148I_SIMD_AUTO) {
        n148_cpu_force(-1);
        return N148I_OK;
    }
    if (level < N148I_SIMD_SCALAR || level > N148I_SIMD_AVX2_FMA)
        return N148I_ERROR_INVALID_ARGUMENT;
    if ((int) level > n148_cpu_detected_level())
        return N148I_ERROR_UNSUPPORTED_SIMD;
    n148_cpu_force((int) level);
    return N148I_OK;
}

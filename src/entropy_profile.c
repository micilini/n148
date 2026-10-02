/*
 * N.148i adaptive-entropy profile codec
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include "entropy_profile.h"

#include "predictive_codec.h"
#include "directional_intra.h"

static uint32_t fidelity_features(uint32_t features) {
    return features & N148_FORMAT_6_SUPPORTED_FEATURES;
}

static uint32_t predictive_features(uint32_t features) {
    return features & N148_FORMAT_3_SUPPORTED_FEATURES;
}

static uint32_t fidelity_tools(uint32_t features) {
    uint32_t tools = 0;
    if (features & N148_FEATURE_FILTERED_INTRA_REFERENCES)
        tools |= N148_FIDELITY_FILTER_REFERENCES;
    if (features & N148_FEATURE_PLANAR_INTRA_PREDICTION)
        tools |= N148_FIDELITY_PLANAR_PREDICTION;
    if (features & N148_FEATURE_STRUCTURAL_RDO)
        tools |= N148_FIDELITY_STRUCTURAL_RDO;
    if (features & N148_FEATURE_QUALITY_LAMBDA)
        tools |= N148_FIDELITY_QUALITY_LAMBDA;
    if (features & N148_FEATURE_LUMA_TRANSFORM_32X32)
        tools |= N148_FIDELITY_DCT32;
    if (features & N148_FEATURE_SECOND_ORDER_DC)
        tools |= N148_FIDELITY_SECOND_ORDER_DC;
    if (features & N148_FEATURE_FINE_INTRA_DIRECTIONS)
        tools |= N148_FIDELITY_FINE_DIRECTIONS;
    if (features & N148_FEATURE_MULTIPLE_INTRA_REFERENCES)
        tools |= N148_FIDELITY_MULTIPLE_REFERENCES;
    if (features & N148_FEATURE_SPECTRAL_LUMA_QUANT)
        tools |= N148_FIDELITY_SPECTRAL_LUMA_QUANT;
    if (features & N148_FEATURE_REFINED_SPECTRAL_LUMA_QUANT)
        tools |= N148_FIDELITY_REFINED_SPECTRAL_LUMA_QUANT;
    if (features & N148_FEATURE_STRUCTURAL_LUMA_QUANT)
        tools |= N148_FIDELITY_STRUCTURAL_LUMA_QUANT;
    if (features & N148_FEATURE_DETAIL_RECONSTRUCTION)
        tools |= N148_FIDELITY_DETAIL_RECONSTRUCTION;
    if (features & N148_FEATURE_BALANCED_RECONSTRUCTION)
        tools |= N148_FIDELITY_BALANCED_RECONSTRUCTION;
    return tools;
}

static uint32_t entropy_tools(uint32_t features) {
    uint32_t tools = 0;
    if (features & N148_FEATURE_ADAPTIVE_ENTROPY)
        tools |= N148_ENTROPY_ADAPTIVE;
    if (features & N148_FEATURE_RICH_COEFFICIENT_CONTEXTS)
        tools |= N148_ENTROPY_RICH_CONTEXT;
    if (features & N148_FEATURE_MULTIPLE_INTRA_REFERENCES)
        tools |= N148_ENTROPY_MULTIPLE_REFERENCES;
    return tools;
}

int n148_format_7_features_valid(uint32_t features) {
    if (features & ~N148_FORMAT_7_SUPPORTED_FEATURES) return 0;
    if (!n148_format_6_features_valid(fidelity_features(features))) return 0;
    if ((features & N148_FEATURE_ADAPTIVE_ENTROPY) &&
        (!(features & N148_FEATURE_RANS) ||
         !(features & N148_FEATURE_CONTEXT))) return 0;
    if ((features & N148_FEATURE_RICH_COEFFICIENT_CONTEXTS) &&
        (!(features & N148_FEATURE_RANS) ||
         !(features & N148_FEATURE_CONTEXT) ||
         !(features & N148_FEATURE_INTRA) ||
         !(features & N148_FEATURE_ADAPTIVE_ENTROPY))) return 0;
    if ((features & N148_FEATURE_MULTIPLE_INTRA_REFERENCES) &&
        (!(features & N148_FEATURE_RICH_COEFFICIENT_CONTEXTS) ||
         !(features & N148_FEATURE_CONTEXTUAL_RDO) ||
         !(features & N148_FEATURE_DIRECTIONAL_INTRA))) return 0;
    if ((features & N148_FEATURE_SPECTRAL_LUMA_QUANT) &&
        !(features & N148_FEATURE_CALIBRATED_LUMA_QUANT)) return 0;
    if ((features & N148_FEATURE_REFINED_SPECTRAL_LUMA_QUANT) &&
        !(features & N148_FEATURE_SPECTRAL_LUMA_QUANT)) return 0;
    if ((features & N148_FEATURE_BALANCED_RECONSTRUCTION) &&
        !(features & N148_FEATURE_STRUCTURAL_LUMA_QUANT)) return 0;
    if ((features & N148_FEATURE_DETAIL_RECONSTRUCTION) &&
        !(features & N148_FEATURE_BALANCED_RECONSTRUCTION)) return 0;
    if (features & N148_FEATURE_STRUCTURAL_LUMA_QUANT) {
        const uint32_t required = N148_FEATURE_REFINED_SPECTRAL_LUMA_QUANT |
            N148_FEATURE_SEGMENTATION | N148_FEATURE_CONTEXTUAL_RDO |
            N148_FEATURE_CALIBRATED_CHROMA_QUANT |
            N148_FEATURE_LUMA_TRANSFORM_16X16;
        if ((features & required) != required) return 0;
    }
    return 1;
}

int n148_encode_format_7(Plane *y, Plane *cb, Plane *cr, int quality,
                   int chroma_quality, uint32_t features, int effort,
                   N148EncodedPayload *output) {
    if (!y || !cb || !cr || !output || quality < 1 || quality > 100 ||
        chroma_quality < 1 || chroma_quality > 100 || effort < 0 ||
        effort > 9 || !n148_format_7_features_valid(features)) return 0;
    int joint_chroma =
        (features & N148_FEATURE_JOINT_CHROMA_INTRA) != 0;
    int calibrated_chroma =
        (features & N148_FEATURE_CALIBRATED_CHROMA_QUANT) != 0;
    int segmented = (features & N148_FEATURE_SEGMENTATION) != 0;
    int adaptive_filter =
        (features & N148_FEATURE_ADAPTIVE_LOOP_FILTER) != 0;
    int calibrated_luma =
        (features & N148_FEATURE_CALIBRATED_LUMA_QUANT) != 0;
    int variable_luma =
        (features & N148_FEATURE_LUMA_TRANSFORM_16X16) != 0;
    int quant_adaptive_filter =
        (features & N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER) != 0;
    int dct32 =
        (features & N148_FEATURE_LUMA_TRANSFORM_32X32) != 0;
    if (!calibrated_chroma) chroma_quality = quality;
    uint32_t encoder_tools = fidelity_tools(features);
    /* Efforts 3 and 4 use the measured four-mode shortlist. Effort 4 may
       revisit the runner-up on difficult luma blocks. */
    if ((effort == 3 || effort == 4) &&
        (features & N148_FEATURE_RICH_COEFFICIENT_CONTEXTS))
        encoder_tools |= N148_FIDELITY_FAST_MODE_SEARCH;
    if (!n148_fidelity_quality_set(quality) ||
        !n148_fidelity_tools_set(encoder_tools)) return 0;
    uint32_t entropy = entropy_tools(features);
    int success;
    if ((entropy & N148_ENTROPY_RICH_CONTEXT) && effort < 5) {
        /* The accepted maximum-compression path compares two complete
           representations. Lower effort levels retain exact format 6 entropy and
           avoid paying that search cost. The format 7 metadata-revision fallback
           makes the choice self-describing. */
        success = n148_predictive_encode(
            y, cb, cr, quality, chroma_quality, predictive_features(features), effort,
            joint_chroma, calibrated_chroma, segmented, adaptive_filter,
            calibrated_luma, variable_luma, 1, quant_adaptive_filter, dct32,
            entropy & N148_ENTROPY_MULTIPLE_REFERENCES, output);
    } else if (entropy & N148_ENTROPY_RICH_CONTEXT) {
        N148EncodedPayload rich = {0};
        N148EncodedPayload legacy = {0};
        N148PredictivePreparation prepared = {0};
        success = n148_predictive_encode_cached(
            y, cb, cr, quality, chroma_quality, predictive_features(features), effort,
            joint_chroma, calibrated_chroma, segmented, adaptive_filter,
            calibrated_luma, variable_luma, 1, quant_adaptive_filter, dct32,
            entropy, &rich, &prepared) &&
            n148_predictive_encode_cached(
                y, cb, cr, quality, chroma_quality, predictive_features(features),
                effort, joint_chroma, calibrated_chroma, segmented,
                adaptive_filter, calibrated_luma, variable_luma, 1,
                quant_adaptive_filter, dct32,
                entropy & N148_ENTROPY_MULTIPLE_REFERENCES, &legacy, &prepared);
        if (success) {
            int rich_overflow = rich.metadata_size >
                SIZE_MAX - rich.payload_size;
            int legacy_overflow = legacy.metadata_size >
                SIZE_MAX - legacy.payload_size;
            if (rich_overflow || legacy_overflow) {
                success = 0;
            } else if (legacy.metadata_size + legacy.payload_size <
                       rich.metadata_size + rich.payload_size) {
                *output = legacy;
                legacy = (N148EncodedPayload){0};
            } else {
                *output = rich;
                rich = (N148EncodedPayload){0};
            }
        }
        n148_encoded_payload_release(&rich);
        n148_encoded_payload_release(&legacy);
        n148_predictive_preparation_release(&prepared);
    } else {
        success = n148_predictive_encode(
            y, cb, cr, quality, chroma_quality, predictive_features(features), effort,
            joint_chroma, calibrated_chroma, segmented, adaptive_filter,
            calibrated_luma, variable_luma, 1, quant_adaptive_filter, dct32,
            entropy, output);
    }
    n148_fidelity_tools_set(0);
    n148_fidelity_quality_set(50);
    return success;
}

/* Balanced output reconstruction: mix each stored chroma sample with
   one quarter of its clamped 3x3 mean. All rows read this stage's
   unmodified samples; prediction references are already complete. */
#include "cpu.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
__attribute__((target("avx2")))
static int balanced_smooth_row(const unsigned char *a,const unsigned char *b,
    const unsigned char *c,unsigned char *output,int width,int shift) {
    int x=1;
    const int half=(9<<shift)/2;
    __m128i count=_mm_cvtsi32_si128(shift);
    for(;x<width-16;x+=16) {
        __m256i center=_mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(b+x)));
        __m256i delta=_mm256_sub_epi16(_mm256_setzero_si256(),_mm256_slli_epi16(center,3));
        for(int k=-1;k<=1;++k) {
            delta=_mm256_add_epi16(delta,_mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(a+x+k))));
            delta=_mm256_add_epi16(delta,_mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(c+x+k))));
        }
        delta=_mm256_add_epi16(delta,_mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(b+x-1))));
        delta=_mm256_add_epi16(delta,_mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(b+x+1))));
        __m256i negative=_mm256_cmpgt_epi16(_mm256_setzero_si256(),delta);
        __m256i magnitude=_mm256_add_epi16(_mm256_abs_epi16(delta),_mm256_set1_epi16((short)half));
        magnitude=_mm256_sub_epi16(magnitude,_mm256_and_si256(negative,_mm256_set1_epi16(1)));
        /* Numerator <= 3192: this reciprocal is exact over the used range. */
        __m256i correction=_mm256_srl_epi16(_mm256_mulhi_epu16(magnitude,_mm256_set1_epi16(7282)),count);
        correction=_mm256_sign_epi16(correction,delta);
        __m256i value=_mm256_add_epi16(center,correction);
        _mm_storeu_si128((__m128i *)(output+x),_mm_packus_epi16(
            _mm256_castsi256_si128(value),_mm256_extracti128_si256(value,1)));
    }
    return x;
}
#endif

static unsigned char balanced_smooth_pixel(const unsigned char *a,
    const unsigned char *b,const unsigned char *c,int x,int width,int shift) {
    int left=x ? x-1 : x,right=x+1<width ? x+1 : x;
    int delta=a[left]+a[x]+a[right]+b[left]+b[right]+c[left]+c[x]+c[right]-8*b[x];
    int denominator=9<<shift;
    int correction=delta>=0 ? (delta+denominator/2)/denominator :
                             -((-delta+denominator/2-1)/denominator);
    return (unsigned char)(b[x]+correction);
}

static int balanced_smooth_chroma(Plane *plane,int quality) {
    if(!plane || !plane->data || plane->width<=0 || plane->height<=0)return 0;
    int shift=2;
    size_t width=(size_t)plane->width;
    if(width>SIZE_MAX/3)return 0;
    unsigned char *scratch=malloc(width*3);
    if(!scratch)return 0;
    unsigned char *a=scratch,*b=a+width,*c=b+width;
    memcpy(a,plane->data,width);memcpy(b,plane->data,width);
    memcpy(c,plane->data+(plane->height>1 ? width : 0),width);
    int vector=0;
#if defined(__x86_64__) || defined(__i386__)
    vector=n148_cpu_level()>=N148_CPU_AVX2;
#endif
    for(int y=0;y<plane->height;++y) {
        unsigned char *output=plane->data+(size_t)y*width;
        output[0]=balanced_smooth_pixel(a,b,c,0,plane->width,shift);
        int x=1;
#if defined(__x86_64__) || defined(__i386__)
        if(vector)x=balanced_smooth_row(a,b,c,output,plane->width,shift);
#endif
        for(;x<plane->width;++x)output[x]=balanced_smooth_pixel(a,b,c,x,plane->width,shift);
        if(y+1<plane->height) {
            unsigned char *old=a;a=b;b=c;c=old;
            int next=y+2<plane->height ? y+2 : plane->height-1;
            memcpy(c,plane->data+(size_t)next*width,width);
        }
    }
    free(scratch);
    return 1;
}

int n148_decode_format_7(const uint8_t *metadata, size_t metadata_size,
                   const uint8_t *payload, size_t payload_size,
                   int width, int height, int quality, int chroma_quality,
                   int chroma, uint32_t features,
                   Plane *y, Plane *cb, Plane *cr) {
    if (!n148_format_7_features_valid(features) || chroma_quality < 1 ||
        chroma_quality > 100) return 0;
    int joint_chroma =
        (features & N148_FEATURE_JOINT_CHROMA_INTRA) != 0;
    int calibrated_chroma =
        (features & N148_FEATURE_CALIBRATED_CHROMA_QUANT) != 0;
    int segmented = (features & N148_FEATURE_SEGMENTATION) != 0;
    int adaptive_filter =
        (features & N148_FEATURE_ADAPTIVE_LOOP_FILTER) != 0;
    int calibrated_luma =
        (features & N148_FEATURE_CALIBRATED_LUMA_QUANT) != 0;
    int variable_luma =
        (features & N148_FEATURE_LUMA_TRANSFORM_16X16) != 0;
    int quant_adaptive_filter =
        (features & N148_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER) != 0;
    int dct32 =
        (features & N148_FEATURE_LUMA_TRANSFORM_32X32) != 0;
    if (!calibrated_chroma) chroma_quality = quality;
    if (!n148_fidelity_tools_set(fidelity_tools(features))) return 0;
    int success = n148_predictive_decode(
        metadata, metadata_size, payload, payload_size,
        width, height, quality, chroma_quality, chroma,
        predictive_features(features), joint_chroma, calibrated_chroma, segmented,
        adaptive_filter, calibrated_luma, variable_luma,
        quant_adaptive_filter, dct32, entropy_tools(features), y, cb, cr);
    if (success && (features & N148_FEATURE_BALANCED_RECONSTRUCTION))
        success = balanced_smooth_chroma(cb, chroma_quality) &&
            balanced_smooth_chroma(cr, chroma_quality);
    n148_fidelity_tools_set(0);
    return success;
}

int n148_inspect_format_7(const uint8_t *metadata, size_t metadata_size,
                    const uint8_t *payload, size_t payload_size,
                    int width, int height, int chroma, uint32_t features,
                    N148SyntaxInspection *inspection) {
    if (!n148_format_7_features_valid(features)) return 0;
    int joint_chroma =
        (features & N148_FEATURE_JOINT_CHROMA_INTRA) != 0;
    int segmented = (features & N148_FEATURE_SEGMENTATION) != 0;
    int adaptive_filter =
        (features & N148_FEATURE_ADAPTIVE_LOOP_FILTER) != 0;
    int variable_luma =
        (features & N148_FEATURE_LUMA_TRANSFORM_16X16) != 0;
    int dct32 =
        (features & N148_FEATURE_LUMA_TRANSFORM_32X32) != 0;
    if (!n148_fidelity_tools_set(fidelity_tools(features))) return 0;
    int success = n148_entropy_profile_inspect(
        metadata, metadata_size, payload, payload_size,
        width, height, chroma, predictive_features(features), joint_chroma,
        segmented, adaptive_filter, variable_luma, dct32,
        entropy_tools(features), inspection);
    n148_fidelity_tools_set(0);
    return success;
}

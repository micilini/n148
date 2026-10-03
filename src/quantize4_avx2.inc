#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static void residual4_avx2(const Plane *source, int x0, int y0,
    const uint8_t prediction[16], int32_t residual[16]) {
    for (int row=0;row<4;row++) {
        uint32_t src,pred;
        memcpy(&src,source->data+(long)(y0+row)*source->width+x0,4);
        memcpy(&pred,prediction+row*4,4);
        __m128i a=_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)src));
        __m128i b=_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)pred));
        _mm_storeu_si128((__m128i *)(residual+row*4),_mm_sub_epi32(a,b));
    }
}

/* The uint32 reciprocal and one correction reproduce the scalar integer
   division, including asymmetric signed-short limits. */
__attribute__((target("avx2")))
static int quantize4_avx2(const int32_t transformed[16],
    const IntraQuant4 *quant, short output[16], float ideal_levels[16]) {
    int32_t natural_levels[16];float natural_ideal[16];
    const __m256i signbit=_mm256_set1_epi32(INT32_MIN);
    for (int k=0;k<16;k+=8) {
        __m256i value=_mm256_loadu_si256((const __m256i *)(transformed+k));
        __m256i step=_mm256_loadu_si256((const __m256i *)(quant->step+k));
        __m256i reciprocal=_mm256_loadu_si256((const __m256i *)(quant->reciprocal_q32+k));
        __m256i negative=_mm256_srai_epi32(value,31);
        __m256i absolute=_mm256_abs_epi32(value);
        __m256i rounded=_mm256_add_epi32(absolute,_mm256_srli_epi32(step,1));
        __m256i even=_mm256_srli_epi64(_mm256_mul_epu32(rounded,reciprocal),32);
        __m256i odd=_mm256_mul_epu32(_mm256_srli_epi64(rounded,32),
                                     _mm256_srli_epi64(reciprocal,32));
        __m256i level=_mm256_or_si256(even,_mm256_and_si256(odd,
                                       _mm256_set1_epi64x(-4294967296ll)));
        __m256i remainder=_mm256_sub_epi32(rounded,_mm256_mullo_epi32(level,step));
        __m256i adjust=_mm256_cmpgt_epi32(remainder,_mm256_sub_epi32(step,_mm256_set1_epi32(1)));
        level=_mm256_sub_epi32(level,adjust);
        __m256i limit=_mm256_sub_epi32(_mm256_set1_epi32(32767),negative);
        __m256i overflow=_mm256_cmpgt_epi32(_mm256_xor_si256(level,signbit),
                                          _mm256_xor_si256(limit,signbit));
        if (_mm256_movemask_epi8(overflow)) return 0;
        __m256i signed_level=_mm256_sub_epi32(_mm256_xor_si256(level,negative),negative);
        _mm256_storeu_si256((__m256i *)(natural_levels+k),signed_level);
        if (ideal_levels) _mm256_storeu_ps(natural_ideal+k,
            _mm256_div_ps(_mm256_cvtepi32_ps(value),_mm256_cvtepi32_ps(step)));
    }
    for(int k=0;k<16;k++) {
        int natural=n148_variable_zigzag(4,k);
        output[k]=(short)natural_levels[natural];
        if(ideal_levels)ideal_levels[k]=natural_ideal[natural];
    }
    return 1;
}
#endif

#if defined(__x86_64__) || defined(__i386__)
/* Eight interior 4:2:x pixels. Byte loads stay within each chroma row;
   scalar borders and nonsubsampled planes retain the original path. */
__attribute__((target("avx2")))
static __m128i projection_chroma8(const unsigned char *first,
                                            const unsigned char *second) {
    const __m128i centres = _mm_setr_epi8(1,1,2,2,3,3,4,4,-1,-1,-1,-1,-1,-1,-1,-1);
    const __m128i neighbours = _mm_setr_epi8(0,2,1,3,2,4,3,5,-1,-1,-1,-1,-1,-1,-1,-1);
    __m128i a = _mm_loadl_epi64((const __m128i *)first);
    __m128i b = _mm_loadl_epi64((const __m128i *)second);
    __m128i a0 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(a, centres));
    __m128i a1 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(a, neighbours));
    __m128i b0 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(b, centres));
    __m128i b1 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(b, neighbours));
    return _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(a0,_mm_set1_epi16(9)),
                                      _mm_mullo_epi16(a1,_mm_set1_epi16(3))),
                         _mm_add_epi16(_mm_mullo_epi16(b0,_mm_set1_epi16(3)),b1));
}

__attribute__((target("avx2")))
static __m128i project_luma4(__m128i rgb, __m128i y,
                                  __m128i cb, __m128i cr, double strength) {
    __m256d blue = _mm256_sub_pd(_mm256_div_pd(_mm256_cvtepi32_pd(cb),
                                  _mm256_set1_pd(16.0)),_mm256_set1_pd(128.0));
    __m256d red = _mm256_sub_pd(_mm256_div_pd(_mm256_cvtepi32_pd(cr),
                                  _mm256_set1_pd(16.0)),_mm256_set1_pd(128.0));
    __m256d source = _mm256_cvtepi32_pd(y);
    __m256d target = _mm256_sub_pd(_mm256_sub_pd(_mm256_cvtepi32_pd(rgb),
                       _mm256_mul_pd(_mm256_set1_pd(1.427864),blue)),
                       _mm256_mul_pd(_mm256_set1_pd(0.687864),red));
    target = _mm256_div_pd(target,_mm256_set1_pd(3.0));
    target = _mm256_add_pd(source,_mm256_mul_pd(_mm256_set1_pd(strength),
                           _mm256_sub_pd(target,source)));
    target = _mm256_min_pd(_mm256_max_pd(target,_mm256_setzero_pd()),
                           _mm256_set1_pd(255.0));
    return _mm256_cvttpd_epi32(_mm256_add_pd(target,_mm256_set1_pd(0.5)));
}

__attribute__((target("avx2")))
static void project_luma8_avx2(const unsigned char *rgb,
    const unsigned char *source, const unsigned char *cb0,
    const unsigned char *cb1, const unsigned char *cr0,
    const unsigned char *cr1, unsigned char *output, double strength) {
    __m256i pixels = _mm256_i32gather_epi32((const int *)rgb,
                              _mm256_setr_epi32(0,3,6,9,12,15,18,21),1);
    const __m256i mask = _mm256_set1_epi32(255);
    __m256i sum = _mm256_add_epi32(_mm256_add_epi32(_mm256_and_si256(pixels,mask),
                  _mm256_and_si256(_mm256_srli_epi32(pixels,8),mask)),
                  _mm256_and_si256(_mm256_srli_epi32(pixels,16),mask));
    __m256i y = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)source));
    __m256i cb = _mm256_cvtepu16_epi32(projection_chroma8(cb0,cb1));
    __m256i cr = _mm256_cvtepu16_epi32(projection_chroma8(cr0,cr1));
    __m128i low = project_luma4(_mm256_castsi256_si128(sum),
                         _mm256_castsi256_si128(y),_mm256_castsi256_si128(cb),
                         _mm256_castsi256_si128(cr),strength);
    __m128i high = project_luma4(_mm256_extracti128_si256(sum,1),
                         _mm256_extracti128_si256(y,1),_mm256_extracti128_si256(cb,1),
                         _mm256_extracti128_si256(cr,1),strength);
    __m128i shorts = _mm_packus_epi32(low,high);
    _mm_storel_epi64((__m128i *)output,_mm_packus_epi16(shorts,shorts));
}
#endif

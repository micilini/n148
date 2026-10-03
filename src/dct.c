#include <math.h>
#include <string.h>
#include "dct.h"
#include "cpu.h"

#define BLOCK_SIZE 8
#define PI 3.14159265358979323846

// ============================================================
// REFERENCE TRANSFORM (article 9)
// ============================================================

static double cos_table[BLOCK_SIZE][BLOCK_SIZE];
static double alpha[BLOCK_SIZE];
static int    tables_ready = 0;
#if defined(__x86_64__) || defined(__i386__)
static float sparse_inverse_basis[8][8];
static void init_sparse_inverse_basis(void);
#endif

void init_dct_tables(void) {
    if (tables_ready) return;
    for (int f = 0; f < BLOCK_SIZE; f++) {
        alpha[f] = (f == 0) ? (1.0 / sqrt(2.0)) : 1.0;
        for (int p = 0; p < BLOCK_SIZE; p++)
            cos_table[f][p] = cos((2.0 * p + 1.0) * f * PI / (2.0 * BLOCK_SIZE));
    }
#if defined(__x86_64__) || defined(__i386__)
    init_sparse_inverse_basis();
#endif
    tables_ready = 1;
}

static void dct_1d(double in[BLOCK_SIZE], double out[BLOCK_SIZE]) {
    for (int f = 0; f < BLOCK_SIZE; f++) {
        double s = 0.0;
        for (int p = 0; p < BLOCK_SIZE; p++) s += in[p] * cos_table[f][p];
        out[f] = 0.5 * alpha[f] * s;
    }
}

static void idct_1d(double in[BLOCK_SIZE], double out[BLOCK_SIZE]) {
    for (int p = 0; p < BLOCK_SIZE; p++) {
        double s = 0.0;
        for (int f = 0; f < BLOCK_SIZE; f++) s += alpha[f] * in[f] * cos_table[f][p];
        out[p] = 0.5 * s;
    }
}

void dct_block(double block[8][8], double coef[8][8]) {
    double temp[8][8];
    for (int r = 0; r < 8; r++) {
        double a[8], b[8];
        for (int c = 0; c < 8; c++) a[c] = block[r][c] - 128.0;
        dct_1d(a, b);
        for (int c = 0; c < 8; c++) temp[r][c] = b[c];
    }
    for (int c = 0; c < 8; c++) {
        double a[8], b[8];
        for (int r = 0; r < 8; r++) a[r] = temp[r][c];
        dct_1d(a, b);
        for (int r = 0; r < 8; r++) coef[r][c] = b[r];
    }
}

void idct_block(double coef[8][8], double block[8][8]) {
    double temp[8][8];
    for (int c = 0; c < 8; c++) {
        double a[8], b[8];
        for (int r = 0; r < 8; r++) a[r] = coef[r][c];
        idct_1d(a, b);
        for (int r = 0; r < 8; r++) temp[r][c] = b[r];
    }
    for (int r = 0; r < 8; r++) {
        double a[8], b[8];
        for (int c = 0; c < 8; c++) a[c] = temp[r][c];
        idct_1d(a, b);
        for (int c = 0; c < 8; c++) block[r][c] = b[c] + 128.0;
    }
}

// The scale the AAN transform leaves behind on each coefficient.
static const double AAN_SCALE[8] = {
    1.0, 1.387039845, 1.306562965, 1.175875602,
    1.0, 0.785694958, 0.541196100, 0.275899379
};

double aan_scale_factor(int u, int v) {
    return AAN_SCALE[u] * AAN_SCALE[v] * 8.0;
}

// ============================================================
// SCALAR AAN — 5 multiplications per 1-D pass instead of 64
// ============================================================

static void fdct_pass(float *p, int stride) {
    float t0,t1,t2,t3,t4,t5,t6,t7,t10,t11,t12,t13,z1,z2,z3,z4,z5,z11,z13;

    t0 = p[0*stride] + p[7*stride];  t7 = p[0*stride] - p[7*stride];
    t1 = p[1*stride] + p[6*stride];  t6 = p[1*stride] - p[6*stride];
    t2 = p[2*stride] + p[5*stride];  t5 = p[2*stride] - p[5*stride];
    t3 = p[3*stride] + p[4*stride];  t4 = p[3*stride] - p[4*stride];

    t10 = t0 + t3;  t13 = t0 - t3;
    t11 = t1 + t2;  t12 = t1 - t2;

    p[0*stride] = t10 + t11;
    p[4*stride] = t10 - t11;

    z1 = (t12 + t13) * 0.707106781f;
    p[2*stride] = t13 + z1;
    p[6*stride] = t13 - z1;

    t10 = t4 + t5;
    t11 = t5 + t6;
    t12 = t6 + t7;

    z5 = (t10 - t12) * 0.382683433f;
    z2 = 0.541196100f * t10 + z5;
    z4 = 1.306562965f * t12 + z5;
    z3 = t11 * 0.707106781f;

    z11 = t7 + z3;
    z13 = t7 - z3;

    p[5*stride] = z13 + z2;
    p[3*stride] = z13 - z2;
    p[1*stride] = z11 + z4;
    p[7*stride] = z11 - z4;
}

static void idct_pass(float *p, int stride) {
    float t0,t1,t2,t3,t4,t5,t6,t7,t10,t11,t12,t13,z5,z10,z11,z12,z13;

    t0 = p[0*stride];  t1 = p[2*stride];
    t2 = p[4*stride];  t3 = p[6*stride];

    t10 = t0 + t2;
    t11 = t0 - t2;
    t13 = t1 + t3;
    t12 = (t1 - t3) * 1.414213562f - t13;

    t0 = t10 + t13;  t3 = t10 - t13;
    t1 = t11 + t12;  t2 = t11 - t12;

    t4 = p[1*stride];  t5 = p[3*stride];
    t6 = p[5*stride];  t7 = p[7*stride];

    z13 = t6 + t5;  z10 = t6 - t5;
    z11 = t4 + t7;  z12 = t4 - t7;

    t7  = z11 + z13;
    t11 = (z11 - z13) * 1.414213562f;

    z5  = (z10 + z12) * 1.847759065f;
    t10 = z5 - z12 * 1.082392200f;
    t12 = z5 - z10 * 2.613125930f;

    t6 = t12 - t7;
    t5 = t11 - t6;
    t4 = t10 - t5;

    p[0*stride] = t0 + t7;   p[7*stride] = t0 - t7;
    p[1*stride] = t1 + t6;   p[6*stride] = t1 - t6;
    p[2*stride] = t2 + t5;   p[5*stride] = t2 - t5;
    p[3*stride] = t3 + t4;   p[4*stride] = t3 - t4;
}

static void dct_scalar(const float block[64], float coef[64]) {
    float t[64];
    for (int i = 0; i < 64; i++) t[i] = block[i] - 128.0f;
    /* Match the AVX2 path's pass order.  The two separable orders are
       mathematically equivalent, but changing their order changes the last
       few float bits and can cross a quantization boundary at high quality. */
    for (int c = 0; c < 8; c++) fdct_pass(&t[c],   8);
    for (int r = 0; r < 8; r++) fdct_pass(&t[r*8], 1);
    memcpy(coef, t, sizeof(t));
}

void n148_idct_block_scalar(const float coef[64], float block[64]) {
    float t[64];
    memcpy(t, coef, sizeof(t));
    for (int c = 0; c < 8; c++) idct_pass(&t[c],   8);
    for (int r = 0; r < 8; r++) idct_pass(&t[r*8], 1);
    for (int i = 0; i < 64; i++) block[i] = t[i] + 128.0f;
}

// ============================================================
// AVX2 AAN — all eight columns travel through the butterfly
// together, one per vector lane.
// ============================================================

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

#define TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7) do {                        \
    __m256 a0,a1,a2,a3,a4,a5,a6,a7,b0,b1,b2,b3,b4,b5,b6,b7;             \
    a0=_mm256_unpacklo_ps(r0,r1); a1=_mm256_unpackhi_ps(r0,r1);         \
    a2=_mm256_unpacklo_ps(r2,r3); a3=_mm256_unpackhi_ps(r2,r3);         \
    a4=_mm256_unpacklo_ps(r4,r5); a5=_mm256_unpackhi_ps(r4,r5);         \
    a6=_mm256_unpacklo_ps(r6,r7); a7=_mm256_unpackhi_ps(r6,r7);         \
    b0=_mm256_shuffle_ps(a0,a2,0x44); b1=_mm256_shuffle_ps(a0,a2,0xEE); \
    b2=_mm256_shuffle_ps(a1,a3,0x44); b3=_mm256_shuffle_ps(a1,a3,0xEE); \
    b4=_mm256_shuffle_ps(a4,a6,0x44); b5=_mm256_shuffle_ps(a4,a6,0xEE); \
    b6=_mm256_shuffle_ps(a5,a7,0x44); b7=_mm256_shuffle_ps(a5,a7,0xEE); \
    r0=_mm256_permute2f128_ps(b0,b4,0x20); r1=_mm256_permute2f128_ps(b1,b5,0x20); \
    r2=_mm256_permute2f128_ps(b2,b6,0x20); r3=_mm256_permute2f128_ps(b3,b7,0x20); \
    r4=_mm256_permute2f128_ps(b0,b4,0x31); r5=_mm256_permute2f128_ps(b1,b5,0x31); \
    r6=_mm256_permute2f128_ps(b2,b6,0x31); r7=_mm256_permute2f128_ps(b3,b7,0x31); \
} while (0)

#define AAN_FWD(r0,r1,r2,r3,r4,r5,r6,r7) do {                           \
    __m256 t0,t1,t2,t3,t4,t5,t6,t7,t10,t11,t12,t13,z1,z2,z3,z4,z5,z11,z13; \
    t0=_mm256_add_ps(r0,r7); t7=_mm256_sub_ps(r0,r7);                   \
    t1=_mm256_add_ps(r1,r6); t6=_mm256_sub_ps(r1,r6);                   \
    t2=_mm256_add_ps(r2,r5); t5=_mm256_sub_ps(r2,r5);                   \
    t3=_mm256_add_ps(r3,r4); t4=_mm256_sub_ps(r3,r4);                   \
    t10=_mm256_add_ps(t0,t3); t13=_mm256_sub_ps(t0,t3);                 \
    t11=_mm256_add_ps(t1,t2); t12=_mm256_sub_ps(t1,t2);                 \
    r0=_mm256_add_ps(t10,t11); r4=_mm256_sub_ps(t10,t11);               \
    z1=_mm256_mul_ps(_mm256_add_ps(t12,t13),_mm256_set1_ps(0.707106781f)); \
    r2=_mm256_add_ps(t13,z1); r6=_mm256_sub_ps(t13,z1);                 \
    t10=_mm256_add_ps(t4,t5); t11=_mm256_add_ps(t5,t6); t12=_mm256_add_ps(t6,t7); \
    z5=_mm256_mul_ps(_mm256_sub_ps(t10,t12),_mm256_set1_ps(0.382683433f)); \
    z2=_mm256_add_ps(_mm256_mul_ps(t10,_mm256_set1_ps(0.541196100f)),z5); \
    z4=_mm256_add_ps(_mm256_mul_ps(t12,_mm256_set1_ps(1.306562965f)),z5); \
    z3=_mm256_mul_ps(t11,_mm256_set1_ps(0.707106781f));                 \
    z11=_mm256_add_ps(t7,z3); z13=_mm256_sub_ps(t7,z3);                 \
    r5=_mm256_add_ps(z13,z2); r3=_mm256_sub_ps(z13,z2);                 \
    r1=_mm256_add_ps(z11,z4); r7=_mm256_sub_ps(z11,z4);                 \
} while (0)

#define AAN_INV(r0,r1,r2,r3,r4,r5,r6,r7) do {                           \
    __m256 t0,t1,t2,t3,t4,t5,t6,t7,t10,t11,t12,t13,z5,z10,z11,z12,z13;  \
    t0=r0; t1=r2; t2=r4; t3=r6;                                         \
    t10=_mm256_add_ps(t0,t2); t11=_mm256_sub_ps(t0,t2);                 \
    t13=_mm256_add_ps(t1,t3);                                           \
    t12=_mm256_sub_ps(_mm256_mul_ps(_mm256_sub_ps(t1,t3),_mm256_set1_ps(1.414213562f)),t13); \
    t0=_mm256_add_ps(t10,t13); t3=_mm256_sub_ps(t10,t13);               \
    t1=_mm256_add_ps(t11,t12); t2=_mm256_sub_ps(t11,t12);               \
    t4=r1; t5=r3; t6=r5; t7=r7;                                         \
    z13=_mm256_add_ps(t6,t5); z10=_mm256_sub_ps(t6,t5);                 \
    z11=_mm256_add_ps(t4,t7); z12=_mm256_sub_ps(t4,t7);                 \
    t7=_mm256_add_ps(z11,z13);                                          \
    t11=_mm256_mul_ps(_mm256_sub_ps(z11,z13),_mm256_set1_ps(1.414213562f)); \
    z5=_mm256_mul_ps(_mm256_add_ps(z10,z12),_mm256_set1_ps(1.847759065f)); \
    t10=_mm256_sub_ps(z5,_mm256_mul_ps(z12,_mm256_set1_ps(1.082392200f))); \
    t12=_mm256_sub_ps(z5,_mm256_mul_ps(z10,_mm256_set1_ps(2.613125930f))); \
    t6=_mm256_sub_ps(t12,t7); t5=_mm256_sub_ps(t11,t6); t4=_mm256_sub_ps(t10,t5); \
    r0=_mm256_add_ps(t0,t7); r7=_mm256_sub_ps(t0,t7);                   \
    r1=_mm256_add_ps(t1,t6); r6=_mm256_sub_ps(t1,t6);                   \
    r2=_mm256_add_ps(t2,t5); r5=_mm256_sub_ps(t2,t5);                   \
    r3=_mm256_add_ps(t3,t4); r4=_mm256_sub_ps(t3,t4);                   \
} while (0)

__attribute__((target("avx2")))
void dct_block_avx2(const float block[64], float coef[64]) {
    __m256 r0=_mm256_loadu_ps(block+0),  r1=_mm256_loadu_ps(block+8);
    __m256 r2=_mm256_loadu_ps(block+16), r3=_mm256_loadu_ps(block+24);
    __m256 r4=_mm256_loadu_ps(block+32), r5=_mm256_loadu_ps(block+40);
    __m256 r6=_mm256_loadu_ps(block+48), r7=_mm256_loadu_ps(block+56);

    const __m256 shift = _mm256_set1_ps(128.0f);
    r0=_mm256_sub_ps(r0,shift); r1=_mm256_sub_ps(r1,shift);
    r2=_mm256_sub_ps(r2,shift); r3=_mm256_sub_ps(r3,shift);
    r4=_mm256_sub_ps(r4,shift); r5=_mm256_sub_ps(r5,shift);
    r6=_mm256_sub_ps(r6,shift); r7=_mm256_sub_ps(r7,shift);

    AAN_FWD(r0,r1,r2,r3,r4,r5,r6,r7);
    TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7);
    AAN_FWD(r0,r1,r2,r3,r4,r5,r6,r7);
    TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7);

    _mm256_storeu_ps(coef+0,r0);  _mm256_storeu_ps(coef+8,r1);
    _mm256_storeu_ps(coef+16,r2); _mm256_storeu_ps(coef+24,r3);
    _mm256_storeu_ps(coef+32,r4); _mm256_storeu_ps(coef+40,r5);
    _mm256_storeu_ps(coef+48,r6); _mm256_storeu_ps(coef+56,r7);
}

__attribute__((target("avx2"), always_inline))
static inline void idct_vectors_avx2(const float coef[64],
                                     __m256 *out0, __m256 *out1,
                                     __m256 *out2, __m256 *out3,
                                     __m256 *out4, __m256 *out5,
                                     __m256 *out6, __m256 *out7) {
    __m256 r0=_mm256_loadu_ps(coef+0),  r1=_mm256_loadu_ps(coef+8);
    __m256 r2=_mm256_loadu_ps(coef+16), r3=_mm256_loadu_ps(coef+24);
    __m256 r4=_mm256_loadu_ps(coef+32), r5=_mm256_loadu_ps(coef+40);
    __m256 r6=_mm256_loadu_ps(coef+48), r7=_mm256_loadu_ps(coef+56);

    AAN_INV(r0,r1,r2,r3,r4,r5,r6,r7);
    TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7);
    AAN_INV(r0,r1,r2,r3,r4,r5,r6,r7);
    TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7);

    *out0=r0; *out1=r1; *out2=r2; *out3=r3;
    *out4=r4; *out5=r5; *out6=r6; *out7=r7;
}

__attribute__((target("avx2")))
void idct_block_avx2(const float coef[64], float block[64]) {
    __m256 r0,r1,r2,r3,r4,r5,r6,r7;
    idct_vectors_avx2(coef, &r0, &r1, &r2, &r3,
                      &r4, &r5, &r6, &r7);

    const __m256 shift = _mm256_set1_ps(128.0f);
    _mm256_storeu_ps(block+0, _mm256_add_ps(r0,shift));
    _mm256_storeu_ps(block+8, _mm256_add_ps(r1,shift));
    _mm256_storeu_ps(block+16,_mm256_add_ps(r2,shift));
    _mm256_storeu_ps(block+24,_mm256_add_ps(r3,shift));
    _mm256_storeu_ps(block+32,_mm256_add_ps(r4,shift));
    _mm256_storeu_ps(block+40,_mm256_add_ps(r5,shift));
    _mm256_storeu_ps(block+48,_mm256_add_ps(r6,shift));
    _mm256_storeu_ps(block+56,_mm256_add_ps(r7,shift));
}

__attribute__((target("avx2"), always_inline))
static inline void store_idct_row_avx2(__m256 value, unsigned char *dst,
                                        __m256 shift, __m256 round) {
    value = _mm256_add_ps(value, shift);
    value = _mm256_add_ps(value, round);
    __m256i i32 = _mm256_cvttps_epi32(value);
    __m128i i16 = _mm_packs_epi32(_mm256_castsi256_si128(i32),
                                  _mm256_extracti128_si256(i32, 1));
    _mm_storel_epi64((__m128i *)dst, _mm_packus_epi16(i16, i16));
}

__attribute__((target("avx2")))
void idct_block_store_avx2(const float coef[64], unsigned char *dst,
                           int stride) {
    __m256 r0,r1,r2,r3,r4,r5,r6,r7;
    idct_vectors_avx2(coef, &r0, &r1, &r2, &r3,
                      &r4, &r5, &r6, &r7);

    const __m256 shift = _mm256_set1_ps(128.0f);
    const __m256 round = _mm256_set1_ps(0.5f);
    store_idct_row_avx2(r0, dst + (long)stride * 0, shift, round);
    store_idct_row_avx2(r1, dst + (long)stride * 1, shift, round);
    store_idct_row_avx2(r2, dst + (long)stride * 2, shift, round);
    store_idct_row_avx2(r3, dst + (long)stride * 3, shift, round);
    store_idct_row_avx2(r4, dst + (long)stride * 4, shift, round);
    store_idct_row_avx2(r5, dst + (long)stride * 5, shift, round);
    store_idct_row_avx2(r6, dst + (long)stride * 6, shift, round);
    store_idct_row_avx2(r7, dst + (long)stride * 7, shift, round);
}

__attribute__((target("avx2"), always_inline))
static inline void store_predicted_idct_row_avx2(
    __m256 residual, const unsigned char *prediction, unsigned char *dst) {
    __m128i bytes = _mm_loadl_epi64((const __m128i *) prediction);
    __m256 predictor = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(bytes));
    const __m256 shift = _mm256_set1_ps(128.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 maximum = _mm256_set1_ps(255.0f);
    const __m256 half = _mm256_set1_ps(0.5f);
    __m256 value = _mm256_add_ps(predictor,
                                  _mm256_add_ps(residual, shift));
    value = _mm256_sub_ps(value, shift);
    value = _mm256_min_ps(_mm256_max_ps(value, zero), maximum);
    __m256i rounded = _mm256_cvttps_epi32(_mm256_add_ps(value, half));
    __m128i words = _mm_packs_epi32(_mm256_castsi256_si128(rounded),
                                    _mm256_extracti128_si256(rounded, 1));
    _mm_storel_epi64((__m128i *) dst, _mm_packus_epi16(words, words));
}

__attribute__((target("avx2")))
void idct_block_add_prediction_dc_avx2(float dc,
                                        const unsigned char prediction[64],
                                        unsigned char *dst, int stride) {
    const __m256 level = _mm256_set1_ps(dc);
    for (int row = 0; row < 8; row++)
        store_predicted_idct_row_avx2(
            level, prediction + row * 8, dst + (long)stride * row);
}

__attribute__((target("avx2")))
void idct_block_add_prediction_avx2(const float coef[64],
                                     const unsigned char prediction[64],
                                     unsigned char *dst, int stride) {
    __m256 r0,r1,r2,r3,r4,r5,r6,r7;
    idct_vectors_avx2(coef, &r0, &r1, &r2, &r3,
                      &r4, &r5, &r6, &r7);
    store_predicted_idct_row_avx2(r0, prediction + 0, dst + (long)stride * 0);
    store_predicted_idct_row_avx2(r1, prediction + 8, dst + (long)stride * 1);
    store_predicted_idct_row_avx2(r2, prediction + 16, dst + (long)stride * 2);
    store_predicted_idct_row_avx2(r3, prediction + 24, dst + (long)stride * 3);
    store_predicted_idct_row_avx2(r4, prediction + 32, dst + (long)stride * 4);
    store_predicted_idct_row_avx2(r5, prediction + 40, dst + (long)stride * 5);
    store_predicted_idct_row_avx2(r6, prediction + 48, dst + (long)stride * 6);
    store_predicted_idct_row_avx2(r7, prediction + 56, dst + (long)stride * 7);
}

/* A separable inverse transform reconstructs a sparse block as the DC level
   plus one outer product per AC coefficient.  These are the exact one-pass
   AAN responses used by the full transform, kept as eight tiny basis rows. */
static void init_sparse_inverse_basis(void) {
    for (int frequency = 0; frequency < 8; frequency++) {
        float values[8] = {0};
        values[frequency] = 1.0f;
        idct_pass(values, 1);
        memcpy(sparse_inverse_basis[frequency], values, sizeof(values));
    }
}

__attribute__((target("avx2")))
void idct_block_store_single_avx2(float dc, float ac, int index,
                                  unsigned char *dst, int stride) {
    int vertical_frequency = index >> 3;
    int horizontal_frequency = index & 7;
    __m256 horizontal =
        _mm256_loadu_ps(sparse_inverse_basis[horizontal_frequency]);
    const __m256 dc_vector = _mm256_set1_ps(dc);
    const __m256 shift = _mm256_set1_ps(128.0f);
    const __m256 round = _mm256_set1_ps(0.5f);
    for (int row = 0; row < 8; row++) {
        float vertical = ac * sparse_inverse_basis[vertical_frequency][row];
        __m256 value = _mm256_add_ps(
            dc_vector, _mm256_mul_ps(_mm256_set1_ps(vertical), horizontal));
        store_idct_row_avx2(value, dst + (long)stride * row, shift, round);
    }
}

__attribute__((target("avx2")))
void idct_block_store_two_avx2(float dc,
                               float first_ac, int first_index,
                               float second_ac, int second_index,
                               unsigned char *dst, int stride) {
    int first_vertical = first_index >> 3;
    int second_vertical = second_index >> 3;
    __m256 first_horizontal =
        _mm256_loadu_ps(sparse_inverse_basis[first_index & 7]);
    __m256 second_horizontal =
        _mm256_loadu_ps(sparse_inverse_basis[second_index & 7]);
    const __m256 dc_vector = _mm256_set1_ps(dc);
    const __m256 shift = _mm256_set1_ps(128.0f);
    const __m256 round = _mm256_set1_ps(0.5f);
    for (int row = 0; row < 8; row++) {
        float first = first_ac * sparse_inverse_basis[first_vertical][row];
        float second = second_ac * sparse_inverse_basis[second_vertical][row];
        __m256 value = _mm256_add_ps(
            dc_vector, _mm256_mul_ps(_mm256_set1_ps(first), first_horizontal));
        value = _mm256_add_ps(
            value, _mm256_mul_ps(_mm256_set1_ps(second), second_horizontal));
        store_idct_row_avx2(value, dst + (long)stride * row, shift, round);
    }
}

__attribute__((target("avx2")))
void idct_block_store_three_avx2(float dc,
                                 float first_ac, int first_index,
                                 float second_ac, int second_index,
                                 float third_ac, int third_index,
                                 unsigned char *dst, int stride) {
    int first_vertical = first_index >> 3;
    int second_vertical = second_index >> 3;
    int third_vertical = third_index >> 3;
    __m256 first_horizontal =
        _mm256_loadu_ps(sparse_inverse_basis[first_index & 7]);
    __m256 second_horizontal =
        _mm256_loadu_ps(sparse_inverse_basis[second_index & 7]);
    __m256 third_horizontal =
        _mm256_loadu_ps(sparse_inverse_basis[third_index & 7]);
    const __m256 dc_vector = _mm256_set1_ps(dc);
    const __m256 shift = _mm256_set1_ps(128.0f);
    const __m256 round = _mm256_set1_ps(0.5f);
    for (int row = 0; row < 8; row++) {
        float first = first_ac * sparse_inverse_basis[first_vertical][row];
        float second = second_ac * sparse_inverse_basis[second_vertical][row];
        float third = third_ac * sparse_inverse_basis[third_vertical][row];
        __m256 value = _mm256_add_ps(
            dc_vector, _mm256_mul_ps(_mm256_set1_ps(first), first_horizontal));
        value = _mm256_add_ps(
            value, _mm256_mul_ps(_mm256_set1_ps(second), second_horizontal));
        value = _mm256_add_ps(
            value, _mm256_mul_ps(_mm256_set1_ps(third), third_horizontal));
        store_idct_row_avx2(value, dst + (long)stride * row, shift, round);
    }
}

#endif

// ============================================================
// DISPATCH
// ============================================================

void dct_block_fast(const float block[64], float coef[64]) {
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        dct_block_avx2(block, coef);
        return;
    }
#endif
    dct_scalar(block, coef);
}

void idct_block_fast(const float coef[64], float block[64]) {
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        idct_block_avx2(coef, block);
        return;
    }
#endif
    n148_idct_block_scalar(coef, block);
}

#include <string.h>

#include "cpu.h"
#include "dct.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

static const float AAN_SCALE[8] = {
    1.000000000f, 1.387039845f, 1.306562965f, 1.175875602f,
    1.000000000f, 0.785694958f, 0.541196100f, 0.275899379f
};

float aan_scale_factor(int row, int column) {
    return AAN_SCALE[row] * AAN_SCALE[column] * 8.0f;
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

static void dct_scalar(const float block[64], float coef[64]) {
    float t[64];
    for (int i = 0; i < 64; i++) t[i] = block[i] - 128.0f;   // level shift
    for (int r = 0; r < 8; r++) fdct_pass(&t[r*8], 1);       // rows
    for (int c = 0; c < 8; c++) fdct_pass(&t[c],   8);       // columns
    memcpy(coef, t, sizeof(t));
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

static void idct_scalar(const float coef[64], float block[64]) {
    float t[64];
    memcpy(t, coef, sizeof(t));
    for (int c = 0; c < 8; c++) idct_pass(&t[c],   8);       // columns
    for (int r = 0; r < 8; r++) idct_pass(&t[r*8], 1);       // rows
    for (int i = 0; i < 64; i++) block[i] = t[i] + 128.0f;   // undo level shift
}

#if defined(__x86_64__) || defined(__i386__)

// Eight rows live in eight registers. Each lane is therefore one column,
// allowing the scalar AAN butterfly to run on all columns at once.
#define AAN_FWD(r0,r1,r2,r3,r4,r5,r6,r7) do {                         \
    __m256 t0,t1,t2,t3,t4,t5,t6,t7,t10,t11,t12,t13;                  \
    __m256 z1,z2,z3,z4,z5,z11,z13;                                    \
    t0=_mm256_add_ps(r0,r7); t7=_mm256_sub_ps(r0,r7);                 \
    t1=_mm256_add_ps(r1,r6); t6=_mm256_sub_ps(r1,r6);                 \
    t2=_mm256_add_ps(r2,r5); t5=_mm256_sub_ps(r2,r5);                 \
    t3=_mm256_add_ps(r3,r4); t4=_mm256_sub_ps(r3,r4);                 \
    t10=_mm256_add_ps(t0,t3); t13=_mm256_sub_ps(t0,t3);               \
    t11=_mm256_add_ps(t1,t2); t12=_mm256_sub_ps(t1,t2);               \
    r0=_mm256_add_ps(t10,t11); r4=_mm256_sub_ps(t10,t11);             \
    z1=_mm256_mul_ps(_mm256_add_ps(t12,t13),                           \
                     _mm256_set1_ps(0.707106781f));                    \
    r2=_mm256_add_ps(t13,z1); r6=_mm256_sub_ps(t13,z1);               \
    t10=_mm256_add_ps(t4,t5);                                         \
    t11=_mm256_add_ps(t5,t6);                                         \
    t12=_mm256_add_ps(t6,t7);                                         \
    z5=_mm256_mul_ps(_mm256_sub_ps(t10,t12),                           \
                     _mm256_set1_ps(0.382683433f));                    \
    z2=_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(0.541196100f),t10), \
                     z5);                                              \
    z4=_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(1.306562965f),t12), \
                     z5);                                              \
    z3=_mm256_mul_ps(t11,_mm256_set1_ps(0.707106781f));                \
    z11=_mm256_add_ps(t7,z3); z13=_mm256_sub_ps(t7,z3);               \
    r5=_mm256_add_ps(z13,z2); r3=_mm256_sub_ps(z13,z2);               \
    r1=_mm256_add_ps(z11,z4); r7=_mm256_sub_ps(z11,z4);               \
} while (0)

#define AAN_INV(r0,r1,r2,r3,r4,r5,r6,r7) do {                         \
    __m256 t0,t1,t2,t3,t4,t5,t6,t7,t10,t11,t12,t13;                  \
    __m256 z5,z10,z11,z12,z13;                                        \
    t0=r0; t1=r2; t2=r4; t3=r6;                                      \
    t10=_mm256_add_ps(t0,t2); t11=_mm256_sub_ps(t0,t2);               \
    t13=_mm256_add_ps(t1,t3);                                         \
    t12=_mm256_sub_ps(                                                 \
        _mm256_mul_ps(_mm256_sub_ps(t1,t3),                           \
                      _mm256_set1_ps(1.414213562f)),t13);              \
    t0=_mm256_add_ps(t10,t13); t3=_mm256_sub_ps(t10,t13);             \
    t1=_mm256_add_ps(t11,t12); t2=_mm256_sub_ps(t11,t12);             \
    t4=r1; t5=r3; t6=r5; t7=r7;                                      \
    z13=_mm256_add_ps(t6,t5); z10=_mm256_sub_ps(t6,t5);               \
    z11=_mm256_add_ps(t4,t7); z12=_mm256_sub_ps(t4,t7);               \
    t7=_mm256_add_ps(z11,z13);                                        \
    t11=_mm256_mul_ps(_mm256_sub_ps(z11,z13),                         \
                      _mm256_set1_ps(1.414213562f));                   \
    z5=_mm256_mul_ps(_mm256_add_ps(z10,z12),                          \
                     _mm256_set1_ps(1.847759065f));                    \
    t10=_mm256_sub_ps(                                                 \
        z5,_mm256_mul_ps(z12,_mm256_set1_ps(1.082392200f)));          \
    t12=_mm256_sub_ps(                                                 \
        z5,_mm256_mul_ps(z10,_mm256_set1_ps(2.613125930f)));          \
    t6=_mm256_sub_ps(t12,t7);                                         \
    t5=_mm256_sub_ps(t11,t6);                                         \
    t4=_mm256_sub_ps(t10,t5);                                         \
    r0=_mm256_add_ps(t0,t7); r7=_mm256_sub_ps(t0,t7);                 \
    r1=_mm256_add_ps(t1,t6); r6=_mm256_sub_ps(t1,t6);                 \
    r2=_mm256_add_ps(t2,t5); r5=_mm256_sub_ps(t2,t5);                 \
    r3=_mm256_add_ps(t3,t4); r4=_mm256_sub_ps(t3,t4);                 \
} while (0)

// Transposing turns rows into columns, so the same vector butterfly can
// perform both separable passes without a second implementation.
#define TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7) do {                      \
    __m256 a0,a1,a2,a3,a4,a5,a6,a7,b0,b1,b2,b3,b4,b5,b6,b7;          \
    a0=_mm256_unpacklo_ps(r0,r1); a1=_mm256_unpackhi_ps(r0,r1);       \
    a2=_mm256_unpacklo_ps(r2,r3); a3=_mm256_unpackhi_ps(r2,r3);       \
    a4=_mm256_unpacklo_ps(r4,r5); a5=_mm256_unpackhi_ps(r4,r5);       \
    a6=_mm256_unpacklo_ps(r6,r7); a7=_mm256_unpackhi_ps(r6,r7);       \
    b0=_mm256_shuffle_ps(a0,a2,_MM_SHUFFLE(1,0,1,0));                 \
    b1=_mm256_shuffle_ps(a0,a2,_MM_SHUFFLE(3,2,3,2));                 \
    b2=_mm256_shuffle_ps(a1,a3,_MM_SHUFFLE(1,0,1,0));                 \
    b3=_mm256_shuffle_ps(a1,a3,_MM_SHUFFLE(3,2,3,2));                 \
    b4=_mm256_shuffle_ps(a4,a6,_MM_SHUFFLE(1,0,1,0));                 \
    b5=_mm256_shuffle_ps(a4,a6,_MM_SHUFFLE(3,2,3,2));                 \
    b6=_mm256_shuffle_ps(a5,a7,_MM_SHUFFLE(1,0,1,0));                 \
    b7=_mm256_shuffle_ps(a5,a7,_MM_SHUFFLE(3,2,3,2));                 \
    r0=_mm256_permute2f128_ps(b0,b4,0x20);                            \
    r1=_mm256_permute2f128_ps(b1,b5,0x20);                            \
    r2=_mm256_permute2f128_ps(b2,b6,0x20);                            \
    r3=_mm256_permute2f128_ps(b3,b7,0x20);                            \
    r4=_mm256_permute2f128_ps(b0,b4,0x31);                            \
    r5=_mm256_permute2f128_ps(b1,b5,0x31);                            \
    r6=_mm256_permute2f128_ps(b2,b6,0x31);                            \
    r7=_mm256_permute2f128_ps(b3,b7,0x31);                            \
} while (0)

__attribute__((target("avx2")))
static void dct_block_avx2(const float block[64], float coef[64]) {
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

__attribute__((target("avx2")))
static void idct_block_avx2(const float coef[64], float block[64]) {
    __m256 r0=_mm256_loadu_ps(coef+0),  r1=_mm256_loadu_ps(coef+8);
    __m256 r2=_mm256_loadu_ps(coef+16), r3=_mm256_loadu_ps(coef+24);
    __m256 r4=_mm256_loadu_ps(coef+32), r5=_mm256_loadu_ps(coef+40);
    __m256 r6=_mm256_loadu_ps(coef+48), r7=_mm256_loadu_ps(coef+56);

    AAN_INV(r0,r1,r2,r3,r4,r5,r6,r7);
    TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7);
    AAN_INV(r0,r1,r2,r3,r4,r5,r6,r7);
    TRANSPOSE8(r0,r1,r2,r3,r4,r5,r6,r7);

    const __m256 shift = _mm256_set1_ps(128.0f);
    r0=_mm256_add_ps(r0,shift); r1=_mm256_add_ps(r1,shift);
    r2=_mm256_add_ps(r2,shift); r3=_mm256_add_ps(r3,shift);
    r4=_mm256_add_ps(r4,shift); r5=_mm256_add_ps(r5,shift);
    r6=_mm256_add_ps(r6,shift); r7=_mm256_add_ps(r7,shift);

    _mm256_storeu_ps(block+0,r0);  _mm256_storeu_ps(block+8,r1);
    _mm256_storeu_ps(block+16,r2); _mm256_storeu_ps(block+24,r3);
    _mm256_storeu_ps(block+32,r4); _mm256_storeu_ps(block+40,r5);
    _mm256_storeu_ps(block+48,r6); _mm256_storeu_ps(block+56,r7);
}

#endif

void dct_block_fast(const float block[64], float coefficients[64]) {
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        dct_block_avx2(block, coefficients);
        return;
    }
#endif
    dct_scalar(block, coefficients);
}

void idct_block_fast(const float coefficients[64], float block[64]) {
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        idct_block_avx2(coefficients, block);
        return;
    }
#endif
    idct_scalar(coefficients, block);
}

#include <string.h>

#include "dct.h"

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

void dct_block_fast(const float block[64], float coefficients[64]) {
    dct_scalar(block, coefficients);
}

void idct_block_fast(const float coefficients[64], float block[64]) {
    idct_scalar(coefficients, block);
}

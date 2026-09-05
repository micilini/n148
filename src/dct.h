/*
 * N.148 codec - forward and inverse DCT.
 *
 * Three implementations live behind this interface:
 *
 *   1. The separable reference transform (slow, exact, easy to read).
 *   2. A scalar AAN fast transform.
 *   3. An AVX2 AAN transform that handles all eight columns at once.
 *
 * The fast paths produce coefficients multiplied by a per-position
 * constant. That constant is folded into the quantization tables, so
 * nothing has to undo it at run time.
 */
#ifndef DCT_H
#define DCT_H

// Builds the cosine tables. Safe to call more than once.
void init_dct_tables(void);

// ---- Reference transform (article 9) ----
void dct_block(double block[8][8], double coef[8][8]);
void idct_block(double coef[8][8], double block[8][8]);

// ---- Fast transform, scaled output ----
// Input and output are 64 floats in row-major order.
void dct_block_fast(const float block[64], float coef[64]);
void idct_block_fast(const float coef[64], float block[64]);

// Direct entry points for callers that already performed CPU dispatch.
#if defined(__x86_64__) || defined(__i386__)
void dct_block_avx2(const float block[64], float coef[64]);
void idct_block_avx2(const float coef[64], float block[64]);
void idct_block_store_avx2(const float coef[64], unsigned char *dst,
                           int stride);
void idct_block_store_single_avx2(float dc, float ac, int index,
                                  unsigned char *dst, int stride);
void idct_block_store_two_avx2(float dc,
                               float first_ac, int first_index,
                               float second_ac, int second_index,
                               unsigned char *dst, int stride);
void idct_block_store_three_avx2(float dc,
                                 float first_ac, int first_index,
                                 float second_ac, int second_index,
                                 float third_ac, int third_index,
                                 unsigned char *dst, int stride);
#endif

// The constant the fast transform leaves on coefficient (u,v).
double aan_scale_factor(int u, int v);

#endif

#ifndef DCT_H
#define DCT_H

// Scale left on one AAN coefficient after the two one-dimensional passes.
float aan_scale_factor(int row, int column);

// Forward AAN DCT: 8x8 pixels to 64 pre-scaled frequency coefficients.
void dct_block_fast(const float block[64], float coefficients[64]);

// Inverse AAN DCT: 64 pre-scaled coefficients to 8x8 pixels.
void idct_block_fast(const float coefficients[64], float block[64]);

#if defined(__x86_64__) || defined(__i386__)
// Sparse inverse paths reconstruct complete interior blocks directly into
// their destination plane.
void idct_block_store_single_avx2(float dc, float ac, int index,
                                  unsigned char *destination, int stride);
void idct_block_store_two_avx2(float dc,
                               float first_ac, int first_index,
                               float second_ac, int second_index,
                               unsigned char *destination, int stride);
void idct_block_store_three_avx2(float dc,
                                 float first_ac, int first_index,
                                 float second_ac, int second_index,
                                 float third_ac, int third_index,
                                 unsigned char *destination, int stride);
#endif

#endif

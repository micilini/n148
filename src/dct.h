#ifndef DCT_H
#define DCT_H

// Scale left on one AAN coefficient after the two one-dimensional passes.
float aan_scale_factor(int row, int column);

// Forward AAN DCT: 8x8 pixels to 64 pre-scaled frequency coefficients.
void dct_block_fast(const float block[64], float coefficients[64]);

// Inverse AAN DCT: 64 pre-scaled coefficients to 8x8 pixels.
void idct_block_fast(const float coefficients[64], float block[64]);

#endif

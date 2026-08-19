#ifndef DCT_H
#define DCT_H

// Builds the cosine tables. Safe to call more than once.
void init_dct_tables(void);

// Forward DCT: 8x8 pixels to 8x8 frequency coefficients.
void dct_block(double block[8][8], double coefficients[8][8]);

// Inverse DCT: 8x8 frequency coefficients to 8x8 pixels.
void idct_block(double coefficients[8][8], double block[8][8]);

#endif

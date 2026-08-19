#ifndef TABLES_H
#define TABLES_H

extern const int Q_LUMA_BASE[8][8];
extern const int Q_CHROMA_BASE[8][8];

extern const int ZIGZAG[64];

extern const int BITS_DC_LUMA[17];
extern const unsigned char VAL_DC_LUMA[12];
extern const int BITS_DC_CHROMA[17];
extern const unsigned char VAL_DC_CHROMA[12];
extern const int BITS_AC_LUMA[17];
extern const unsigned char VAL_AC_LUMA[162];
extern const int BITS_AC_CHROMA[17];
extern const unsigned char VAL_AC_CHROMA[162];

void scale_table(const int base[8][8], int quality, int output[8][8]);

#endif

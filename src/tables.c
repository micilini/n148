#include "tables.h"

const int Q_LUMA_BASE[8][8] = {
    {16, 11, 10, 16, 24, 40, 51, 61},
    {12, 12, 14, 19, 26, 58, 60, 55},
    {14, 13, 16, 24, 40, 57, 69, 56},
    {14, 17, 22, 29, 51, 87, 80, 62},
    {18, 22, 37, 56, 68, 109, 103, 77},
    {24, 35, 55, 64, 81, 104, 113, 92},
    {49, 64, 78, 87, 103, 121, 120, 101},
    {72, 92, 95, 98, 112, 100, 103, 99}
};

/* Format 5's accepted table is a 12.5% geometric blend toward the measured
   residual-standard-deviation shape, scaled by 1.4 to preserve the
   luma/chroma rate balance. Keeping it format-local protects format 1-4 streams. */
const int FORMAT_5_LUMA_QUANT_BASE[8][8] = {
    {22, 15, 14, 20, 28, 44, 53, 60},
    {16, 16, 18, 23, 29, 58, 58, 53},
    {18, 16, 19, 27, 42, 56, 65, 53},
    {18, 20, 25, 32, 51, 80, 73, 57},
    {21, 25, 39, 55, 65, 96, 90, 67},
    {27, 36, 54, 61, 74, 91, 95, 77},
    {49, 60, 71, 77, 89, 101, 97, 81},
    {67, 80, 82, 83, 92, 82, 82, 77}
};

/* Format 7's optional spectral profile redistributes bits from the first
   spatial frequencies to fine luma detail. The shape was guided by a
   matched-rate reconstruction diagnosis; the old table stays intact for
   every pre-existing stream. */
const int N148_SPECTRAL_LUMA_QUANT_BASE[8][8] = {
    {26, 17, 16, 20, 28, 44, 53, 48},
    {19, 19, 18, 23, 29, 58, 46, 42},
    {21, 16, 19, 27, 42, 45, 52, 42},
    {18, 20, 25, 32, 41, 64, 58, 46},
    {21, 25, 39, 44, 52, 77, 72, 46},
    {27, 36, 43, 49, 59, 73, 65, 52},
    {49, 48, 57, 62, 71, 69, 66, 55},
    {54, 64, 66, 66, 63, 56, 56, 52},
};

/* Format 7's refined spectral profile moves a small bit budget from the low
   band to fine luma detail: low-band steps rise by about 8%, middle-band
   steps stay fixed, and high-band steps fall by about 4%. The extra format-7
   feature bit keeps previously encoded spectral streams on their table. */
const int N148_REFINED_SPECTRAL_LUMA_QUANT_BASE[8][8] = {
    {28, 18, 17, 20, 28, 44, 53, 46},
    {21, 21, 18, 23, 29, 58, 44, 40},
    {23, 16, 19, 27, 42, 43, 50, 40},
    {18, 20, 25, 32, 39, 61, 56, 44},
    {21, 25, 39, 42, 50, 74, 69, 44},
    {27, 36, 41, 47, 57, 70, 62, 50},
    {49, 46, 55, 60, 68, 66, 63, 53},
    {52, 61, 63, 63, 60, 54, 54, 50},
};

const int Q_CHROMA_BASE[8][8] = {
    {17, 18, 24, 47, 99, 99, 99, 99},
    {18, 21, 26, 66, 99, 99, 99, 99},
    {24, 26, 56, 99, 99, 99, 99, 99},
    {47, 66, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99}
};

/* Format 4 keeps its chroma-residual table separate so calibration cannot change
   any format 1, 2, or 3 stream. These accepted weights retain more medium/high
   frequency chroma detail than the heavily saturated historical table. */
const int FORMAT_4_CHROMA_QUANT_BASE[8][8] = {
    {17, 18, 24, 40, 70, 80, 90, 99},
    {18, 21, 26, 48, 70, 85, 95, 99},
    {24, 26, 40, 60, 80, 90, 99, 99},
    {40, 48, 60, 75, 90, 99, 99, 99},
    {70, 70, 80, 90, 99, 99, 99, 99},
    {80, 85, 90, 99, 99, 99, 99, 99},
    {90, 95, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99}
};

/* Segmentation starts from the mature JPEG frequency weighting, then calibrates
   the plane scales in its own colour space. Keeping dedicated tables makes
   that calibration normative without changing any earlier stream. */
const int Q_PERCEPTUAL_INTENSITY_BASE[8][8] = {
    {16, 11, 10, 16, 24, 40, 51, 61},
    {12, 12, 14, 19, 26, 58, 60, 55},
    {14, 13, 16, 24, 40, 57, 69, 56},
    {14, 17, 22, 29, 51, 87, 80, 62},
    {18, 22, 37, 56, 68,109,103, 77},
    {24, 35, 55, 64, 81,104,113, 92},
    {49, 64, 78, 87,103,121,120,101},
    {72, 92, 95, 98,112,100,103, 99}
};

const int Q_PERCEPTUAL_OPPONENT_BASE[8][8] = {
    {17, 18, 24, 47, 99, 99, 99, 99},
    {18, 21, 26, 66, 99, 99, 99, 99},
    {24, 26, 56, 99, 99, 99, 99, 99},
    {47, 66, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99}
};

const int ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

const int BITS_DC_LUMA[17] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
const unsigned char VAL_DC_LUMA[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

const int BITS_DC_CHROMA[17] = {
    0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
const unsigned char VAL_DC_CHROMA[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

const int BITS_AC_LUMA[17] = {
    0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125
};
const unsigned char VAL_AC_LUMA[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

const int BITS_AC_CHROMA[17] = {
    0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119
};
const unsigned char VAL_AC_CHROMA[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

void scale_table(const int base[8][8], int quality, int output[8][8]) {
    if (quality <= 0) {
        quality = 1;
    }
    if (quality > 100) {
        quality = 100;
    }

    int factor =
        (quality < 50) ? (5000 / quality) : (200 - quality * 2);

    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            int value = (base[row][column] * factor + 50) / 100;
            if (value < 1) {
                value = 1;
            }
            if (value > 255) {
                value = 255;
            }
            output[row][column] = value;
        }
    }
}

/* Derived from N.148 spatial-basis error energy, with six protected low frequencies. */
const int N148_BALANCED_LUMA_QUANT_BASE[8][8] = {
    {28, 25, 25, 28, 29, 31, 32, 31},
    {26, 26, 28, 28, 29, 32, 31, 30},
    {27, 27, 28, 29, 31, 31, 31, 30},
    {28, 28, 29, 30, 30, 32, 32, 31},
    {28, 29, 30, 31, 31, 33, 33, 31},
    {29, 30, 31, 31, 32, 33, 32, 31},
    {31, 31, 32, 32, 33, 33, 32, 32},
    {32, 32, 32, 32, 32, 32, 32, 31},
};

const int N148_STRUCTURAL_LUMA_QUANT_BASE[8][8] = {
    {28, 18, 17, 28, 33, 41, 45, 42},
    {21, 21, 27, 30, 34, 47, 41, 39},
    {23, 26, 27, 32, 40, 41, 44, 39},
    {27, 28, 31, 35, 39, 49, 47, 41},
    {29, 31, 39, 40, 44, 54, 52, 41},
    {32, 37, 40, 43, 47, 52, 49, 44},
    {44, 42, 46, 48, 51, 51, 49, 45},
    {45, 49, 49, 49, 48, 46, 46, 44}
};

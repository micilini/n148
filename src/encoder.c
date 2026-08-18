#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "encoder.h"

#define BLOCK_SIZE 8
#define PI 3.14159265358979323846

// ============================================================
// DCT
// ============================================================

static double cos_table[BLOCK_SIZE][BLOCK_SIZE];
static double alpha[BLOCK_SIZE];
static int tables_ready = 0;

static void init_dct_tables(void) {
    if (tables_ready) {
        return;
    }

    for (int frequency = 0; frequency < BLOCK_SIZE; frequency++) {
        alpha[frequency] =
            (frequency == 0) ? (1.0 / sqrt(2.0)) : 1.0;

        for (int position = 0; position < BLOCK_SIZE; position++) {
            cos_table[frequency][position] =
                cos((2.0 * position + 1.0) * frequency * PI /
                    (2.0 * BLOCK_SIZE));
        }
    }

    tables_ready = 1;
}

static void dct_1d(double input[BLOCK_SIZE], double output[BLOCK_SIZE]) {
    for (int frequency = 0; frequency < BLOCK_SIZE; frequency++) {
        double sum = 0.0;
        for (int position = 0; position < BLOCK_SIZE; position++) {
            sum += input[position] * cos_table[frequency][position];
        }
        output[frequency] = 0.5 * alpha[frequency] * sum;
    }
}

static void dct_block(double block[8][8], double coefficients[8][8]) {
    double temporary[8][8];

    for (int row = 0; row < 8; row++) {
        double input[8];
        double output[8];
        for (int column = 0; column < 8; column++) {
            input[column] = block[row][column] - 128.0; // Level shift.
        }
        dct_1d(input, output);
        for (int column = 0; column < 8; column++) {
            temporary[row][column] = output[column];
        }
    }

    for (int column = 0; column < 8; column++) {
        double input[8];
        double output[8];
        for (int row = 0; row < 8; row++) {
            input[row] = temporary[row][column];
        }
        dct_1d(input, output);
        for (int row = 0; row < 8; row++) {
            coefficients[row][column] = output[row];
        }
    }
}

// ============================================================
// QUANTIZATION TABLES
// ============================================================

static const int Q_LUMA_BASE[8][8] = {
    {16, 11, 10, 16, 24, 40, 51, 61},
    {12, 12, 14, 19, 26, 58, 60, 55},
    {14, 13, 16, 24, 40, 57, 69, 56},
    {14, 17, 22, 29, 51, 87, 80, 62},
    {18, 22, 37, 56, 68, 109, 103, 77},
    {24, 35, 55, 64, 81, 104, 113, 92},
    {49, 64, 78, 87, 103, 121, 120, 101},
    {72, 92, 95, 98, 112, 100, 103, 99}
};

static const int Q_CHROMA_BASE[8][8] = {
    {17, 18, 24, 47, 99, 99, 99, 99},
    {18, 21, 26, 66, 99, 99, 99, 99},
    {24, 26, 56, 99, 99, 99, 99, 99},
    {47, 66, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99},
    {99, 99, 99, 99, 99, 99, 99, 99}
};

static void scale_table(const int base[8][8], int quality, int output[8][8]) {
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

// ============================================================
// ZIG-ZAG ORDER
// ============================================================

static const int ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

// ============================================================
// HUFFMAN TABLES
// ============================================================

static const int BITS_DC_LUMA[17] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const unsigned char VAL_DC_LUMA[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const int BITS_DC_CHROMA[17] = {
    0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
static const unsigned char VAL_DC_CHROMA[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const int BITS_AC_LUMA[17] = {
    0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125
};
static const unsigned char VAL_AC_LUMA[162] = {
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

static const int BITS_AC_CHROMA[17] = {
    0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119
};
static const unsigned char VAL_AC_CHROMA[162] = {
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

typedef struct {
    int code[256];
    int length[256];
} HuffTable;

static void build_huffman(const int bits[17], const unsigned char *values,
                          HuffTable *table) {
    for (int i = 0; i < 256; i++) {
        table->code[i] = 0;
        table->length[i] = 0;
    }

    int code = 0;
    int value_index = 0;
    for (int length = 1; length <= 16; length++) {
        for (int i = 0; i < bits[length]; i++) {
            unsigned char symbol = values[value_index++];
            table->code[symbol] = code;
            table->length[symbol] = length;
            code++;
        }
        code <<= 1;
    }
}

// ============================================================
// BIT WRITER
// ============================================================

typedef struct {
    unsigned char *buffer;
    long capacity;
    long size;
    int bit_count;
    unsigned int accumulator;
} BitWriter;

static int bw_init(BitWriter *writer, long capacity) {
    writer->buffer = (unsigned char *)malloc((size_t)capacity);
    if (!writer->buffer) {
        return 0;
    }

    writer->capacity = capacity;
    writer->size = 0;
    writer->bit_count = 0;
    writer->accumulator = 0;
    return 1;
}

static void bw_write_bits(BitWriter *writer, int value, int count) {
    for (int i = count - 1; i >= 0; i--) {
        writer->accumulator =
            (writer->accumulator << 1) | ((value >> i) & 1);
        writer->bit_count++;

        if (writer->bit_count == 8) {
            if (writer->size >= writer->capacity) {
                writer->capacity *= 2;
                writer->buffer = (unsigned char *)realloc(
                    writer->buffer, (size_t)writer->capacity);
            }
            writer->buffer[writer->size++] =
                (unsigned char)writer->accumulator;
            writer->accumulator = 0;
            writer->bit_count = 0;
        }
    }
}

static void bw_flush(BitWriter *writer) {
    while (writer->bit_count != 0) {
        bw_write_bits(writer, 0, 1); // Pad the final byte with zeros.
    }
}

// ============================================================
// CATEGORY AND AMPLITUDE
// ============================================================

static int category(int value) {
    if (value == 0) {
        return 0;
    }

    int absolute = abs(value);
    int size = 0;
    while (absolute) {
        size++;
        absolute >>= 1;
    }
    return size;
}

static int amplitude(int value, int size) {
    int mask = (1 << size) - 1;
    return (value < 0) ? ((value - 1) & mask) : (value & mask);
}

// ============================================================
// ENCODE ONE 8x8 BLOCK
// ============================================================

static void encode_block(BitWriter *writer, double block[8][8],
                         int quantization[8][8],
                         HuffTable *dc_table, HuffTable *ac_table,
                         int *previous_dc) {
    double coefficients[8][8];
    dct_block(block, coefficients);

    // Quantize directly into zig-zag order.
    int zigzag[64];
    for (int i = 0; i < 64; i++) {
        int index = ZIGZAG[i];
        int row = index / 8;
        int column = index % 8;
        zigzag[i] = (int)round(
            coefficients[row][column] / quantization[row][column]);
    }

    // DC: differential pulse-code modulation.
    int difference = zigzag[0] - *previous_dc;
    *previous_dc = zigzag[0];
    int size = category(difference);
    bw_write_bits(writer, dc_table->code[size], dc_table->length[size]);
    if (size > 0) {
        bw_write_bits(writer, amplitude(difference, size), size);
    }

    // AC: (run, size) symbol followed by the amplitude.
    int run = 0;
    for (int i = 1; i < 64; i++) {
        if (zigzag[i] == 0) {
            run++;
            continue;
        }

        while (run > 15) {
            bw_write_bits(writer, ac_table->code[0xF0],
                          ac_table->length[0xF0]);
            run -= 16;
        }

        size = category(zigzag[i]);
        int symbol = (run << 4) | size;
        bw_write_bits(writer, ac_table->code[symbol],
                      ac_table->length[symbol]);
        bw_write_bits(writer, amplitude(zigzag[i], size), size);
        run = 0;
    }

    if (run > 0) {
        bw_write_bits(writer, ac_table->code[0x00],
                      ac_table->length[0x00]);
    }
}

// ============================================================
// ENCODE A WHOLE PLANE, BLOCK BY BLOCK
// ============================================================

static long encode_plane(BitWriter *writer, Plane *plane,
                         int quantization[8][8],
                         HuffTable *dc_table, HuffTable *ac_table) {
    int blocks_x = (plane->width + 7) / 8;
    int blocks_y = (plane->height + 7) / 8;
    int previous_dc = 0; // The DC predictor resets for each plane.

    for (int block_y = 0; block_y < blocks_y; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            double block[8][8];
            for (int row = 0; row < 8; row++) {
                for (int column = 0; column < 8; column++) {
                    block[row][column] = (double)plane_sample(
                        plane, block_x * 8 + column, block_y * 8 + row);
                }
            }
            encode_block(writer, block, quantization,
                         dc_table, ac_table, &previous_dc);
        }
    }

    return (long)blocks_x * blocks_y;
}

// ============================================================
// ENCODE THE WHOLE IMAGE
// ============================================================

int encode_image(Plane *y, Plane *cb, Plane *cr, int quality,
                 unsigned char **out_buffer, EncodeStats *stats) {
    init_dct_tables();

    HuffTable dc_luma;
    HuffTable ac_luma;
    HuffTable dc_chroma;
    HuffTable ac_chroma;
    build_huffman(BITS_DC_LUMA, VAL_DC_LUMA, &dc_luma);
    build_huffman(BITS_AC_LUMA, VAL_AC_LUMA, &ac_luma);
    build_huffman(BITS_DC_CHROMA, VAL_DC_CHROMA, &dc_chroma);
    build_huffman(BITS_AC_CHROMA, VAL_AC_CHROMA, &ac_chroma);

    int quant_luma[8][8];
    int quant_chroma[8][8];
    scale_table(Q_LUMA_BASE, quality, quant_luma);
    scale_table(Q_CHROMA_BASE, quality, quant_chroma);

    BitWriter writer;
    if (!bw_init(&writer, 1 << 16)) {
        return 0;
    }

    stats->blocks_y = encode_plane(
        &writer, y, quant_luma, &dc_luma, &ac_luma);
    stats->blocks_cb = encode_plane(
        &writer, cb, quant_chroma, &dc_chroma, &ac_chroma);
    stats->blocks_cr = encode_plane(
        &writer, cr, quant_chroma, &dc_chroma, &ac_chroma);

    bw_flush(&writer);

    stats->data_size = writer.size;
    *out_buffer = writer.buffer;
    return 1;
}

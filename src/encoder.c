#include <math.h>
#include <stdlib.h>

#include "dct.h"
#include "encoder.h"
#include "tables.h"

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
    int failed;
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
    writer->failed = 0;
    return 1;
}

static void bw_write_bits(BitWriter *writer, int value, int count) {
    if (writer->failed) {
        return;
    }

    for (int i = count - 1; i >= 0; i--) {
        writer->accumulator =
            (writer->accumulator << 1) | ((value >> i) & 1);
        writer->bit_count++;

        if (writer->bit_count == 8) {
            if (writer->size >= writer->capacity) {
                long new_capacity = writer->capacity * 2;
                unsigned char *new_buffer = (unsigned char *)realloc(
                    writer->buffer, (size_t)new_capacity);
                if (!new_buffer) {
                    writer->failed = 1;
                    return;
                }
                writer->buffer = new_buffer;
                writer->capacity = new_capacity;
            }

            writer->buffer[writer->size++] =
                (unsigned char)writer->accumulator;
            writer->accumulator = 0;
            writer->bit_count = 0;
        }
    }
}

static void bw_flush(BitWriter *writer) {
    while (!writer->failed && writer->bit_count != 0) {
        bw_write_bits(writer, 0, 1);
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
    int previous_dc = 0;

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
            if (writer->failed) {
                return -1;
            }
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

    if (stats->blocks_y < 0 || stats->blocks_cb < 0 ||
        stats->blocks_cr < 0 || writer.failed) {
        free(writer.buffer);
        return 0;
    }

    bw_flush(&writer);
    if (writer.failed) {
        free(writer.buffer);
        return 0;
    }

    stats->data_size = writer.size;
    *out_buffer = writer.buffer;
    return 1;
}

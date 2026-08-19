#include <stdlib.h>
#include <string.h>

#include "dct.h"
#include "decoder.h"
#include "tables.h"

// ============================================================
// HUFFMAN TABLES FOR DECODING
// ============================================================

typedef struct {
    int min_code[17];
    int max_code[17];
    int value_index[17];
    const unsigned char *values;
} HuffDecodeTable;

static void build_decode_table(const int bits[17],
                               const unsigned char *values,
                               HuffDecodeTable *table) {
    table->values = values;
    int code = 0;
    int value_index = 0;

    for (int length = 1; length <= 16; length++) {
        if (bits[length] == 0) {
            table->min_code[length] = 0;
            table->max_code[length] = -1;
            table->value_index[length] = 0;
        } else {
            table->value_index[length] = value_index;
            table->min_code[length] = code;
            code += bits[length];
            value_index += bits[length];
            table->max_code[length] = code - 1;
        }
        code <<= 1;
    }
}

// ============================================================
// BIT READER
// ============================================================

typedef struct {
    unsigned char *buffer;
    long size;
    long byte_position;
    int bit_position;
    int failed;
} BitReader;

static void br_init(BitReader *reader, unsigned char *buffer, long size) {
    reader->buffer = buffer;
    reader->size = size;
    reader->byte_position = 0;
    reader->bit_position = 0;
    reader->failed = 0;
}

static int br_next_bit(BitReader *reader) {
    if (reader->byte_position >= reader->size) {
        reader->failed = 1;
        return 0;
    }

    int bit =
        (reader->buffer[reader->byte_position] >>
         (7 - reader->bit_position)) & 1;
    reader->bit_position++;

    if (reader->bit_position == 8) {
        reader->bit_position = 0;
        reader->byte_position++;
    }

    return bit;
}

static int br_read_symbol(BitReader *reader, HuffDecodeTable *table) {
    int code = br_next_bit(reader);

    for (int length = 1; length <= 16 && !reader->failed; length++) {
        if (table->max_code[length] >= 0 &&
            code >= table->min_code[length] &&
            code <= table->max_code[length]) {
            int offset = table->value_index[length] +
                         (code - table->min_code[length]);
            return table->values[offset];
        }
        code = (code << 1) | br_next_bit(reader);
    }

    return -1;
}

static int br_read_amplitude(BitReader *reader, int size) {
    if (size == 0) {
        return 0;
    }

    int value = 0;
    for (int i = 0; i < size; i++) {
        value = (value << 1) | br_next_bit(reader);
    }

    if (value < (1 << (size - 1))) {
        value += 1 - (1 << size);
    }
    return value;
}

// ============================================================
// DECODE ONE 8x8 BLOCK
// ============================================================

static int decode_block(BitReader *reader, double block[8][8],
                        int quantization[8][8],
                        HuffDecodeTable *dc_table,
                        HuffDecodeTable *ac_table,
                        int *previous_dc) {
    int zigzag[64];
    memset(zigzag, 0, sizeof(zigzag));

    // DC: restore the difference and add it to the previous value.
    int size = br_read_symbol(reader, dc_table);
    if (size < 0 || size > 11) {
        return 0;
    }
    int difference = br_read_amplitude(reader, size);
    if (reader->failed) {
        return 0;
    }
    zigzag[0] = *previous_dc + difference;
    *previous_dc = zigzag[0];

    // AC: expand (run, size) pairs into the remaining 63 slots.
    int position = 1;
    while (position < 64) {
        int symbol = br_read_symbol(reader, ac_table);
        if (symbol < 0) {
            return 0;
        }
        if (symbol == 0x00) {
            break; // EOB: all remaining values stay zero.
        }
        if (symbol == 0xF0) {
            position += 16; // ZRL: exactly 16 zero coefficients.
            if (position > 64) {
                return 0;
            }
            continue;
        }

        int run = symbol >> 4;
        size = symbol & 0x0F;
        if (size == 0) {
            return 0;
        }

        position += run;
        if (position >= 64) {
            return 0;
        }

        zigzag[position] = br_read_amplitude(reader, size);
        if (reader->failed) {
            return 0;
        }
        position++;
    }

    // Undo quantization and zig-zag ordering in one pass.
    double coefficients[8][8];
    for (int i = 0; i < 64; i++) {
        int index = ZIGZAG[i];
        int row = index / 8;
        int column = index % 8;
        coefficients[row][column] =
            (double)(zigzag[i] * quantization[row][column]);
    }

    idct_block(coefficients, block);
    return 1;
}

static long decode_plane(BitReader *reader, Plane *plane,
                         int quantization[8][8],
                         HuffDecodeTable *dc_table,
                         HuffDecodeTable *ac_table) {
    int blocks_x = (plane->width + 7) / 8;
    int blocks_y = (plane->height + 7) / 8;
    int previous_dc = 0;

    for (int block_y = 0; block_y < blocks_y; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            double block[8][8];
            if (!decode_block(reader, block, quantization,
                              dc_table, ac_table, &previous_dc)) {
                return -1;
            }

            // Copy real pixels and discard encoded padding outside the plane.
            for (int row = 0; row < 8; row++) {
                for (int column = 0; column < 8; column++) {
                    int x = block_x * 8 + column;
                    int y = block_y * 8 + row;
                    if (x >= plane->width || y >= plane->height) {
                        continue;
                    }

                    double value = block[row][column];
                    if (value < 0.0) {
                        value = 0.0;
                    }
                    if (value > 255.0) {
                        value = 255.0;
                    }
                    plane->data[(long)y * plane->width + x] =
                        (unsigned char)(value + 0.5);
                }
            }
        }
    }

    return (long)blocks_x * blocks_y;
}

int decode_image(unsigned char *buffer, long buffer_size,
                 int width, int height, int quality, int chroma,
                 Plane *y, Plane *cb, Plane *cr, DecodeStats *stats) {
    if (!buffer || buffer_size <= 0 || width <= 0 || height <= 0 ||
        (chroma != 0 && chroma != 2)) {
        return 0;
    }

    init_dct_tables();

    HuffDecodeTable dc_luma;
    HuffDecodeTable ac_luma;
    HuffDecodeTable dc_chroma;
    HuffDecodeTable ac_chroma;
    build_decode_table(BITS_DC_LUMA, VAL_DC_LUMA, &dc_luma);
    build_decode_table(BITS_AC_LUMA, VAL_AC_LUMA, &ac_luma);
    build_decode_table(BITS_DC_CHROMA, VAL_DC_CHROMA, &dc_chroma);
    build_decode_table(BITS_AC_CHROMA, VAL_AC_CHROMA, &ac_chroma);

    int quant_luma[8][8];
    int quant_chroma[8][8];
    scale_table(Q_LUMA_BASE, quality, quant_luma);
    scale_table(Q_CHROMA_BASE, quality, quant_chroma);

    int chroma_width = (chroma == 2) ? (width + 1) / 2 : width;
    int chroma_height = (chroma == 2) ? (height + 1) / 2 : height;

    *y = create_plane(width, height);
    *cb = create_plane(chroma_width, chroma_height);
    *cr = create_plane(chroma_width, chroma_height);
    if (!y->data || !cb->data || !cr->data) {
        free_plane(y);
        free_plane(cb);
        free_plane(cr);
        return 0;
    }

    BitReader reader;
    br_init(&reader, buffer, buffer_size);

    stats->blocks_y = decode_plane(
        &reader, y, quant_luma, &dc_luma, &ac_luma);
    stats->blocks_cb = decode_plane(
        &reader, cb, quant_chroma, &dc_chroma, &ac_chroma);
    stats->blocks_cr = decode_plane(
        &reader, cr, quant_chroma, &dc_chroma, &ac_chroma);

    if (stats->blocks_y < 0 || stats->blocks_cb < 0 ||
        stats->blocks_cr < 0 || reader.failed) {
        free_plane(y);
        free_plane(cb);
        free_plane(cr);
        return 0;
    }

    stats->bytes_consumed =
        reader.byte_position + (reader.bit_position > 0 ? 1 : 0);
    return 1;
}

// ============================================================
// YCbCr TO RGB WITH CHROMA UPSAMPLING
// ============================================================

static unsigned char clamp_byte(double value) {
    if (value < 0.0) {
        return 0;
    }
    if (value > 255.0) {
        return 255;
    }
    return (unsigned char)(value + 0.5);
}

int merge_channels(Plane *y, Plane *cb, Plane *cr, Image *output) {
    int width = y->width;
    int height = y->height;

    if (width <= 0 || height <= 0 || !y->data || !cb->data || !cr->data ||
        cb->width != cr->width || cb->height != cr->height) {
        return 0;
    }

    output->width = width;
    output->height = height;
    output->pixels = (unsigned char *)malloc(
        (size_t)((long)width * height * 3));
    if (!output->pixels) {
        return 0;
    }

    int scale_x = (cb->width < width) ? 2 : 1;
    int scale_y = (cb->height < height) ? 2 : 1;

    for (int pixel_y = 0; pixel_y < height; pixel_y++) {
        for (int pixel_x = 0; pixel_x < width; pixel_x++) {
            double luma = y->data[(long)pixel_y * width + pixel_x];

            // Nearest-neighbour upsampling reuses one chroma value for
            // every pixel in its original 2x2 group.
            int chroma_x = pixel_x / scale_x;
            int chroma_y = pixel_y / scale_y;
            double blue_difference =
                plane_sample(cb, chroma_x, chroma_y) - 128.0;
            double red_difference =
                plane_sample(cr, chroma_x, chroma_y) - 128.0;

            double red = luma + 1.402 * red_difference;
            double green = luma - 0.344136 * blue_difference -
                           0.714136 * red_difference;
            double blue = luma + 1.772 * blue_difference;

            long index = ((long)pixel_y * width + pixel_x) * 3;
            output->pixels[index + 0] = clamp_byte(red);
            output->pixels[index + 1] = clamp_byte(green);
            output->pixels[index + 2] = clamp_byte(blue);
        }
    }

    return 1;
}

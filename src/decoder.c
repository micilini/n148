#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dct.h"
#include "decoder.h"
#include "header.h"
#include "huffman.h"
#include "tables.h"

// ============================================================
// HUFFMAN TABLES FOR DECODING
// ============================================================

#define LOOKUP_BITS 10

typedef struct {
    int min_code[17];
    int max_code[17];
    int value_index[17];
    const unsigned char *values;
    unsigned char fast_symbol[1 << LOOKUP_BITS];
    unsigned char fast_length[1 << LOOKUP_BITS];
} HuffDecodeTable;

static void build_decode_table(const HuffSpec *spec,
                               HuffDecodeTable *table) {
    table->values = spec->values;
    int code = 0;
    int value_index = 0;

    for (int length = 1; length <= 16; length++) {
        if (spec->bits[length] == 0) {
            table->min_code[length] = 0;
            table->max_code[length] = -1;
            table->value_index[length] = 0;
        } else {
            table->value_index[length] = value_index;
            table->min_code[length] = code;
            code += spec->bits[length];
            value_index += spec->bits[length];
            table->max_code[length] = code - 1;
        }
        code <<= 1;
    }

    // Every slot whose prefix matches a short code gets filled in.
    // A code of length L covers 2^(LOOKUP_BITS - L) consecutive slots,
    // because the bits after it can be anything.
    memset(table->fast_length, 0, sizeof(table->fast_length));

    code = 0;
    value_index = 0;
    for (int length = 1; length <= 16; length++) {
        for (int i = 0; i < spec->bits[length];
             i++, value_index++, code++) {
            if (length <= LOOKUP_BITS) {
                int shift = LOOKUP_BITS - length;
                int start = code << shift;
                int end = start + (1 << shift);
                for (int slot = start; slot < end; slot++) {
                    table->fast_symbol[slot] = spec->values[value_index];
                    table->fast_length[slot] = (unsigned char)length;
                }
            }
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
    uint64_t accumulator;
    int available;
    int failed;
} BitReader;

static inline uint64_t load_be64(const unsigned char *source) {
    uint64_t value;
    memcpy(&value, source, sizeof(value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#else
    return ((uint64_t)source[0] << 56) |
           ((uint64_t)source[1] << 48) |
           ((uint64_t)source[2] << 40) |
           ((uint64_t)source[3] << 32) |
           ((uint64_t)source[4] << 24) |
           ((uint64_t)source[5] << 16) |
           ((uint64_t)source[6] << 8) |
           (uint64_t)source[7];
#endif
}

// Tops the accumulator back up. Near the end of the buffer it falls
// back to byte at a time so it never reads past the data.
static inline void br_refill(BitReader *reader) {
    if (reader->available > 56) {
        return;
    }

    if (reader->byte_position + 8 <= reader->size) {
        uint64_t next = load_be64(reader->buffer + reader->byte_position);
        reader->accumulator |= next >> reader->available;
        reader->byte_position += (63 - reader->available) >> 3;
        reader->available |= 56;
        return;
    }

    while (reader->available <= 56 &&
           reader->byte_position < reader->size) {
        reader->accumulator |=
            (uint64_t)reader->buffer[reader->byte_position++]
            << (56 - reader->available);
        reader->available += 8;
    }
}

static inline unsigned int br_peek(BitReader *reader, int count) {
    return (unsigned int)(reader->accumulator >> (64 - count));
}

static inline void br_skip(BitReader *reader, int count) {
    if (count > reader->available) {
        reader->failed = 1;
        return;
    }

    reader->accumulator <<= count;
    reader->available -= count;
}

static void br_init(BitReader *reader, unsigned char *buffer, long size) {
    reader->buffer = buffer;
    reader->size = size;
    reader->byte_position = 0;
    reader->accumulator = 0;
    reader->available = 0;
    reader->failed = 0;
}

static int br_next_bit(BitReader *reader) {
    br_refill(reader);
    if (reader->available == 0) {
        reader->failed = 1;
        return 0;
    }

    int bit = (int)(reader->accumulator >> 63);
    br_skip(reader, 1);
    return bit;
}

static int br_read_symbol(BitReader *reader, HuffDecodeTable *table) {
    br_refill(reader);

    // One shot: peek at the next ten bits and see whether a short
    // code already claims that prefix.
    unsigned int window = br_peek(reader, LOOKUP_BITS);
    int length = table->fast_length[window];
    if (length) {
        br_skip(reader, length);
        return table->fast_symbol[window];
    }

    // Long code: walk the canonical ranges one length at a time.
    int code = br_next_bit(reader);

    for (length = 1; length <= 16 && !reader->failed; length++) {
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

// The fast inverse transform expects its input already multiplied by
// the AAN constants, so those are folded into the dequantization
// table exactly as they were folded into the encoder's table.
typedef struct {
    float multiplier[64];
} ScaledDequant;

static void build_scaled_dequant(int quantization[8][8], ScaledDequant *out) {
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            out->multiplier[row * 8 + column] = (float)(
                quantization[row][column] *
                aan_scale_factor(row, column) / 64.0);
        }
    }
}

static int decode_block(BitReader *reader, float block[64],
                        const ScaledDequant *quantization,
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
    int nonzero_ac = 0;
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
        nonzero_ac = 1;
        position++;
    }

    // A block whose AC coefficients all vanished reconstructs to a
    // single flat value: the transform of a constant is a constant.
    // At lower qualities this covers a large share of the image, and
    // skipping the transform there is free speed.
    if (!nonzero_ac) {
        float flat = zigzag[0] * quantization->multiplier[0] + 128.0f;
        if (flat < 0.0f) {
            flat = 0.0f;
        }
        if (flat > 255.0f) {
            flat = 255.0f;
        }
        for (int i = 0; i < 64; i++) {
            block[i] = flat;
        }
        return 1;
    }

    // Undo quantization and zig-zag ordering in one pass.
    float coefficients[64];
    for (int i = 0; i < 64; i++) {
        int index = ZIGZAG[i];
        coefficients[index] = zigzag[i] * quantization->multiplier[index];
    }

    idct_block_fast(coefficients, block);
    return 1;
}

static long decode_plane(BitReader *reader, Plane *plane,
                         const ScaledDequant *quantization,
                         HuffDecodeTable *dc_table,
                         HuffDecodeTable *ac_table) {
    int blocks_x = (plane->width + 7) / 8;
    int blocks_y = (plane->height + 7) / 8;
    int previous_dc = 0;

    for (int block_y = 0; block_y < blocks_y; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++) {
            float block[64];
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

                    float value = block[row * 8 + column];
                    if (value < 0.0f) {
                        value = 0.0f;
                    }
                    if (value > 255.0f) {
                        value = 255.0f;
                    }
                    plane->data[(long)y * plane->width + x] =
                        (unsigned char)(value + 0.5f);
                }
            }
        }
    }

    return (long)blocks_x * blocks_y;
}

int decode_image(unsigned char *buffer, long buffer_size,
                 int width, int height, int quality, int chroma,
                 const HuffSpec specs[HUFFMAN_TABLE_COUNT],
                 Plane *y, Plane *cb, Plane *cr, DecodeStats *stats) {
    if (!buffer || buffer_size <= 0 || width <= 0 || height <= 0 ||
        !specs || !y || !cb || !cr || !stats ||
        (chroma != CHROMA_444 && chroma != CHROMA_422 &&
         chroma != CHROMA_420)) {
        return 0;
    }

    HuffDecodeTable dc_luma;
    HuffDecodeTable ac_luma;
    HuffDecodeTable dc_chroma;
    HuffDecodeTable ac_chroma;
    for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
        if (!huffman_spec_is_valid(&specs[table])) {
            return 0;
        }
    }
    build_decode_table(&specs[HUFFMAN_DC_LUMA], &dc_luma);
    build_decode_table(&specs[HUFFMAN_AC_LUMA], &ac_luma);
    build_decode_table(&specs[HUFFMAN_DC_CHROMA], &dc_chroma);
    build_decode_table(&specs[HUFFMAN_AC_CHROMA], &ac_chroma);

    int quant_luma[8][8];
    int quant_chroma[8][8];
    scale_table(Q_LUMA_BASE, quality, quant_luma);
    scale_table(Q_CHROMA_BASE, quality, quant_chroma);

    ScaledDequant scaled_luma;
    ScaledDequant scaled_chroma;
    build_scaled_dequant(quant_luma, &scaled_luma);
    build_scaled_dequant(quant_chroma, &scaled_chroma);

    int chroma_width;
    int chroma_height;
    chroma_dimensions(chroma, width, height,
                      &chroma_width, &chroma_height);

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
        &reader, y, &scaled_luma, &dc_luma, &ac_luma);
    stats->blocks_cb = decode_plane(
        &reader, cb, &scaled_chroma, &dc_chroma, &ac_chroma);
    stats->blocks_cr = decode_plane(
        &reader, cr, &scaled_chroma, &dc_chroma, &ac_chroma);

    if (stats->blocks_y < 0 || stats->blocks_cb < 0 ||
        stats->blocks_cr < 0 || reader.failed) {
        free_plane(y);
        free_plane(cb);
        free_plane(cr);
        return 0;
    }

    stats->bytes_consumed =
        reader.byte_position - reader.available / 8;
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

#define CHROMA_SHIFT 4

static void expand_chroma_row(Plane *plane, int py, int dst_width,
                              int stretch_x, int stretch_y,
                              unsigned short *dst) {
    int near_y = stretch_y ? (py >> 1) : py;
    int far_y = stretch_y
        ? ((py & 1) ? near_y + 1 : near_y - 1)
        : near_y;

    for (int x = 0; x < dst_width; x++) {
        int near_x = stretch_x ? (x >> 1) : x;
        int far_x = stretch_x
            ? ((x & 1) ? near_x + 1 : near_x - 1)
            : near_x;

        int a = plane_sample(plane, near_x, near_y);
        int b = plane_sample(plane, far_x, near_y);
        int c = plane_sample(plane, near_x, far_y);
        int d = plane_sample(plane, far_x, far_y);

        // The division is deliberately not performed here. Keeping the
        // value multiplied by 16 preserves its fractional part until the
        // colour conversion has used it.
        dst[x] = (unsigned short)(9 * a + 3 * b + 3 * c + d);
    }
}

int merge_channels(Plane *y, Plane *cb, Plane *cr,
                   int smooth, Image *output) {
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

    unsigned short *row_cb = NULL;
    unsigned short *row_cr = NULL;
    if (smooth) {
        row_cb = (unsigned short *)malloc(
            (size_t)width * sizeof(unsigned short));
        row_cr = (unsigned short *)malloc(
            (size_t)width * sizeof(unsigned short));
        if (!row_cb || !row_cr) {
            free(row_cb);
            free(row_cr);
            free(output->pixels);
            output->pixels = NULL;
            return 0;
        }
    }

    double scale_x = (double)cb->width / width;
    double scale_y = (double)cb->height / height;
    double chroma_scale = 1.0 / (1 << CHROMA_SHIFT);
    int stretch_x = cb->width != width;
    int stretch_y = cb->height != height;

    for (int pixel_y = 0; pixel_y < height; pixel_y++) {
        if (smooth) {
            expand_chroma_row(cb, pixel_y, width,
                              stretch_x, stretch_y, row_cb);
            expand_chroma_row(cr, pixel_y, width,
                              stretch_x, stretch_y, row_cr);
        }

        for (int pixel_x = 0; pixel_x < width; pixel_x++) {
            double luma = y->data[(long)pixel_y * width + pixel_x];

            double blue_difference;
            double red_difference;
            if (smooth) {
                // The 1/16 is applied here, in floating point, so the
                // fractional part survives all the way to the colour
                // conversion.
                blue_difference =
                    row_cb[pixel_x] * chroma_scale - 128.0;
                red_difference =
                    row_cr[pixel_x] * chroma_scale - 128.0;
            } else {
                int chroma_x = (int)(pixel_x * scale_x);
                int chroma_y = (int)(pixel_y * scale_y);
                blue_difference =
                    plane_sample(cb, chroma_x, chroma_y) - 128.0;
                red_difference =
                    plane_sample(cr, chroma_x, chroma_y) - 128.0;
            }

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

    free(row_cb);
    free(row_cr);
    return 1;
}

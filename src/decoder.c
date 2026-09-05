#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dct.h"
#include "decoder.h"
#include "header.h"
#include "huffman.h"
#include "tables.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

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

#define CHROMA_SHIFT 4
#define YCC_FIX_BITS 16
#define YCC_FIX(x)   ((int)((x) * (1 << YCC_FIX_BITS) + 0.5))
#define YCC_RGB_SHIFT (YCC_FIX_BITS + CHROMA_SHIFT)
#define YCC_RGB_HALF  (1 << (YCC_RGB_SHIFT - 1))
#define YCC_CENTER     (128 << CHROMA_SHIFT)

static const int YCC_RCR = YCC_FIX(1.402000);
static const int YCC_GCB = YCC_FIX(0.344136);
static const int YCC_GCR = YCC_FIX(0.714136);
static const int YCC_BCB = YCC_FIX(1.772000);

static unsigned char clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (unsigned char)value;
}

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

static void expand_nearest_chroma_row(Plane *plane, int py, int dst_width,
                                      double scale_x, double scale_y,
                                      unsigned short *dst) {
    int source_y = (int)(py * scale_y);
    for (int x = 0; x < dst_width; x++) {
        int source_x = (int)(x * scale_x);
        dst[x] = (unsigned short)(
            plane_sample(plane, source_x, source_y) << CHROMA_SHIFT);
    }
}

static void ycbcr_to_rgb_scalar(const unsigned char *y,
                                const unsigned short *cb,
                                const unsigned short *cr,
                                unsigned char *output,
                                int count) {
    for (int i = 0; i < count; i++) {
        int luma = y[i] << YCC_RGB_SHIFT;
        int blue_difference = (int)cb[i] - YCC_CENTER;
        int red_difference = (int)cr[i] - YCC_CENTER;

        int red = (luma + YCC_RCR * red_difference + YCC_RGB_HALF) >>
                  YCC_RGB_SHIFT;
        int green = (luma - YCC_GCB * blue_difference -
                     YCC_GCR * red_difference + YCC_RGB_HALF) >>
                    YCC_RGB_SHIFT;
        int blue = (luma + YCC_BCB * blue_difference + YCC_RGB_HALF) >>
                   YCC_RGB_SHIFT;

        output[i * 3 + 0] = clamp_byte(red);
        output[i * 3 + 1] = clamp_byte(green);
        output[i * 3 + 2] = clamp_byte(blue);
    }
}

#if defined(__x86_64__) || defined(__i386__)

// Converts eight pixels per iteration. Each channel is packed separately,
// spread into its RGB byte positions with pshufb, then combined with OR.
__attribute__((target("avx2")))
static int ycbcr_to_rgb_avx2(const unsigned char *y,
                             const unsigned short *cb,
                             const unsigned short *cr,
                             unsigned char *output,
                             int count) {
    const __m128i mr0 = _mm_setr_epi8(
         0,-1,-1, 1,-1,-1, 2,-1,-1, 3,-1,-1, 4,-1,-1, 5);
    const __m128i mg0 = _mm_setr_epi8(
        -1, 0,-1,-1, 1,-1,-1, 2,-1,-1, 3,-1,-1, 4,-1,-1);
    const __m128i mb0 = _mm_setr_epi8(
        -1,-1, 0,-1,-1, 1,-1,-1, 2,-1,-1, 3,-1,-1, 4,-1);
    const __m128i mr1 = _mm_setr_epi8(
        -1,-1, 6,-1,-1, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1);
    const __m128i mg1 = _mm_setr_epi8(
         5,-1,-1, 6,-1,-1, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1);
    const __m128i mb1 = _mm_setr_epi8(
        -1, 5,-1,-1, 6,-1,-1, 7,-1,-1,-1,-1,-1,-1,-1,-1);

    const __m256i center = _mm256_set1_epi32(YCC_CENTER);
    const __m256i rcr = _mm256_set1_epi32(YCC_RCR);
    const __m256i gcb = _mm256_set1_epi32(YCC_GCB);
    const __m256i gcr = _mm256_set1_epi32(YCC_GCR);
    const __m256i bcb = _mm256_set1_epi32(YCC_BCB);
    const __m256i half = _mm256_set1_epi32(YCC_RGB_HALF);

    int i = 0;
    // The second interleaved store writes eight useful bytes through a
    // 16-byte store, so the final group is finished by the scalar tail.
    for (; i + 16 <= count; i += 8) {
        __m128i y_bytes = _mm_loadl_epi64((const __m128i *)(y + i));
        __m128i cb_words = _mm_loadu_si128((const __m128i *)(cb + i));
        __m128i cr_words = _mm_loadu_si128((const __m128i *)(cr + i));

        __m256i luma = _mm256_slli_epi32(
            _mm256_cvtepu8_epi32(y_bytes), YCC_RGB_SHIFT);
        __m256i blue_difference = _mm256_sub_epi32(
            _mm256_cvtepu16_epi32(cb_words), center);
        __m256i red_difference = _mm256_sub_epi32(
            _mm256_cvtepu16_epi32(cr_words), center);

        __m256i red = _mm256_add_epi32(
            _mm256_add_epi32(
                luma, _mm256_mullo_epi32(red_difference, rcr)),
            half);
        __m256i green = _mm256_add_epi32(
            _mm256_sub_epi32(
                _mm256_sub_epi32(
                    luma, _mm256_mullo_epi32(blue_difference, gcb)),
                _mm256_mullo_epi32(red_difference, gcr)),
            half);
        __m256i blue = _mm256_add_epi32(
            _mm256_add_epi32(
                luma, _mm256_mullo_epi32(blue_difference, bcb)),
            half);

        red = _mm256_srai_epi32(red, YCC_RGB_SHIFT);
        green = _mm256_srai_epi32(green, YCC_RGB_SHIFT);
        blue = _mm256_srai_epi32(blue, YCC_RGB_SHIFT);

        __m128i red16 = _mm_packs_epi32(
            _mm256_castsi256_si128(red),
            _mm256_extracti128_si256(red, 1));
        __m128i green16 = _mm_packs_epi32(
            _mm256_castsi256_si128(green),
            _mm256_extracti128_si256(green, 1));
        __m128i blue16 = _mm_packs_epi32(
            _mm256_castsi256_si128(blue),
            _mm256_extracti128_si256(blue, 1));
        __m128i red8 = _mm_packus_epi16(red16, red16);
        __m128i green8 = _mm_packus_epi16(green16, green16);
        __m128i blue8 = _mm_packus_epi16(blue16, blue16);

        unsigned char *out = output + i * 3;
        _mm_storeu_si128(
            (__m128i *)out,
            _mm_or_si128(
                _mm_or_si128(_mm_shuffle_epi8(red8, mr0),
                              _mm_shuffle_epi8(green8, mg0)),
                _mm_shuffle_epi8(blue8, mb0)));
        _mm_storeu_si128(
            (__m128i *)(out + 16),
            _mm_or_si128(
                _mm_or_si128(_mm_shuffle_epi8(red8, mr1),
                              _mm_shuffle_epi8(green8, mg1)),
                _mm_shuffle_epi8(blue8, mb1)));
    }

    return i;
}

#endif

static void ycbcr_to_rgb(const unsigned char *y,
                         const unsigned short *cb,
                         const unsigned short *cr,
                         unsigned char *output,
                         int count) {
    int converted = 0;
#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        converted = ycbcr_to_rgb_avx2(y, cb, cr, output, count);
    }
#endif
    ycbcr_to_rgb_scalar(y + converted, cb + converted, cr + converted,
                        output + converted * 3, count - converted);
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

    unsigned short *row_cb = (unsigned short *)malloc(
        (size_t)width * sizeof(unsigned short));
    unsigned short *row_cr = (unsigned short *)malloc(
        (size_t)width * sizeof(unsigned short));
    if (!row_cb || !row_cr) {
        free(row_cb);
        free(row_cr);
        free(output->pixels);
        output->pixels = NULL;
        return 0;
    }

    double scale_x = (double)cb->width / width;
    double scale_y = (double)cb->height / height;
    int stretch_x = cb->width != width;
    int stretch_y = cb->height != height;

    for (int pixel_y = 0; pixel_y < height; pixel_y++) {
        if (smooth) {
            expand_chroma_row(cb, pixel_y, width,
                              stretch_x, stretch_y, row_cb);
            expand_chroma_row(cr, pixel_y, width,
                              stretch_x, stretch_y, row_cr);
        } else {
            expand_nearest_chroma_row(cb, pixel_y, width,
                                      scale_x, scale_y, row_cb);
            expand_nearest_chroma_row(cr, pixel_y, width,
                                      scale_x, scale_y, row_cr);
        }

        ycbcr_to_rgb(y->data + (long)pixel_y * width,
                     row_cb, row_cr,
                     output->pixels + (long)pixel_y * width * 3,
                     width);
    }

    free(row_cb);
    free(row_cr);
    return 1;
}

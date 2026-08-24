#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dct.h"
#include "encoder.h"
#include "huffman.h"
#include "tables.h"

typedef struct {
    int code[256];
    int length[256];
} HuffTable;

static void build_huffman(const HuffSpec *spec, HuffTable *table) {
    for (int i = 0; i < 256; i++) {
        table->code[i] = 0;
        table->length[i] = 0;
    }

    int code = 0;
    int value_index = 0;
    for (int length = 1; length <= 16; length++) {
        for (int i = 0; i < spec->bits[length]; i++) {
            unsigned char symbol = spec->values[value_index++];
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
    uint64_t accumulator;
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

// Pushing one bit at a time costs a shift, a mask and a test for
// every single bit: a twelve bit code becomes twelve iterations. Here
// a 64 bit accumulator swallows whole codes in one shift, and memory
// is only touched once at least four bytes are ready to leave.
static void bw_write_bits(BitWriter *writer, unsigned int value, int count) {
    if (writer->failed || count <= 0) {
        return;
    }

    writer->accumulator = (writer->accumulator << count)
                        | (value & ((count >= 32) ? 0xFFFFFFFFu
                                                  : ((1u << count) - 1u)));
    writer->bit_count += count;

    if (writer->bit_count >= 32) {
        if (writer->size + 4 > writer->capacity) {
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

        writer->bit_count -= 32;
        unsigned int word =
            (unsigned int)(writer->accumulator >> writer->bit_count);
        word = __builtin_bswap32(word);
        memcpy(writer->buffer + writer->size, &word, 4);
        writer->size += 4;
    }
}

static void bw_flush(BitWriter *writer) {
    if (writer->failed || writer->bit_count == 0) {
        return;
    }

    int byte_count = (writer->bit_count + 7) / 8;
    int padding = byte_count * 8 - writer->bit_count;
    writer->accumulator <<= padding;

    if (writer->size + byte_count > writer->capacity) {
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

    for (int byte = byte_count - 1; byte >= 0; byte--) {
        writer->buffer[writer->size++] =
            (unsigned char)(writer->accumulator >> (byte * 8));
    }

    writer->accumulator = 0;
    writer->bit_count = 0;
}

// ============================================================
// CATEGORY AND AMPLITUDE
// ============================================================

// How many bits a value needs. The loop version reads well but runs
// once per surviving coefficient; counting leading zeros gets the
// same answer in a single instruction on any modern processor.
static inline int category(int value) {
    unsigned int magnitude = (unsigned int)(value < 0 ? -value : value);
    if (magnitude == 0) {
        return 0;
    }
#if defined(__GNUC__) || defined(__clang__)
    return 32 - __builtin_clz(magnitude);
#else
    int size = 0;
    while (magnitude) {
        size++;
        magnitude >>= 1;
    }
    return size;
#endif
}

static int amplitude(int value, int size) {
    int mask = (1 << size) - 1;
    return (value < 0) ? ((value - 1) & mask) : (value & mask);
}

// Both passes consume the exact same token stream. Pass one only counts
// symbols; pass two writes the symbol and its amplitude bits.
typedef struct {
    int symbol;
    int extra_bits;
    int extra_value;
    int is_dc;
} Token;

// The fast transform leaves every coefficient multiplied by a known
// constant. Rather than dividing it back out on every block, the
// constant is folded into the quantization table once, at startup.
// Quantizing then becomes a multiplication instead of a division.
typedef struct {
    float reciprocal[64];
} ScaledQuant;

static void build_scaled_quant(int quantization[8][8], ScaledQuant *out) {
    for (int row = 0; row < 8; row++) {
        for (int column = 0; column < 8; column++) {
            out->reciprocal[row * 8 + column] = (float)(1.0 /
                (quantization[row][column] * aan_scale_factor(row, column)));
        }
    }
}

static void quantize_block(const float block[64],
                           const ScaledQuant *quantization,
                           short zigzag[64]) {
    float coefficients[64];
    dct_block_fast(block, coefficients);

    for (int i = 0; i < 64; i++) {
        int index = ZIGZAG[i];
        zigzag[i] = (short)lrintf(
            coefficients[index] * quantization->reciprocal[index]);
    }
}

// ============================================================
// COEFFICIENT CACHE
// ============================================================
//
// The costly part of a block is the transform, not the bookkeeping.
// Running it once and keeping the quantized result lets the counting
// pass and the writing pass share the same work instead of each
// redoing the mathematics from the pixels.

typedef struct {
    short *coefficients;    // 64 values per block, zig-zag order
    long   count;
} CoeffCache;

static int cache_fill(CoeffCache *cache, Plane *plane,
                      const ScaledQuant *quantization) {
    int blocks_x = (plane->width + 7) / 8;
    int blocks_y = (plane->height + 7) / 8;
    long total = (long)blocks_x * blocks_y;

    cache->count = total;
    cache->coefficients = (short *)malloc((size_t)total * 64 * sizeof(short));
    if (!cache->coefficients) {
        return 0;
    }

    long index = 0;
    for (int block_y = 0; block_y < blocks_y; block_y++) {
        for (int block_x = 0; block_x < blocks_x; block_x++, index++) {
            float block[64];
            for (int row = 0; row < 8; row++) {
                for (int column = 0; column < 8; column++) {
                    block[row * 8 + column] = (float)plane_sample(
                        plane, block_x * 8 + column, block_y * 8 + row);
                }
            }
            quantize_block(block, quantization,
                           &cache->coefficients[index * 64]);
        }
    }
    return 1;
}

static void cache_free(CoeffCache *cache) {
    free(cache->coefficients);
    cache->coefficients = NULL;
    cache->count = 0;
}

static int tokenize_block(const short zigzag[64],
                          int *previous_dc, Token tokens[64]) {
    // DC: differential pulse-code modulation.
    int difference = zigzag[0] - *previous_dc;
    *previous_dc = zigzag[0];
    int size = category(difference);
    if (size > 15) {
        return -1;
    }
    int token_count = 0;
    tokens[token_count++] = (Token){
        size, size, amplitude(difference, size), 1
    };

    // AC: (run, size) symbol followed by the amplitude.
    int run = 0;
    for (int i = 1; i < 64; i++) {
        if (zigzag[i] == 0) {
            run++;
            continue;
        }

        while (run > 15) {
            tokens[token_count++] = (Token){0xF0, 0, 0, 0};
            run -= 16;
        }

        size = category(zigzag[i]);
        if (size > 15 || token_count >= 64) {
            return -1;
        }
        int symbol = (run << 4) | size;
        tokens[token_count++] = (Token){
            symbol, size, amplitude(zigzag[i], size), 0
        };
        run = 0;
    }

    if (run > 0) {
        if (token_count >= 64) {
            return -1;
        }
        tokens[token_count++] = (Token){0x00, 0, 0, 0};
    }

    return token_count;
}

// ============================================================
// PROCESS A WHOLE COEFFICIENT CACHE, BLOCK BY BLOCK
// ============================================================

static long process_cache(const CoeffCache *cache,
                          int dc_table_index, int ac_table_index,
                          long frequencies[HUFFMAN_TABLE_COUNT][256],
                          BitWriter *writer,
                          HuffTable tables[HUFFMAN_TABLE_COUNT]) {
    int previous_dc = 0;

    for (long block = 0; block < cache->count; block++) {
        Token tokens[64];
        int token_count = tokenize_block(
            &cache->coefficients[block * 64], &previous_dc, tokens);
        if (token_count < 0) {
            return -1;
        }

        for (int i = 0; i < token_count; i++) {
            Token *token = &tokens[i];
            int table_index = token->is_dc
                ? dc_table_index : ac_table_index;

            if (frequencies) {
                frequencies[table_index][token->symbol]++;
                continue;
            }

            HuffTable *table = &tables[table_index];
            int length = table->length[token->symbol];
            if (length == 0) {
                writer->failed = 1;
                return -1;
            }
            bw_write_bits(writer, (unsigned int)table->code[token->symbol],
                          length);
            if (token->extra_bits > 0) {
                bw_write_bits(writer, (unsigned int)token->extra_value,
                              token->extra_bits);
            }
            if (writer->failed) {
                return -1;
            }
        }
    }

    return cache->count;
}

// ============================================================
// ENCODE THE WHOLE IMAGE
// ============================================================

int encode_image(Plane *y, Plane *cb, Plane *cr, int quality, int optimize,
                 HuffSpec specs[HUFFMAN_TABLE_COUNT],
                 unsigned char **out_buffer, EncodeStats *stats) {
    if (!y || !cb || !cr || !y->data || !cb->data || !cr->data ||
        !specs || !out_buffer || !stats) {
        return 0;
    }

    *out_buffer = NULL;
    memset(stats, 0, sizeof(*stats));

    int quant_luma[8][8];
    int quant_chroma[8][8];
    scale_table(Q_LUMA_BASE, quality, quant_luma);
    scale_table(Q_CHROMA_BASE, quality, quant_chroma);

    ScaledQuant scaled_luma;
    ScaledQuant scaled_chroma;
    build_scaled_quant(quant_luma, &scaled_luma);
    build_scaled_quant(quant_chroma, &scaled_chroma);

    CoeffCache cache_y = {0};
    CoeffCache cache_cb = {0};
    CoeffCache cache_cr = {0};
    if (!cache_fill(&cache_y, y, &scaled_luma) ||
        !cache_fill(&cache_cb, cb, &scaled_chroma) ||
        !cache_fill(&cache_cr, cr, &scaled_chroma)) {
        cache_free(&cache_y);
        cache_free(&cache_cb);
        cache_free(&cache_cr);
        return 0;
    }

    if (optimize) {
        long frequencies[HUFFMAN_TABLE_COUNT][256] = {{0}};
        if (process_cache(&cache_y,
                          HUFFMAN_DC_LUMA, HUFFMAN_AC_LUMA,
                          frequencies, NULL, NULL) < 0 ||
            process_cache(&cache_cb,
                          HUFFMAN_DC_CHROMA, HUFFMAN_AC_CHROMA,
                          frequencies, NULL, NULL) < 0 ||
            process_cache(&cache_cr,
                          HUFFMAN_DC_CHROMA, HUFFMAN_AC_CHROMA,
                          frequencies, NULL, NULL) < 0) {
            cache_free(&cache_y);
            cache_free(&cache_cb);
            cache_free(&cache_cr);
            return 0;
        }

        for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
            if (!huffman_build_optimized(frequencies[table],
                                         &specs[table])) {
                cache_free(&cache_y);
                cache_free(&cache_cb);
                cache_free(&cache_cr);
                return 0;
            }
        }
        stats->table_size = huffman_tables_size(specs);
    } else {
        huffman_default_specs(specs);
    }

    HuffTable tables[HUFFMAN_TABLE_COUNT];
    for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
        build_huffman(&specs[table], &tables[table]);
    }

    BitWriter writer;
    if (!bw_init(&writer, 1 << 16)) {
        cache_free(&cache_y);
        cache_free(&cache_cb);
        cache_free(&cache_cr);
        return 0;
    }

    stats->blocks_y = process_cache(
        &cache_y, HUFFMAN_DC_LUMA, HUFFMAN_AC_LUMA,
        NULL, &writer, tables);
    stats->blocks_cb = process_cache(
        &cache_cb, HUFFMAN_DC_CHROMA, HUFFMAN_AC_CHROMA,
        NULL, &writer, tables);
    stats->blocks_cr = process_cache(
        &cache_cr, HUFFMAN_DC_CHROMA, HUFFMAN_AC_CHROMA,
        NULL, &writer, tables);

    cache_free(&cache_y);
    cache_free(&cache_cb);
    cache_free(&cache_cr);

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

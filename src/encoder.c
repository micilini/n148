#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dct.h"
#include "encoder.h"
#include "huffman.h"
#include "parallel.h"
#include "tables.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

typedef struct {
    unsigned int code[256];
    unsigned char length[256];
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
            table->length[symbol] = (unsigned char)length;
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

#if defined(__x86_64__) || defined(__i386__)

// Quantizing in natural order keeps every load contiguous, and the values
// are narrowed before zig-zag reordering so the scattered pass moves half
// as many bytes. Gather is deliberately avoided because its real-codec
// performance varies considerably between processors.
__attribute__((target("avx2")))
static void quantize_block_avx2(const float coefficients[64],
                                const float reciprocal[64],
                                short zigzag[64]) {
    short natural[64];
    for (int i = 0; i < 64; i += 16) {
        __m256i low = _mm256_cvtps_epi32(
            _mm256_mul_ps(_mm256_loadu_ps(coefficients + i),
                          _mm256_loadu_ps(reciprocal + i)));
        __m256i high = _mm256_cvtps_epi32(
            _mm256_mul_ps(_mm256_loadu_ps(coefficients + i + 8),
                          _mm256_loadu_ps(reciprocal + i + 8)));

        __m256i packed = _mm256_packs_epi32(low, high);
        // packs works within 128-bit lanes, so restore natural order.
        packed = _mm256_permute4x64_epi64(packed, 0xD8);
        _mm256_storeu_si256((__m256i *)(natural + i), packed);
    }

    for (int i = 0; i < 64; i++) {
        zigzag[i] = natural[ZIGZAG[i]];
    }
}

#endif

static uint64_t quantize_block(const float block[64],
                               const ScaledQuant *quantization,
                               short zigzag[64]) {
    float coefficients[64];
    dct_block_fast(block, coefficients);

#if defined(__x86_64__) || defined(__i386__)
    if (n148_cpu_level() >= N148_CPU_AVX2) {
        quantize_block_avx2(coefficients, quantization->reciprocal, zigzag);
    } else
#endif
    {
        for (int i = 0; i < 64; i++) {
            int index = ZIGZAG[i];
            zigzag[i] = (short)lrintf(
                coefficients[index] * quantization->reciprocal[index]);
        }
    }

    uint64_t nonzero = 0;
    for (int i = 1; i < 64; i++) {
        if (zigzag[i] != 0) {
            nonzero |= UINT64_C(1) << i;
        }
    }
    return nonzero;
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
    uint64_t *nonzero;      // AC positions that survived quantization
    long   count;
} CoeffCache;

#if defined(__x86_64__) || defined(__i386__)

// Loads an interior 8x8 patch one row at a time. Edge blocks stay on the
// scalar sampler because they need coordinate clamping for their padding.
__attribute__((target("avx2")))
static void load_block_avx2(const unsigned char *source, int stride,
                            float block[64]) {
    for (int row = 0; row < 8; row++, source += stride) {
        __m128i bytes = _mm_loadl_epi64((const __m128i *)source);
        _mm256_storeu_ps(
            block + row * 8,
            _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(bytes)));
    }
}

#endif

static int count_ac_frequencies(const short *zigzag, uint64_t mask,
                                long frequencies[256]) {
    int previous = 0;
    while (mask) {
        int position = __builtin_ctzll(mask);
        int run = position - previous - 1;
        while (run > 15) {
            frequencies[0xF0]++;
            run -= 16;
        }

        int size = category(zigzag[position]);
        if (size > 15) {
            return 0;
        }
        frequencies[(run << 4) | size]++;
        previous = position;
        mask &= mask - 1;
    }
    if (previous < 63) {
        frequencies[0x00]++;
    }
    return 1;
}

typedef struct {
    CoeffCache *cache;
    Plane *plane;
    const ScaledQuant *quantization;
    int blocks_x;
    int use_avx2;
    int failed;
    long (*ac_freq)[256];
} QuantJob;

// Handles one horizontal band of blocks. Bands never overlap, so workers
// write to disjoint slices of the coefficient and nonzero-mask buffers.
static void quantize_rows(long start, long end, int worker, void *context) {
    QuantJob *job = (QuantJob *)context;
    Plane *plane = job->plane;

    for (long block_y = start; block_y < end; block_y++) {
        for (int block_x = 0; block_x < job->blocks_x; block_x++) {
            long index = block_y * job->blocks_x + block_x;
            float block[64];
            int interior = (block_x + 1) * 8 <= plane->width &&
                           (block_y + 1) * 8 <= plane->height;
#if defined(__x86_64__) || defined(__i386__)
            if (interior && job->use_avx2) {
                load_block_avx2(
                    plane->data + block_y * 8 * plane->width + block_x * 8,
                    plane->width, block);
            } else
#endif
            {
                for (int row = 0; row < 8; row++) {
                    for (int column = 0; column < 8; column++) {
                        block[row * 8 + column] = (float)plane_sample(
                            plane, block_x * 8 + column,
                            (int)block_y * 8 + row);
                    }
                }
            }

            short *zigzag = &job->cache->coefficients[index * 64];
            uint64_t mask = quantize_block(block, job->quantization, zigzag);
            job->cache->nonzero[index] = mask;
            if (job->ac_freq &&
                !count_ac_frequencies(zigzag, mask, job->ac_freq[worker])) {
#if defined(__GNUC__) || defined(__clang__)
                __atomic_store_n(&job->failed, 1, __ATOMIC_RELAXED);
#else
                job->failed = 1;
#endif
            }
        }
    }
}

static int cache_fill(CoeffCache *cache, Plane *plane,
                      const ScaledQuant *quantization,
                      long (*ac_freq)[256]) {
    int blocks_x = (plane->width + 7) / 8;
    int blocks_y = (plane->height + 7) / 8;
    long total = (long)blocks_x * blocks_y;

    cache->count = total;
    cache->coefficients = (short *)malloc((size_t)total * 64 * sizeof(short));
    cache->nonzero = (uint64_t *)malloc((size_t)total * sizeof(uint64_t));
    if (!cache->coefficients || !cache->nonzero) {
        free(cache->coefficients);
        free(cache->nonzero);
        cache->coefficients = NULL;
        cache->nonzero = NULL;
        return 0;
    }

    QuantJob job = {
        cache, plane, quantization, blocks_x,
        n148_cpu_level() >= N148_CPU_AVX2, 0, ac_freq
    };
    n148_parallel_for(blocks_y, quantize_rows, &job);
#if defined(__GNUC__) || defined(__clang__)
    int failed = __atomic_load_n(&job.failed, __ATOMIC_RELAXED);
#else
    int failed = job.failed;
#endif
    if (failed) {
        free(cache->coefficients);
        free(cache->nonzero);
        cache->coefficients = NULL;
        cache->nonzero = NULL;
        return 0;
    }
    return 1;
}

static void cache_free(CoeffCache *cache) {
    free(cache->coefficients);
    free(cache->nonzero);
    cache->coefficients = NULL;
    cache->nonzero = NULL;
    cache->count = 0;
}

// ============================================================
// PROCESS A WHOLE COEFFICIENT CACHE, BLOCK BY BLOCK
// ============================================================

typedef struct {
    unsigned int bits;
    int count;
} AcBitBatch;

static inline void ac_batch_flush(BitWriter *writer, AcBitBatch *batch) {
    if (batch->count > 0) {
        bw_write_bits(writer, batch->bits, batch->count);
        batch->bits = 0;
        batch->count = 0;
    }
}

static inline void ac_batch_put(BitWriter *writer, AcBitBatch *batch,
                                unsigned int value, int count) {
    if (batch->count > 0 && batch->count + count > 32) {
        ac_batch_flush(writer, batch);
    }

    if (batch->count == 0) {
        batch->bits = value;
        batch->count = count;
    } else {
        batch->bits = (batch->bits << count) | value;
        batch->count += count;
    }
}

static inline void write_ac_sparse(BitWriter *writer, const short *zigzag,
                                   uint64_t mask,
                                   const HuffTable *ac_table) {
    int previous = 0;
    while (mask) {
        int position = __builtin_ctzll(mask);
        int run = position - previous - 1;
        while (run > 15) {
            int length = ac_table->length[0xF0];
            if (length == 0) {
                writer->failed = 1;
                return;
            }
            bw_write_bits(writer, ac_table->code[0xF0], length);
            run -= 16;
        }

        int value = zigzag[position];
        int coefficient_size = category(value);
        if (coefficient_size > 15) {
            writer->failed = 1;
            return;
        }
        int symbol = (run << 4) | coefficient_size;
        int length = ac_table->length[symbol];
        if (length == 0) {
            writer->failed = 1;
            return;
        }
        bw_write_bits(writer,
                      (ac_table->code[symbol] << coefficient_size) |
                          (unsigned int)amplitude(value, coefficient_size),
                      length + coefficient_size);
        previous = position;
        mask &= mask - 1;
    }
    if (previous < 63) {
        int length = ac_table->length[0x00];
        if (length == 0) {
            writer->failed = 1;
            return;
        }
        bw_write_bits(writer, ac_table->code[0x00], length);
    }
}

static inline void write_ac_dense(BitWriter *writer, const short *zigzag,
                                  uint64_t mask,
                                  const HuffTable *ac_table) {
    AcBitBatch batch = {0, 0};
    int previous = 0;
    while (mask) {
        int position = __builtin_ctzll(mask);
        int run = position - previous - 1;
        while (run > 15) {
            int length = ac_table->length[0xF0];
            if (length == 0) {
                writer->failed = 1;
                return;
            }
            ac_batch_put(writer, &batch, ac_table->code[0xF0], length);
            run -= 16;
        }

        int value = zigzag[position];
        int coefficient_size = category(value);
        if (coefficient_size > 15) {
            writer->failed = 1;
            return;
        }
        int symbol = (run << 4) | coefficient_size;
        int length = ac_table->length[symbol];
        if (length == 0) {
            writer->failed = 1;
            return;
        }
        ac_batch_put(writer, &batch,
                     (ac_table->code[symbol] << coefficient_size) |
                         (unsigned int)amplitude(value, coefficient_size),
                     length + coefficient_size);
        previous = position;
        mask &= mask - 1;
    }
    if (previous < 63) {
        int length = ac_table->length[0x00];
        if (length == 0) {
            writer->failed = 1;
            return;
        }
        ac_batch_put(writer, &batch, ac_table->code[0x00], length);
    }
    ac_batch_flush(writer, &batch);
}

static int count_dc_frequencies(const CoeffCache *cache,
                                long frequencies[256]) {
    int previous_dc = 0;
    for (long block = 0; block < cache->count; block++) {
        const short *zigzag = &cache->coefficients[block * 64];
        int difference = zigzag[0] - previous_dc;
        previous_dc = zigzag[0];
        int size = category(difference);
        if (size > 15) {
            return 0;
        }
        frequencies[size]++;
    }
    return 1;
}

static void reduce_ac_frequencies(long destination[256],
                                  long (*workers)[256]) {
    for (int worker = 0; worker < N148_MAX_WORKERS; worker++) {
        for (int symbol = 0; symbol < 256; symbol++) {
            destination[symbol] += workers[worker][symbol];
        }
    }
}

static long process_cache(const CoeffCache *cache,
                          int dc_table_index, int ac_table_index,
                          BitWriter *writer,
                          HuffTable tables[HUFFMAN_TABLE_COUNT]) {
    int previous_dc = 0;

    for (long block = 0; block < cache->count; block++) {
        const short *zigzag = &cache->coefficients[block * 64];
        int difference = zigzag[0] - previous_dc;
        previous_dc = zigzag[0];
        int dc_size = category(difference);
        if (dc_size > 15) {
            return -1;
        }

        HuffTable *dc_table = &tables[dc_table_index];
        int dc_length = dc_table->length[dc_size];
        if (dc_length == 0) {
            writer->failed = 1;
            return -1;
        }
        bw_write_bits(writer,
                      (dc_table->code[dc_size] << dc_size) |
                          (unsigned int)amplitude(difference, dc_size),
                      dc_length + dc_size);

        uint64_t mask = cache->nonzero[block];
        if (__builtin_popcountll(mask) <= 8) {
            write_ac_sparse(writer, zigzag, mask, &tables[ac_table_index]);
        } else {
            write_ac_dense(writer, zigzag, mask, &tables[ac_table_index]);
        }
        if (writer->failed) {
            return -1;
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
    long (*luma_ac)[256] = NULL;
    long (*chroma_ac)[256] = NULL;
    if (optimize) {
        luma_ac = (long (*)[256])calloc(N148_MAX_WORKERS,
                                        sizeof(*luma_ac));
        chroma_ac = (long (*)[256])calloc(N148_MAX_WORKERS,
                                          sizeof(*chroma_ac));
        if (!luma_ac || !chroma_ac) {
            free(luma_ac);
            free(chroma_ac);
            return 0;
        }
    }

    if (!cache_fill(&cache_y, y, &scaled_luma, luma_ac) ||
        !cache_fill(&cache_cb, cb, &scaled_chroma, chroma_ac) ||
        !cache_fill(&cache_cr, cr, &scaled_chroma, chroma_ac)) {
        cache_free(&cache_y);
        cache_free(&cache_cb);
        cache_free(&cache_cr);
        free(luma_ac);
        free(chroma_ac);
        return 0;
    }

    if (optimize) {
        long frequencies[HUFFMAN_TABLE_COUNT][256] = {{0}};
        reduce_ac_frequencies(frequencies[HUFFMAN_AC_LUMA], luma_ac);
        reduce_ac_frequencies(frequencies[HUFFMAN_AC_CHROMA], chroma_ac);
        if (!count_dc_frequencies(&cache_y,
                                  frequencies[HUFFMAN_DC_LUMA]) ||
            !count_dc_frequencies(&cache_cb,
                                  frequencies[HUFFMAN_DC_CHROMA]) ||
            !count_dc_frequencies(&cache_cr,
                                  frequencies[HUFFMAN_DC_CHROMA])) {
            cache_free(&cache_y);
            cache_free(&cache_cb);
            cache_free(&cache_cr);
            free(luma_ac);
            free(chroma_ac);
            return 0;
        }

        for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
            if (!huffman_build_optimized(frequencies[table],
                                         &specs[table])) {
                cache_free(&cache_y);
                cache_free(&cache_cb);
                cache_free(&cache_cr);
                free(luma_ac);
                free(chroma_ac);
                return 0;
            }
        }
        stats->table_size = huffman_tables_size(specs);
    } else {
        huffman_default_specs(specs);
    }
    free(luma_ac);
    free(chroma_ac);

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
        &writer, tables);
    stats->blocks_cb = process_cache(
        &cache_cb, HUFFMAN_DC_CHROMA, HUFFMAN_AC_CHROMA,
        &writer, tables);
    stats->blocks_cr = process_cache(
        &cache_cr, HUFFMAN_DC_CHROMA, HUFFMAN_AC_CHROMA,
        &writer, tables);

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

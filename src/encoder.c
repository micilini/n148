#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "encoder.h"
#include "tables.h"
#include "dct.h"
#include "cpu.h"
#include "parallel.h"

// ============================================================
// HUFFMAN TABLES FOR ENCODING
// ============================================================

typedef struct {
    unsigned int code[256];
    unsigned int len[256];
} HuffTable;

static void build_huffman(const HuffSpec *spec, HuffTable *table) {
    for (int i = 0; i < 256; i++) { table->code[i] = 0; table->len[i] = 0; }
    unsigned int code = 0;
    int k = 0;
    for (int length = 1; length <= 16; length++) {
        for (int i = 0; i < spec->bits[length]; i++) {
            unsigned char symbol = spec->values[k++];
            table->code[symbol] = code;
            table->len[symbol]  = length;
            code++;
        }
        code <<= 1;
    }
}

// ============================================================
// BIT WRITER
// ============================================================
//
// Writing one bit at a time is simple but wasteful: a 12 bit code
// costs 12 shifts, 12 masks and 12 branches. Here we keep a 64 bit
// accumulator, drop whole codes into it in a single shift, and only
// touch memory when at least one full byte is ready. That turns the
// inner loop from "per bit" into "per symbol".

typedef struct {
    unsigned char *buffer;
    long capacity;
    long size;
    unsigned long long accumulator;   // bits waiting to be written
    int filled;                       // how many bits the accumulator holds
} BitWriter;

static int bw_init(BitWriter *writer, long capacity) {
    writer->buffer = (unsigned char *) malloc(capacity);
    if (!writer->buffer) return 0;
    writer->capacity = capacity;
    writer->size = 0;
    writer->accumulator = 0;
    writer->filled = 0;
    return 1;
}

static int bw_reserve(BitWriter *writer, long extra) {
    if (writer->size + extra <= writer->capacity) return 1;
    long wanted = writer->capacity * 2;
    while (wanted < writer->size + extra) wanted *= 2;
    unsigned char *grown = (unsigned char *) realloc(writer->buffer, wanted);
    if (!grown) return 0;
    writer->buffer = grown;
    writer->capacity = wanted;
    return 1;
}

// Pushes up to 32 bits at once.
// Emitting the accumulator one byte at a time costs a loop with a
// shift, a store and a test per byte. Once at least four bytes are
// ready they can leave together: take the top 32 bits, put them in
// big endian order and store the whole word.
//
// Callers must have reserved room beforehand (see write_cache), which
// keeps the capacity check out of the inner loop entirely.
static inline void bw_write_bits(BitWriter *writer, unsigned int value, int count) {
    if (count <= 0) return;

    writer->accumulator = (writer->accumulator << count)
                        | (value & ((count >= 32) ? 0xFFFFFFFFu
                                                  : ((1u << count) - 1u)));
    writer->filled += count;

    if (writer->filled >= 32) {
        writer->filled -= 32;
        unsigned int word = (unsigned int)(writer->accumulator >> writer->filled);
#if defined(__GNUC__) || defined(__clang__)
        word = __builtin_bswap32(word);
#else
        word = ((word & 0xFFu) << 24) | ((word & 0xFF00u) << 8) |
               ((word >> 8) & 0xFF00u) | ((word >> 24) & 0xFFu);
#endif
        memcpy(writer->buffer + writer->size, &word, 4);
        writer->size += 4;
    }
}

static void bw_flush(BitWriter *writer) {
    bw_reserve(writer, 8);
    while (writer->filled >= 8) {
        writer->filled -= 8;
        writer->buffer[writer->size++] =
            (unsigned char)(writer->accumulator >> writer->filled);
    }
    if (writer->filled > 0) {                       // pad the last byte
        writer->buffer[writer->size++] =
            (unsigned char)(writer->accumulator << (8 - writer->filled));
        writer->filled = 0;
    }
}

// ============================================================
// CATEGORY / AMPLITUDE
// ============================================================

// How many bits a value needs. The loop version is easy to read but
// runs once per coefficient; counting leading zeros gets the same
// answer in a single instruction on any modern CPU.
static inline int category(int value) {
    unsigned int magnitude = (unsigned int)(value < 0 ? -value : value);
    if (magnitude == 0) return 0;
#if defined(__GNUC__) || defined(__clang__)
    return 32 - __builtin_clz(magnitude);
#else
    int size = 0;
    while (magnitude) { size++; magnitude >>= 1; }
    return size;
#endif
}

static inline unsigned int amplitude(int value, int size) {
    unsigned int mask = (1u << size) - 1u;
    return (value < 0) ? (((unsigned int)(value - 1)) & mask)
                       : (((unsigned int) value) & mask);
}

// ============================================================
// PRE-SCALED QUANTIZATION TABLE
// ============================================================
//
// The fast DCT leaves each coefficient multiplied by a constant.
// Rather than dividing it back out for every block, the constant is
// folded into the table once, and quantizing becomes a multiply.

// The reciprocals are stored in ZIG-ZAG order. That way the
// quantizer can walk its output sequentially and pick up the
// coefficients with a gather, instead of quantizing in natural order
// and then shuffling 64 values around afterwards.
typedef struct { float recip[64]; } FastQuant;

static void build_fast_quant(int quantization[8][8], FastQuant *out) {
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++)
            out->recip[u*8+v] =
                (float)(1.0 / (quantization[u][v] * aan_scale_factor(u, v)));
}

// ============================================================
// COEFFICIENT CACHE
// ============================================================
//
// The costly part of a block is the transform, not the bookkeeping.
// Running it once and storing the quantized result lets the symbol
// counting pass and the bit writing pass share the work.

typedef struct {
    short *coefficients;    // 64 quantized values per block, zig-zag order
    unsigned long long *nonzero_masks;
    unsigned char *nonzero_counts;
    long   count;
} CoeffCache;

static void cache_release(CoeffCache *cache) {
    free(cache->coefficients);
    cache->coefficients = NULL;
    cache->nonzero_masks = NULL;
    cache->nonzero_counts = NULL;
}

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

// Quantizes straight into zig-zag order: the gather picks the eight
// coefficients that belong at this point in the sequence, and the
// reciprocals are already lined up with them. Packing to 16 bits
// finishes the block without a single scalar pass.
__attribute__((target("avx2")))
static void quantize_avx2(const float coef[64], const float recip[64],
                          const int zigzag[64], short out[64]) {
    // Quantizing in natural order keeps every load contiguous, which
    // matters: a hardware gather looked tempting here but measured
    // more than twice as slow as a plain multiply followed by a
    // sixteen bit reorder. The values are narrowed to short first, so
    // the reordering pass moves half as many bytes.
    short natural[64];
    for (int i = 0; i < 64; i += 16) {
        __m256i low = _mm256_cvtps_epi32(
            _mm256_mul_ps(_mm256_loadu_ps(coef + i), _mm256_loadu_ps(recip + i)));
        __m256i high = _mm256_cvtps_epi32(
            _mm256_mul_ps(_mm256_loadu_ps(coef + i + 8), _mm256_loadu_ps(recip + i + 8)));

        __m256i packed = _mm256_packs_epi32(low, high);
        // packs works within 128 bit lanes, so the halves need one
        // more shuffle to come out in sequence.
        packed = _mm256_permute4x64_epi64(packed, 0xD8);
        _mm256_storeu_si256((__m256i *)(natural + i), packed);
    }

    for (int i = 0; i < 64; i++) out[i] = natural[zigzag[i]];
}

// Loads an 8x8 patch of bytes as floats, one row per instruction.
__attribute__((target("avx2")))
static void load_block_avx2(const unsigned char *src, int stride, float block[64]) {
    for (int r = 0; r < 8; r++, src += stride) {
        __m128i bytes = _mm_loadl_epi64((const __m128i *) src);
        _mm256_storeu_ps(block + r*8,
                         _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(bytes)));
    }
}
#endif

static void quantize_scalar(const float coef[64], const float recip[64],
                            const int zigzag[64], short out[64]) {
    short natural[64];
    for (int i = 0; i < 64; i++) natural[i] = (short) lrintf(coef[i] * recip[i]);
    for (int i = 0; i < 64; i++) out[i] = natural[zigzag[i]];
}

// ============================================================
// NON-ZERO SCAN
// ============================================================
//
// A quantized block is mostly zeros, yet both passes still walked all
// 63 AC slots looking for the few that survived. Building a 64 bit
// mask of the non-zero positions lets the loops jump straight from
// one coefficient to the next: the run length is simply the distance
// between two set bits.

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static unsigned long long nonzero_mask_avx2(const short *zz) {
    unsigned long long mask = 0;
    const __m256i zero = _mm256_setzero_si256();
    for (int group = 0; group < 4; group++) {
        __m256i values = _mm256_loadu_si256((const __m256i *)(zz + group * 16));
        __m256i is_zero = _mm256_cmpeq_epi16(values, zero);
        __m128i packed = _mm_packs_epi16(_mm256_castsi256_si128(is_zero),
                                         _mm256_extracti128_si256(is_zero, 1));
        unsigned int bits = (unsigned int) _mm_movemask_epi8(packed);
        mask |= ((unsigned long long)((~bits) & 0xFFFFu)) << (group * 16);
    }
    return mask;
}
#endif

static unsigned long long nonzero_mask_scalar(const short *zz) {
    unsigned long long mask = 0;
    for (int i = 0; i < 64; i++)
        if (zz[i]) mask |= 1ull << i;
    return mask;
}

static inline unsigned long long nonzero_mask(const short *zz, int use_avx2) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2) return nonzero_mask_avx2(zz);
#else
    (void) use_avx2;
#endif
    return nonzero_mask_scalar(zz);
}

#define MAX_WORKERS 32

typedef struct {
    Plane *plane;
    const FastQuant *quant;
    short *coefficients;
    unsigned long long *nonzero_masks;
    unsigned char *nonzero_counts;
    int blocks_x;
    int use_avx2;
    int collect;                     // also gather AC statistics
    long (*ac_freq)[256];            // one row per worker, merged later
} QuantJob;

// Handles one horizontal band of blocks. Bands never overlap, so the
// workers write to disjoint slices of the output buffer.
static void quantize_rows(long start, long end, int worker, void *context) {
    QuantJob *job = (QuantJob *) context;
    Plane *plane = job->plane;

    for (long by = start; by < end; by++) {
        for (int bx = 0; bx < job->blocks_x; bx++) {
            float block[64], coef[64];

            if ((bx + 1) * 8 <= plane->width && (by + 1) * 8 <= plane->height) {
                const unsigned char *src =
                    plane->data + (long)(by * 8) * plane->width + bx * 8;
#if defined(__x86_64__) || defined(__i386__)
                if (job->use_avx2) load_block_avx2(src, plane->width, block);
                else
#endif
                for (int r = 0; r < 8; r++, src += plane->width)
                    for (int c = 0; c < 8; c++)
                        block[r*8+c] = (float) src[c];
            } else {
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        block[r*8+c] = (float) plane_sample(plane, bx*8 + c,
                                                            (int)(by*8) + r);
            }

#if defined(__x86_64__) || defined(__i386__)
            if (job->use_avx2) dct_block_avx2(block, coef);
            else
#endif
            dct_block_fast(block, coef);

            long block_index = by * job->blocks_x + bx;
            short *out = job->coefficients + block_index * 64;
#if defined(__x86_64__) || defined(__i386__)
            if (job->use_avx2) quantize_avx2(coef, job->quant->recip, ZIGZAG, out);
            else
#endif
            quantize_scalar(coef, job->quant->recip, ZIGZAG, out);

            // The AC statistics do not depend on block order, so they
            // can be gathered right here while the coefficients are
            // still in cache. Counting them later would mean reading
            // the whole coefficient buffer back from memory.
            unsigned long long nonzero = nonzero_mask(out, job->use_avx2);
            job->nonzero_masks[block_index] = nonzero;

            if (job->collect) {
                long *freq = job->ac_freq[worker];
                unsigned long long mask = nonzero & ~1ull;
                unsigned char count = 0;
                int previous = 0;
                while (mask) {
                    int position = __builtin_ctzll(mask);
                    int run = position - previous - 1;
                    while (run > 15) { freq[0xF0]++; run -= 16; }
                    freq[(run << 4) | category(out[position])]++;
                    previous = position;
                    count++;
                    mask &= mask - 1;
                }
                if (previous < 63) freq[0x00]++;
                job->nonzero_counts[block_index] = count;
            }
        }
    }
}

static long quantize_plane(Plane *plane, const FastQuant *quant, CoeffCache *cache,
                           int collect, long (*ac_freq)[256]) {
    int blocks_x = (plane->width  + 7) / 8;
    int blocks_y = (plane->height + 7) / 8;
    long total = (long) blocks_x * blocks_y;

    cache->count = total;
    size_t coefficient_bytes = (size_t) total * 64 * sizeof(short);
    size_t mask_bytes = (size_t) total * sizeof(*cache->nonzero_masks);
    size_t count_bytes = collect ? (size_t) total : 0;
    cache->coefficients = (short *) malloc(coefficient_bytes + mask_bytes +
                                           count_bytes);
    if (!cache->coefficients) return 0;
    cache->nonzero_masks = (unsigned long long *)
        ((unsigned char *) cache->coefficients + coefficient_bytes);
    cache->nonzero_counts = collect
        ? (unsigned char *) cache->nonzero_masks + mask_bytes
        : NULL;

    QuantJob job;
    job.plane = plane;
    job.quant = quant;
    job.coefficients = cache->coefficients;
    job.nonzero_masks = cache->nonzero_masks;
    job.nonzero_counts = cache->nonzero_counts;
    job.blocks_x = blocks_x;
    job.collect = collect;
    job.ac_freq = ac_freq;
#if defined(__x86_64__) || defined(__i386__)
    job.use_avx2 = (n148_cpu_level() >= N148_CPU_AVX2);
#else
    job.use_avx2 = 0;
#endif

    n148_parallel_for(blocks_y, quantize_rows, &job);
    return total;
}

// ============================================================
// SYMBOL GENERATION
// ============================================================
//
// Both passes read the cached coefficients through this routine, so
// they can never disagree about which symbols the block produces.



// Only the DC symbols are left for a sequential pass: they depend on
// the running predictor, so their order matters. One read per block.
static void count_dc(const CoeffCache *cache, long dc_freq[256]) {
    int dc_previous = 0;
    for (long b = 0; b < cache->count; b++) {
        int dc = cache->coefficients[b * 64];
        dc_freq[category(dc - dc_previous)]++;
        dc_previous = dc;
    }
}

// Sparse blocks are cheapest when each surviving coefficient is sent straight
// to the bit writer.  Dense blocks have enough short entropy tokens to profit
// from joining several of them into one 32-bit write.  Both paths walk the same
// cached non-zero mask and emit the exact same bits: this is an implementation
// choice, not a bitstream mode.
#ifndef N148_DENSE_AC_THRESHOLD
#define N148_DENSE_AC_THRESHOLD 8
#endif

static inline void write_ac_sparse(BitWriter *writer, const short *zz,
                                   unsigned long long mask,
                                   const HuffTable *ac_table) {
    int previous = 0;
    while (mask) {
        int position = __builtin_ctzll(mask);
        int run = position - previous - 1;
        while (run > 15) {
            bw_write_bits(writer, ac_table->code[0xF0], ac_table->len[0xF0]);
            run -= 16;
        }
        int value = zz[position];
        int coefficient_size = category(value);
        int symbol = (run << 4) | coefficient_size;
        bw_write_bits(writer,
                      (ac_table->code[symbol] << coefficient_size)
                          | amplitude(value, coefficient_size),
                      (int) ac_table->len[symbol] + coefficient_size);
        previous = position;
        mask &= mask - 1;
    }
    if (previous < 63)
        bw_write_bits(writer, ac_table->code[0x00], ac_table->len[0x00]);
}

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
    if (batch->count > 0 && batch->count + count > 32)
        ac_batch_flush(writer, batch);

    if (batch->count == 0) {
        batch->bits = value;
        batch->count = count;
    } else {
        batch->bits = (batch->bits << count) | value;
        batch->count += count;
    }
}

static inline void write_ac_dense(BitWriter *writer, const short *zz,
                                  unsigned long long mask,
                                  const HuffTable *ac_table) {
    AcBitBatch batch = {0, 0};
    int previous = 0;
    while (mask) {
        int position = __builtin_ctzll(mask);
        int run = position - previous - 1;
        while (run > 15) {
            ac_batch_put(writer, &batch,
                         ac_table->code[0xF0], ac_table->len[0xF0]);
            run -= 16;
        }
        int value = zz[position];
        int coefficient_size = category(value);
        int symbol = (run << 4) | coefficient_size;
        ac_batch_put(writer, &batch,
                     (ac_table->code[symbol] << coefficient_size)
                         | amplitude(value, coefficient_size),
                     (int) ac_table->len[symbol] + coefficient_size);
        previous = position;
        mask &= mask - 1;
    }
    if (previous < 63)
        ac_batch_put(writer, &batch,
                     ac_table->code[0x00], ac_table->len[0x00]);
    ac_batch_flush(writer, &batch);
}

static long write_cache(BitWriter *writer, const CoeffCache *cache,
                        const HuffTable *dc_table, const HuffTable *ac_table) {
    int dc_previous = 0;

    // Worst case for one block: 64 coefficients, each at most a 16 bit
    // code plus a 16 bit payload, so 256 bytes covers it with room to
    // spare. Reserving once per block lets the writer skip the check.
    const long block_margin = 288;

    for (long b = 0; b < cache->count; b++) {
        if (writer->size + block_margin > writer->capacity) {
            if (!bw_reserve(writer, block_margin)) return b;
        }
        const short *zz = &cache->coefficients[b * 64];

        // --- DC ---
        int difference = zz[0] - dc_previous;
        dc_previous = zz[0];
        int size = category(difference);
        bw_write_bits(writer,
                      (dc_table->code[size] << size) | amplitude(difference, size),
                      (int) dc_table->len[size] + size);

        // --- AC: select the cheapest scan without changing its symbols ---
        unsigned long long mask = cache->nonzero_masks[b] & ~1ull;
        if (cache->nonzero_counts &&
            cache->nonzero_counts[b] >= N148_DENSE_AC_THRESHOLD)
            write_ac_dense(writer, zz, mask, ac_table);
        else
            write_ac_sparse(writer, zz, mask, ac_table);
    }
    return cache->count;
}

// ============================================================
// ENCODE THE WHOLE IMAGE
// ============================================================

int encode_image(Plane *y, Plane *cb, Plane *cr, int quality, int optimize,
                 HuffSpec specs[HUFFMAN_TABLE_COUNT],
                 unsigned char **out_buffer, EncodeStats *stats) {
    init_dct_tables();

    int luma_quant[8][8], chroma_quant[8][8];
    scale_table(Q_LUMA_BASE,   quality, luma_quant);
    scale_table(Q_CHROMA_BASE, quality, chroma_quant);

    FastQuant fast_luma, fast_chroma;
    build_fast_quant(luma_quant,   &fast_luma);
    build_fast_quant(chroma_quant, &fast_chroma);

    CoeffCache cache_y = {0}, cache_cb = {0}, cache_cr = {0};

    long dc_luma_freq[256]   = {0}, ac_luma_freq[256]   = {0};
    long dc_chroma_freq[256] = {0}, ac_chroma_freq[256] = {0};

    // Per worker accumulators, merged once the bands are done.
    static long luma_bins[MAX_WORKERS][256];
    static long chroma_bins[MAX_WORKERS][256];
    int worker_count = n148_thread_count();
    if (worker_count > MAX_WORKERS) worker_count = MAX_WORKERS;
    if (optimize) {
        memset(luma_bins, 0, (size_t)worker_count * sizeof(luma_bins[0]));
        memset(chroma_bins, 0, (size_t)worker_count * sizeof(chroma_bins[0]));
    }

    if (!quantize_plane(y, &fast_luma, &cache_y, optimize, luma_bins)) return 0;
    if (!quantize_plane(cb, &fast_chroma, &cache_cb, optimize, chroma_bins)) {
        cache_release(&cache_y); return 0;
    }
    if (!quantize_plane(cr, &fast_chroma, &cache_cr, optimize, chroma_bins)) {
        cache_release(&cache_y); cache_release(&cache_cb); return 0;
    }

    if (optimize) {
        for (int w = 0; w < worker_count; w++)
            for (int s = 0; s < 256; s++) {
                ac_luma_freq[s]   += luma_bins[w][s];
                ac_chroma_freq[s] += chroma_bins[w][s];
            }

        count_dc(&cache_y,  dc_luma_freq);
        count_dc(&cache_cb, dc_chroma_freq);
        count_dc(&cache_cr, dc_chroma_freq);

        huffman_build_optimized(dc_luma_freq,   &specs[HUFFMAN_DC_LUMA]);
        huffman_build_optimized(ac_luma_freq,   &specs[HUFFMAN_AC_LUMA]);
        huffman_build_optimized(dc_chroma_freq, &specs[HUFFMAN_DC_CHROMA]);
        huffman_build_optimized(ac_chroma_freq, &specs[HUFFMAN_AC_CHROMA]);
    } else {
        huffman_default_specs(specs);
    }

    HuffTable dc_luma, ac_luma, dc_chroma, ac_chroma;
    build_huffman(&specs[HUFFMAN_DC_LUMA],   &dc_luma);
    build_huffman(&specs[HUFFMAN_AC_LUMA],   &ac_luma);
    build_huffman(&specs[HUFFMAN_DC_CHROMA], &dc_chroma);
    build_huffman(&specs[HUFFMAN_AC_CHROMA], &ac_chroma);

    BitWriter writer;
    long guess = (cache_y.count + cache_cb.count + cache_cr.count) * 24 + 4096;
    if (!bw_init(&writer, guess)) {
        cache_release(&cache_y); cache_release(&cache_cb); cache_release(&cache_cr);
        return 0;
    }

    stats->blocks_y  = write_cache(&writer, &cache_y,  &dc_luma,   &ac_luma);
    stats->blocks_cb = write_cache(&writer, &cache_cb, &dc_chroma, &ac_chroma);
    stats->blocks_cr = write_cache(&writer, &cache_cr, &dc_chroma, &ac_chroma);

    bw_flush(&writer);

    cache_release(&cache_y);
    cache_release(&cache_cb);
    cache_release(&cache_cr);

    stats->table_size = optimize ? huffman_tables_size(specs) : 0;
    stats->data_size  = writer.size;
    *out_buffer = writer.buffer;
    return 1;
}

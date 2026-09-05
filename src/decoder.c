#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "decoder.h"
#include "tables.h"
#include "dct.h"
#include "cpu.h"
#include "parallel.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

// ============================================================
// HUFFMAN DECODING TABLES
// ============================================================
//
// Two structures work together here.
//
// The slow path walks the canonical ranges one length at a time,
// exactly as the JPEG specification describes.
//
// The fast path is a flat lookup: we peek at the next LOOKUP_BITS
// bits and, for every code short enough to fit, read the symbol and
// its length straight out of an array. Most symbols in a real image
// are short, so this resolves the large majority of them without a
// single loop iteration.

#ifndef N148_LOOKUP_BITS
#define N148_LOOKUP_BITS 10
#endif
#define LOOKUP_BITS N148_LOOKUP_BITS
#define LOOKUP_SIZE (1 << LOOKUP_BITS)

typedef struct {
    int min_code[17];
    int max_code[17];        // -1 when no code has this length
    int value_index[17];
    const unsigned char *values;

    /* This table has two entry forms.  A resolved entry folds the Huffman
       code, amplitude and sign into one lookup.  A partial entry retains
       code length and symbol when the amplitude extends past the lookup
       window.  Zero alone selects the long-code path. */
    unsigned int action[LOOKUP_SIZE];
} HuffDecoder;

#define ACTION_BITS_SHIFT 16
#define ACTION_RUN_SHIFT  21
#define ACTION_EOB        (1u << 25)
#define ACTION_ZRL        (1u << 26)
#define ACTION_PARTIAL    (1u << 31)

static void build_decoder(const HuffSpec *spec, HuffDecoder *decoder,
                          int is_ac) {
    decoder->values = spec->values;

    int code = 0, k = 0;
    for (int length = 1; length <= 16; length++) {
        if (spec->bits[length] == 0) {
            decoder->max_code[length] = -1;
        } else {
            decoder->value_index[length] = k;
            decoder->min_code[length] = code;
            code += spec->bits[length];
            k    += spec->bits[length];
            decoder->max_code[length] = code - 1;
        }
        code <<= 1;
    }

    memset(decoder->action, 0, sizeof(decoder->action));

    // Fill every lookup slot whose prefix matches a short code.
    code = 0; k = 0;
    for (int length = 1; length <= 16; length++) {
        for (int i = 0; i < spec->bits[length]; i++, k++, code++) {
            if (length <= LOOKUP_BITS) {
                int shift = LOOKUP_BITS - length;
                int start = code << shift;
                int end   = start + (1 << shift);
                for (int slot = start; slot < end; slot++) {
                    decoder->action[slot] = ACTION_PARTIAL |
                        ((unsigned int) length << ACTION_BITS_SHIFT) |
                        spec->values[k];
                }
            }

            int symbol = spec->values[k];
            int amplitude_size = is_ac ? (symbol & 0x0F) : symbol;
            int special = is_ac && (symbol == 0x00 || symbol == 0xF0);
            int total = length + (special ? 0 : amplitude_size);
            if (total > LOOKUP_BITS || (!special && amplitude_size == 0))
                continue;

            int amplitudes = special ? 1 : (1 << amplitude_size);
            int suffix_bits = LOOKUP_BITS - total;
            for (int amplitude = 0; amplitude < amplitudes; amplitude++) {
                int value = amplitude;
                if (!special && amplitude < (1 << (amplitude_size - 1)))
                    value += 1 - (1 << amplitude_size);

                unsigned int action = (unsigned short) value;
                action |= (unsigned int) total << ACTION_BITS_SHIFT;
                if (is_ac) {
                    action |= (unsigned int)(symbol >> 4) << ACTION_RUN_SHIFT;
                    if (symbol == 0x00) action |= ACTION_EOB;
                    if (symbol == 0xF0) action |= ACTION_ZRL;
                }

                int prefix = ((code << (special ? 0 : amplitude_size)) |
                              amplitude) << suffix_bits;
                int repeat = 1 << suffix_bits;
                for (int slot = prefix; slot < prefix + repeat; slot++)
                    decoder->action[slot] = action;
            }
        }
        code <<= 1;
    }
}

// ============================================================
// BIT READER
// ============================================================
//
// The accumulator is kept left aligned: the next bit to be consumed
// always sits at the top. Peeking is then a single shift, and a
// refill can pull eight bytes in one read instead of looping a byte
// at a time.

typedef struct {
    const unsigned char *buffer;
    long size;
    long position;
    unsigned long long accumulator;
    int available;                    // valid bits, counted from the top
    int padding;                      // synthetic zero bits after the payload
} BitReader;

static void br_init(BitReader *reader, const unsigned char *buffer, long size) {
    reader->buffer = buffer;
    reader->size = size;
    reader->position = 0;
    reader->accumulator = 0;
    reader->available = 0;
    reader->padding = 0;
}

static inline unsigned int load_be32(const unsigned char *p) {
    unsigned int v;
    memcpy(&v, p, sizeof(v));
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
#endif
}

static inline void br_refill(BitReader *reader) {
    /* A Huffman code plus its amplitude uses at most 31 bits.  Keeping
       32 ready avoids the old overlapping 64-bit load before every symbol. */
    if (reader->available >= 32) return;

    if (reader->position + 4 <= reader->size) {
        unsigned int next = load_be32(reader->buffer + reader->position);
        reader->accumulator |=
            (unsigned long long) next << (32 - reader->available);
        reader->position += 4;
        reader->available += 32;
        return;
    }

    while (reader->available < 32) {
        unsigned long long byte = 0;
        if (reader->position < reader->size) {
            byte = reader->buffer[reader->position++];
        } else {
            reader->padding += 8;
        }
        reader->accumulator |= byte << (56 - reader->available);
        reader->available += 8;
    }
}

static inline unsigned int br_peek(BitReader *reader, int count) {
    return (unsigned int)(reader->accumulator >> (64 - count));
}

static inline void br_skip(BitReader *reader, int count) {
    reader->accumulator <<= count;
    reader->available -= count;
}

static inline unsigned int br_read(BitReader *reader, int count) {
    if (count == 0) return 0;
    unsigned int value = br_peek(reader, count);
    br_skip(reader, count);
    return value;
}

/* Returns length in the high byte and symbol in the low byte, without
   consuming either the code or its amplitude. */
static int br_read_code_slow(BitReader *reader, const HuffDecoder *decoder) {
    unsigned int bits = br_peek(reader, 16);
    for (int length = LOOKUP_BITS + 1; length <= 16; length++) {
        int code = (int)(bits >> (16 - length));
        if (decoder->max_code[length] >= 0 &&
            code >= decoder->min_code[length] &&
            code <= decoder->max_code[length]) {
            int offset = decoder->value_index[length]
                       + (code - decoder->min_code[length]);
            return (length << 8) | decoder->values[offset];
        }
    }
    br_skip(reader, 16);             // preserve progress on malformed input
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline int br_read_code_fallback(BitReader *reader,
                                        const HuffDecoder *decoder,
                                        unsigned int entry) {
    if (entry & ACTION_PARTIAL)
        return (int)(((entry >> ACTION_BITS_SHIFT) & 31) << 8) |
                     (int)(entry & 0xFF);
    return br_read_code_slow(reader, decoder);
}

/* Consumes a Huffman code and its amplitude together, then restores sign. */
static inline int br_read_amplitude(BitReader *reader, int code_length,
                                    int size) {
    if (size == 0) {
        br_skip(reader, code_length);
        return 0;
    }
    int total = code_length + size;
    unsigned int mask = (1u << size) - 1u;
    int value = (int)(br_peek(reader, total) & mask);
    br_skip(reader, total);
    int threshold = 1 << (size - 1);
    unsigned int negative = (unsigned int)-(value < threshold);
    return value + (int)(negative & (1u - (1u << size)));
}

// ============================================================
// DEQUANTIZATION TABLE FOR THE FAST INVERSE TRANSFORM
// ============================================================

typedef struct { float multiplier[64]; } FastDequant;

static void build_fast_dequant(int quantization[8][8], FastDequant *out) {
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++)
            out->multiplier[u*8+v] =
                (float)(quantization[u][v] * aan_scale_factor(u, v) / 64.0);
}

// ============================================================
// BLOCK RECONSTRUCTION
// ============================================================

#define SPARSE_TWO_BASE   64
#define SPARSE_THREE_BASE (64 + 4096)

/* Returns the flat byte value plus one for a DC-only block, zero for the full
   transform, or a negative sparse descriptor.  Sparse descriptors pack one,
   two or three six-bit natural-order indices into disjoint integer ranges. */
static int decode_block(BitReader *reader, float block[64],
                        const FastDequant *dequant,
                        const HuffDecoder *dc_decoder,
                        const HuffDecoder *ac_decoder,
                        int *dc_previous) {
    // Coefficients are dequantized as they are read. Writing straight
    // into the natural order array removes the separate 64 step
    // reordering pass: only the handful that survived are touched.
    float *coefficients = block;

    br_refill(reader);
    unsigned int action = dc_decoder->action[br_peek(reader, LOOKUP_BITS)];
    int code;
    int difference;
    if (action && !(action & ACTION_PARTIAL)) {
        br_skip(reader, (action >> ACTION_BITS_SHIFT) & 31);
        difference = (short) action;
    } else {
        code = br_read_code_fallback(reader, dc_decoder, action);
        int size = code & 0xFF;
        difference = br_read_amplitude(reader, code >> 8, size);
    }
    int dc = *dc_previous + difference;
    *dc_previous = dc;
    float dc_coefficient = dc * dequant->multiplier[0];

    int nonzero_ac = 0;
    int single_ac_index = 0;
    int second_ac_index = 0;
    int third_ac_index = 0;
    int position = 1;
    while (position < 64) {
        br_refill(reader);
        action = ac_decoder->action[br_peek(reader, LOOKUP_BITS)];
        int value;
        if (action && !(action & ACTION_PARTIAL)) {
            br_skip(reader, (action >> ACTION_BITS_SHIFT) & 31);
            if (action & ACTION_EOB) break;
            if (action & ACTION_ZRL) {
                position += 16;
                continue;
            }
            position += (action >> ACTION_RUN_SHIFT) & 15;
            value = (short) action;
        } else {
            code = br_read_code_fallback(reader, ac_decoder, action);
            if (code == 0) break;
            int code_length = code >> 8;
            int symbol = code & 0xFF;
            if (symbol == 0x00) {                          // EOB
                br_skip(reader, code_length);
                break;
            }
            if (symbol == 0xF0) {                          // ZRL
                br_skip(reader, code_length);
                position += 16;
                continue;
            }

            position += symbol >> 4;
            int coefficient_size = symbol & 0x0F;
            value = br_read_amplitude(reader, code_length, coefficient_size);
        }
        if (position >= 64) {
            break;
        }

        int index = ZIGZAG[position];
        if (nonzero_ac == 0) {
            /* Most low-frequency blocks end at EOB.  Delay clearing the
               transform workspace until an AC coefficient proves it is
               actually needed. */
            memset(coefficients, 0, 64 * sizeof(*coefficients));
            coefficients[0] = dc_coefficient;
            single_ac_index = index;
        } else if (nonzero_ac == 1) {
            second_ac_index = index;
        } else if (nonzero_ac == 2) {
            third_ac_index = index;
        }
        coefficients[index] = value * dequant->multiplier[index];
        nonzero_ac++;
        position++;
    }

    // A block whose AC coefficients all vanished reconstructs to one
    // flat value, so the transform can be skipped entirely. At lower
    // quality settings this covers a large share of the image.
    if (nonzero_ac == 0) {
        float flat = dc_coefficient + 128.0f;   // the 1/8 lives in the table
        if (flat < 0.0f)   flat = 0.0f;
        if (flat > 255.0f) flat = 255.0f;
        return (int)(flat + 0.5f) + 1;
    }

    if (nonzero_ac == 1) return -single_ac_index;
    if (nonzero_ac == 2)
        return -(SPARSE_TWO_BASE +
                 (single_ac_index << 6) + second_ac_index);
    if (nonzero_ac == 3)
        return -(SPARSE_THREE_BASE + (single_ac_index << 12) +
                 (second_ac_index << 6) + third_ac_index);

    return 0;
}

static long decode_plane(BitReader *reader, Plane *plane,
                         const FastDequant *dequant,
                         const HuffDecoder *dc_decoder,
                         const HuffDecoder *ac_decoder) {
    int blocks_x = (plane->width  + 7) / 8;
    int blocks_y = (plane->height + 7) / 8;
    int dc_previous = 0;
    int use_avx2 = 0;
#if defined(__x86_64__) || defined(__i386__)
    use_avx2 = (n148_cpu_level() >= N148_CPU_AVX2);
#endif

    for (int by = 0; by < blocks_y; by++) {
        for (int bx = 0; bx < blocks_x; bx++) {
            float block[64];
            int max_x = bx*8 + 8 <= plane->width  ? 8 : plane->width  - bx*8;
            int max_y = by*8 + 8 <= plane->height ? 8 : plane->height - by*8;
            unsigned char *block_dst =
                plane->data + (long)(by * 8) * plane->width + bx * 8;
            int flat = decode_block(reader, block, dequant, dc_decoder,
                                    ac_decoder, &dc_previous);

            if (flat > 0) {
                unsigned char *dst = block_dst;
                if (max_x == 8) {
                    unsigned long long packed = 0x0101010101010101ull *
                                                (unsigned int)(flat - 1);
                    for (int r = 0; r < max_y; r++, dst += plane->width)
                        memcpy(dst, &packed, sizeof(packed));
                } else {
                    for (int r = 0; r < max_y; r++, dst += plane->width)
                        memset(dst, flat - 1, (size_t) max_x);
                }
                continue;
            }

#if defined(__x86_64__) || defined(__i386__)
            if (flat < 0 && max_x == 8 && max_y == 8 && use_avx2) {
                int sparse = -flat;
                if (sparse >= SPARSE_THREE_BASE) {
                    sparse -= SPARSE_THREE_BASE;
                    int first = sparse >> 12;
                    int second = (sparse >> 6) & 63;
                    int third = sparse & 63;
                    idct_block_store_three_avx2(
                        block[0], block[first], first,
                        block[second], second, block[third], third,
                        block_dst, plane->width);
                } else if (sparse >= SPARSE_TWO_BASE) {
                    sparse -= SPARSE_TWO_BASE;
                    int first = sparse >> 6;
                    int second = sparse & 63;
                    idct_block_store_two_avx2(
                        block[0], block[first], first,
                        block[second], second, block_dst, plane->width);
                } else {
                    idct_block_store_single_avx2(
                        block[0], block[sparse], sparse,
                        block_dst, plane->width);
                }
                continue;
            }
#endif

#if defined(__x86_64__) || defined(__i386__)
            if (max_x == 8 && max_y == 8 && use_avx2) {
                idct_block_store_avx2(block, block_dst, plane->width);
                continue;
            }
            if (use_avx2) idct_block_avx2(block, block);
            else
#endif
            idct_block_fast(block, block);

            for (int r = 0; r < max_y; r++) {
                unsigned char *dst =
                    plane->data + (long)(by*8 + r) * plane->width + bx*8;
                const float *src = &block[r*8];
                for (int c = 0; c < max_x; c++) {
                    float value = src[c];
                    if (value < 0.0f)   value = 0.0f;
                    if (value > 255.0f) value = 255.0f;
                    dst[c] = (unsigned char)(value + 0.5f);
                }
            }
        }
    }
    return (long) blocks_x * blocks_y;
}

int decode_image(unsigned char *buffer, long buffer_size,
                 int width, int height, int quality, int chroma,
                 const HuffSpec specs[HUFFMAN_TABLE_COUNT],
                 Plane *y, Plane *cb, Plane *cr, DecodeStats *stats) {
    init_dct_tables();

    HuffDecoder dc_luma, ac_luma, dc_chroma, ac_chroma;
    build_decoder(&specs[HUFFMAN_DC_LUMA],   &dc_luma,   0);
    build_decoder(&specs[HUFFMAN_AC_LUMA],   &ac_luma,   1);
    build_decoder(&specs[HUFFMAN_DC_CHROMA], &dc_chroma, 0);
    build_decoder(&specs[HUFFMAN_AC_CHROMA], &ac_chroma, 1);

    int luma_quant[8][8], chroma_quant[8][8];
    scale_table(Q_LUMA_BASE,   quality, luma_quant);
    scale_table(Q_CHROMA_BASE, quality, chroma_quant);

    FastDequant dequant_luma, dequant_chroma;
    build_fast_dequant(luma_quant,   &dequant_luma);
    build_fast_dequant(chroma_quant, &dequant_chroma);

    int chroma_width, chroma_height;
    chroma_dimensions(chroma, width, height, &chroma_width, &chroma_height);

    *y  = create_plane(width, height);
    *cb = create_plane(chroma_width, chroma_height);
    *cr = create_plane(chroma_width, chroma_height);
    if (!y->data || !cb->data || !cr->data) return 0;

    BitReader reader;
    br_init(&reader, buffer, buffer_size);

    stats->blocks_y  = decode_plane(&reader, y,  &dequant_luma,   &dc_luma,   &ac_luma);
    stats->blocks_cb = decode_plane(&reader, cb, &dequant_chroma, &dc_chroma, &ac_chroma);
    stats->blocks_cr = decode_plane(&reader, cr, &dequant_chroma, &dc_chroma, &ac_chroma);

    long consumed_bits = reader.position * 8L -
                         (reader.available - reader.padding);
    stats->bytes_consumed = (consumed_bits + 7) >> 3;
    return 1;
}

// ============================================================
// YCbCr -> RGB
// ============================================================

static unsigned char clamp_byte(float value) {
    if (value < 0.0f)   return 0;
    if (value > 255.0f) return 255;
    return (unsigned char)(value + 0.5f);
}

// ============================================================
// CHROMA UPSAMPLING
// ============================================================
//
// For the subsampled modes the bilinear weights are not arbitrary:
// mapping pixel centres between a full resolution grid and a half
// resolution one always lands on the same 3:1 split. Knowing that in
// advance turns the interpolation into integer arithmetic with fixed
// weights, far cheaper than the general bilinear formula.
//
//   horizontal only:  (3*near + far) * 4        -> scaled by 16
//   vertical only:    (3*near + far) * 4        -> scaled by 16
//   both:             9*a + 3*b + 3*c + d       -> scaled by 16
//
// The rows stay at that 16x scale and reach the colour stage
// unrounded: collapsing them to a byte first would discard the
// fractional part the conversion still needs.

#define CHROMA_SHIFT 4                 /* rows carry value * 16 */

#if defined(__x86_64__) || defined(__i386__)
// Vertical blend of two chroma rows, sixteen samples per pass.
__attribute__((target("avx2")))
static void blend_rows_avx2(const unsigned char *near_row,
                            const unsigned char *far_row,
                            int count, unsigned short *out) {
    const __m256i three = _mm256_set1_epi16(3);
    int x = 0;
    for (; x + 16 <= count; x += 16) {
        __m256i n = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(near_row + x)));
        __m256i f = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(far_row  + x)));
        _mm256_storeu_si256((__m256i *)(out + x),
                            _mm256_add_epi16(_mm256_mullo_epi16(n, three), f));
    }
    for (; x < count; x++)
        out[x] = (unsigned short)(3 * near_row[x] + far_row[x]);
}

// Horizontal stretch: every source sample produces the two outputs it
// dominates, so both are computed as vectors and interleaved back.
__attribute__((target("avx2")))
static void stretch_avx2(const unsigned short *src, int first, int last,
                         unsigned short *dst) {
    const __m256i three = _mm256_set1_epi16(3);
    int k = first;
    for (; k + 16 <= last; k += 16) {
        __m256i cur  = _mm256_loadu_si256((const __m256i *)(src + k));
        __m256i prev = _mm256_loadu_si256((const __m256i *)(src + k - 1));
        __m256i next = _mm256_loadu_si256((const __m256i *)(src + k + 1));

        __m256i scaled = _mm256_mullo_epi16(cur, three);
        __m256i even = _mm256_add_epi16(scaled, prev);
        __m256i odd  = _mm256_add_epi16(scaled, next);

        __m256i lo = _mm256_unpacklo_epi16(even, odd);
        __m256i hi = _mm256_unpackhi_epi16(even, odd);
        _mm256_storeu_si256((__m256i *)(dst + 2*k),
                            _mm256_permute2x128_si256(lo, hi, 0x20));
        _mm256_storeu_si256((__m256i *)(dst + 2*k + 16),
                            _mm256_permute2x128_si256(lo, hi, 0x31));
    }
    for (; k < last; k++) {
        int centre = src[k] * 3;
        dst[2*k]     = (unsigned short)(centre + src[k-1]);
        dst[2*k + 1] = (unsigned short)(centre + src[k+1]);
    }
}
#endif

static void expand_h(const unsigned char *src, int src_width,
                     unsigned short *dst, int dst_width) {
    if (src_width == 1) {
        unsigned short v = (unsigned short)(src[0] << CHROMA_SHIFT);
        for (int x = 0; x < dst_width; x++) dst[x] = v;
        return;
    }

    dst[0] = (unsigned short)(src[0] * 16);
    if (dst_width > 1) dst[1] = (unsigned short)(src[0] * 12 + src[1] * 4);

    for (int k = 1; k < src_width - 1; k++) {
        int centre = src[k] * 12;
        dst[2*k]     = (unsigned short)(centre + src[k-1] * 4);
        dst[2*k + 1] = (unsigned short)(centre + src[k+1] * 4);
    }

    int last = src_width - 1;
    int centre = src[last] * 12;
    if (2*last     < dst_width) dst[2*last]     = (unsigned short)(centre + src[last-1] * 4);
    if (2*last + 1 < dst_width) dst[2*last + 1] = (unsigned short)(centre + src[last] * 4);
    for (int x = 2*src_width; x < dst_width; x++)
        dst[x] = (unsigned short)(src[src_width-1] << CHROMA_SHIFT);
}

static void expand_hv(const unsigned char *near_row, const unsigned char *far_row,
                      int src_width, unsigned short *scratch,
                      unsigned short *dst, int dst_width, int use_avx2) {
    int blended = 0;
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2) {
        blend_rows_avx2(near_row, far_row, src_width, scratch);
        blended = 1;
    }
#else
    (void) use_avx2;
#endif
    if (!blended) {
        for (int x = 0; x < src_width; x++)
            scratch[x] = (unsigned short)(3 * near_row[x] + far_row[x]);
    }

    if (src_width == 1) {
        unsigned short v = (unsigned short)(scratch[0] * 4);
        for (int x = 0; x < dst_width; x++) dst[x] = v;
        return;
    }

    dst[0] = (unsigned short)(scratch[0] * 4);
    if (dst_width > 1) dst[1] = (unsigned short)(scratch[0] * 3 + scratch[1]);

    int stretched = 0;
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2) {
        stretch_avx2(scratch, 1, src_width - 1, dst);
        stretched = 1;
    }
#endif
    if (!stretched) {
        for (int k = 1; k < src_width - 1; k++) {
            int centre = scratch[k] * 3;
            dst[2*k]     = (unsigned short)(centre + scratch[k-1]);
            dst[2*k + 1] = (unsigned short)(centre + scratch[k+1]);
        }
    }

    int last = src_width - 1;
    int centre = scratch[last] * 3;
    if (2*last     < dst_width) dst[2*last]     = (unsigned short)(centre + scratch[last-1]);
    if (2*last + 1 < dst_width) dst[2*last + 1] = (unsigned short)(centre + scratch[last]);
    for (int x = 2*src_width; x < dst_width; x++)
        dst[x] = (unsigned short)(scratch[src_width-1] * 4);
}

// Produces one chroma row for output line py, scaled by 16.
static void expand_chroma_row(Plane *plane, int py, int dst_width,
                              int stretch_x, int stretch_y,
                              unsigned short *scratch, unsigned short *dst,
                              int use_avx2) {
    if (stretch_y) {
        int near = py >> 1;
        int far  = (py & 1) ? near + 1 : near - 1;
        if (far < 0) far = 0;
        if (far >= plane->height) far = plane->height - 1;
        if (near >= plane->height) near = plane->height - 1;

        const unsigned char *near_row = plane->data + (long) near * plane->width;
        const unsigned char *far_row  = plane->data + (long) far  * plane->width;

        if (stretch_x) {
            expand_hv(near_row, far_row, plane->width, scratch, dst, dst_width, use_avx2);
        } else {
            for (int x = 0; x < dst_width; x++) {
                int sx = x < plane->width ? x : plane->width - 1;
                dst[x] = (unsigned short)((3 * near_row[sx] + far_row[sx]) * 4);
            }
        }
        return;
    }

    int sy = py < plane->height ? py : plane->height - 1;
    const unsigned char *row = plane->data + (long) sy * plane->width;

    if (stretch_x) {
        expand_h(row, plane->width, dst, dst_width);
    } else {
        for (int x = 0; x < dst_width; x++) {
            int sx = x < plane->width ? x : plane->width - 1;
            dst[x] = (unsigned short)(row[sx] << CHROMA_SHIFT);
        }
    }
}

// ============================================================
// COLOUR CONVERSION
// ============================================================

#if defined(__x86_64__) || defined(__i386__)
// Interleaves eight pixels worth of separate R, G and B bytes into
// the 24 packed bytes the output image expects.
__attribute__((target("avx2")))
static inline void store_rgb8(unsigned char *out, __m128i r, __m128i g, __m128i b) {
    __m128i rg = _mm_unpacklo_epi8(r, g);
    const __m128i mrg0 = _mm_setr_epi8(
         0, 1,-1, 2, 3,-1, 4, 5,-1, 6, 7,-1, 8, 9,-1,10);
    const __m128i mb0 = _mm_setr_epi8(
        -1,-1, 0,-1,-1, 1,-1,-1, 2,-1,-1, 3,-1,-1, 4,-1);
    _mm_storeu_si128((__m128i *) out,
        _mm_or_si128(_mm_shuffle_epi8(rg, mrg0), _mm_shuffle_epi8(b, mb0)));

    const __m128i mrg1 = _mm_setr_epi8(
        11,-1,12,13,-1,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1);
    const __m128i mb1 = _mm_setr_epi8(
        -1, 5,-1,-1, 6,-1,-1, 7,-1,-1,-1,-1,-1,-1,-1,-1);
    _mm_storel_epi64((__m128i *)(out + 16),
        _mm_or_si128(_mm_shuffle_epi8(rg, mrg1), _mm_shuffle_epi8(b, mb1)));
}

__attribute__((target("avx2"), always_inline))
static inline void finish_ycbcr8_avx2(__m256 yf, __m256 cbf, __m256 crf,
                                      __m256 rf, __m256 bf,
                                      unsigned char *out) {
    const __m256 k_g_cb = _mm256_set1_ps(-0.344136f);
    const __m256 k_g_cr = _mm256_set1_ps(-0.714136f);
    const __m256 round  = _mm256_set1_ps(0.5f);
    rf = _mm256_add_ps(rf, round);
    bf = _mm256_add_ps(bf, round);
    __m256 gf = _mm256_add_ps(_mm256_add_ps(yf,
                   _mm256_add_ps(_mm256_mul_ps(cbf, k_g_cb),
                                 _mm256_mul_ps(crf, k_g_cr))), round);

    __m256i r = _mm256_cvttps_epi32(rf);
    __m256i g = _mm256_cvttps_epi32(gf);
    __m256i b = _mm256_cvttps_epi32(bf);
    __m128i r16 = _mm_packs_epi32(_mm256_castsi256_si128(r),
                                  _mm256_extracti128_si256(r, 1));
    __m128i g16 = _mm_packs_epi32(_mm256_castsi256_si128(g),
                                  _mm256_extracti128_si256(g, 1));
    __m128i b16 = _mm_packs_epi32(_mm256_castsi256_si128(b),
                                  _mm256_extracti128_si256(b, 1));
    store_rgb8(out, _mm_packus_epi16(r16, r16),
                    _mm_packus_epi16(g16, g16),
                    _mm_packus_epi16(b16, b16));
}

__attribute__((target("avx2"), always_inline))
static inline void convert_ycbcr8_avx2(const unsigned char *luma,
                                        __m128i blue, __m128i red,
                                        unsigned char *out) {
    const __m256 inv    = _mm256_set1_ps(1.0f / (1 << CHROMA_SHIFT));
    const __m256 half   = _mm256_set1_ps(128.0f);
    const __m256 k_r_cr = _mm256_set1_ps(1.402f);
    const __m256 k_b_cb = _mm256_set1_ps(1.772f);

    __m128i y8 = _mm_loadl_epi64((const __m128i *) luma);
    __m256 yf  = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(y8));
    __m256 cbf = _mm256_sub_ps(
        _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(blue)), inv), half);
    __m256 crf = _mm256_sub_ps(
        _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(red)), inv), half);

    __m256 rf = _mm256_add_ps(yf, _mm256_mul_ps(crf, k_r_cr));
    __m256 bf = _mm256_add_ps(yf, _mm256_mul_ps(cbf, k_b_cb));
    finish_ycbcr8_avx2(yf, cbf, crf, rf, bf, out);
}

__attribute__((target("avx2,fma"), always_inline))
static inline void convert_ycbcr8_fma(const unsigned char *luma,
                                      __m128i blue, __m128i red,
                                      unsigned char *out) {
    const __m256 inv    = _mm256_set1_ps(1.0f / (1 << CHROMA_SHIFT));
    const __m256 half   = _mm256_set1_ps(128.0f);
    const __m256 k_r_cr = _mm256_set1_ps(1.402f);
    const __m256 k_b_cb = _mm256_set1_ps(1.772f);

    __m128i y8 = _mm_loadl_epi64((const __m128i *) luma);
    __m256 yf  = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(y8));
    __m256 cbf = _mm256_sub_ps(
        _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(blue)), inv), half);
    __m256 crf = _mm256_sub_ps(
        _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(red)), inv), half);

    __m256 rf = _mm256_fmadd_ps(crf, k_r_cr, yf);
    __m256 bf = _mm256_fmadd_ps(cbf, k_b_cb, yf);
    finish_ycbcr8_avx2(yf, cbf, crf, rf, bf, out);
}

// Eight pixels per iteration. The chroma rows arrive scaled by 16 so
// no precision is lost on the way in.
__attribute__((target("avx2")))
static void ycbcr_row_avx2(const unsigned char *luma,
                           const unsigned short *blue, const unsigned short *red,
                           unsigned char *out, int count) {
    int i = 0;
    // Each interleaved store writes exactly the 24 bytes for this group.
    for (; i + 8 <= count; i += 8) {
        __m128i cb16 = _mm_loadu_si128((const __m128i *)(blue + i));
        __m128i cr16 = _mm_loadu_si128((const __m128i *)(red  + i));
        convert_ycbcr8_avx2(luma + i, cb16, cr16, out + i * 3);
    }

    const float inv_s = 1.0f / (1 << CHROMA_SHIFT);
    for (; i < count; i++) {
        float yf  = luma[i];
        float cbf = blue[i] * inv_s - 128.0f;
        float crf = red[i]  * inv_s - 128.0f;
        out[i*3+0] = clamp_byte(yf + 1.402f * crf);
        float green = (-0.344136f * cbf) + (-0.714136f * crf);
        out[i*3+1] = clamp_byte(yf + green);
        out[i*3+2] = clamp_byte(yf + 1.772f * cbf);
    }
}

__attribute__((target("avx2,fma")))
static void ycbcr_row_fma(const unsigned char *luma,
                          const unsigned short *blue,
                          const unsigned short *red,
                          unsigned char *out, int count) {
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        __m128i cb16 = _mm_loadu_si128((const __m128i *)(blue + i));
        __m128i cr16 = _mm_loadu_si128((const __m128i *)(red  + i));
        convert_ycbcr8_fma(luma + i, cb16, cr16, out + i * 3);
    }

    /* Keep the rare tail on the non-FMA implementation: fusing the green
       channel changes a handful of exact half-way rounding cases. */
    if (i < count)
        ycbcr_row_avx2(luma + i, blue + i, red + i,
                       out + i * 3, count - i);
}

#endif

// ============================================================
// RECONSTRUCTION
// ============================================================

typedef struct {
    Plane *y, *cb, *cr;
    Image *output;
    int smooth, stretch_x, stretch_y, use_avx2, use_fma;
} MergeJob;

// Reconstructs a band of output rows. Every worker keeps its own
// scratch rows so nothing is shared between them.
static void merge_rows(long start, long end, int worker, void *context) {
    (void) worker;
    MergeJob *job = (MergeJob *) context;
    Plane *y = job->y, *cb = job->cb, *cr = job->cr;
    int width = y->width;

#define MERGE_STACK_WIDTH 2048
    _Alignas(32) unsigned short stack_rows[(MERGE_STACK_WIDTH + 32) * 3];
    unsigned short *allocated = NULL;
    unsigned short *row_cb;
    if (width <= MERGE_STACK_WIDTH && cb->width <= MERGE_STACK_WIDTH) {
        row_cb = stack_rows;
    } else {
        size_t samples = (size_t)(width + 32) * 2 + (size_t)cb->width + 32;
        allocated = (unsigned short *) malloc(samples * sizeof(unsigned short));
        if (!allocated) return;
        row_cb = allocated;
    }
    unsigned short *row_cr = row_cb + width + 32;
    unsigned short *scratch = row_cr + width + 32;

    const float inv_scale = 1.0f / (1 << CHROMA_SHIFT);

    for (long py = start; py < end; py++) {
        unsigned char *out = job->output->pixels + py * width * 3;
        const unsigned char *luma = y->data + py * width;

        if (job->smooth) {
            expand_chroma_row(cb, (int) py, width, job->stretch_x, job->stretch_y,
                              scratch, row_cb, job->use_avx2);
            expand_chroma_row(cr, (int) py, width, job->stretch_x, job->stretch_y,
                              scratch, row_cr, job->use_avx2);
        } else {
            int cy = job->stretch_y ? (int)(py >> 1) : (int) py;
            if (cy >= cb->height) cy = cb->height - 1;
            const unsigned char *src_cb = cb->data + (long) cy * cb->width;
            const unsigned char *src_cr = cr->data + (long) cy * cr->width;
            for (int px = 0; px < width; px++) {
                int cx = job->stretch_x ? (px >> 1) : px;
                if (cx >= cb->width) cx = cb->width - 1;
                row_cb[px] = (unsigned short)(src_cb[cx] << CHROMA_SHIFT);
                row_cr[px] = (unsigned short)(src_cr[cx] << CHROMA_SHIFT);
            }
        }

#if defined(__x86_64__) || defined(__i386__)
        if (job->use_fma) { ycbcr_row_fma(luma, row_cb, row_cr, out, width); continue; }
        if (job->use_avx2) { ycbcr_row_avx2(luma, row_cb, row_cr, out, width); continue; }
#endif
        for (int px = 0; px < width; px++) {
            float value = luma[px];
            float blue_difference = row_cb[px] * inv_scale - 128.0f;
            float red_difference  = row_cr[px] * inv_scale - 128.0f;
            out[px*3+0] = clamp_byte(value + 1.402f * red_difference);
            float green = (-0.344136f * blue_difference) +
                          (-0.714136f * red_difference);
            out[px*3+1] = clamp_byte(value + green);
            out[px*3+2] = clamp_byte(value + 1.772f * blue_difference);
        }
    }

    free(allocated);
#undef MERGE_STACK_WIDTH
}

int merge_channels(Plane *y, Plane *cb, Plane *cr, int smooth, Image *output) {
    int width = y->width, height = y->height;

    if (width <= 0 || height <= 0 || !y->data || !cb->data || !cr->data ||
        cb->width != cr->width || cb->height != cr->height) {
        return 0;
    }

    output->width  = width;
    output->height = height;
    output->pixels = (unsigned char *) malloc((size_t)((long) width * height * 3));
    if (!output->pixels) return 0;

    MergeJob job;
    job.y = y; job.cb = cb; job.cr = cr; job.output = output;
    job.smooth = smooth;
    job.stretch_x = (cb->width  < width);
    job.stretch_y = (cb->height < height);
#if defined(__x86_64__) || defined(__i386__)
    int cpu_level = n148_cpu_level();
    job.use_avx2 = (cpu_level >= N148_CPU_AVX2);
    job.use_fma = (cpu_level >= N148_CPU_AVX2_FMA);
#else
    job.use_avx2 = 0;
    job.use_fma = 0;
#endif

    /* On small images the colour merge is only a few tenths of a
       millisecond; dispatching it to a pool costs more than the work saved,
       especially on hybrid CPUs.  Larger frames still scale across cores. */
    if ((long) width * height < 512L * 1024L)
        merge_rows(0, height, 0, &job);
    else
        n148_parallel_for(height, merge_rows, &job);
    return 1;
}

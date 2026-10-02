/*
 * N.148i range Asymmetric Numeral System coder
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_RANS_H
#define N148_RANS_H

#include <stddef.h>
#include <stdint.h>

#define N148_RANS_SCALE_BITS 12
#define N148_RANS_TOTAL (1u << N148_RANS_SCALE_BITS)
#define N148_RANS_LOWER_BOUND (1u << 23)
#define N148_RANS_ALPHABET_SIZE 256

typedef struct {
    uint16_t frequency[N148_RANS_ALPHABET_SIZE];
    uint16_t cumulative[N148_RANS_ALPHABET_SIZE + 1];
    uint8_t symbol_for_slot[N148_RANS_TOTAL];
    /* Fixed-model decode reads both values after resolving a slot. */
    uint32_t decode_pair[N148_RANS_ALPHABET_SIZE];
    uint16_t used_symbols;
} N148RansModel;

/* Mutable fixed-total model used by format 7. A Fenwick tree keeps prefix
   sums and inverse lookup deterministic without rebuilding the 4096-entry
   table after every symbol. */
typedef struct {
    uint16_t frequency[N148_RANS_ALPHABET_SIZE];
    uint16_t fenwick[N148_RANS_ALPHABET_SIZE + 1];
    uint8_t active_symbols[N148_RANS_ALPHABET_SIZE];
    uint16_t used_symbols;
} N148RansAdaptiveModel;

typedef struct {
    const N148RansModel *model;
    const N148RansModel *model_base;
    N148RansAdaptiveModel *adaptive_models;
    size_t adaptive_model_count;
    size_t adaptive_model_limit;
    const uint8_t *adaptive_model_flags;
    size_t adaptive_model_flag_count;
    uint8_t adaptation_rate_shift;
    const uint8_t *next;
    const uint8_t *end;
    uint32_t state;
    int valid;
} N148RansDecoder;

/* Normalize nonzero counts to exactly 2^12 without dropping used symbols. */
int n148_rans_build_model(const uint32_t counts[N148_RANS_ALPHABET_SIZE],
                          N148RansModel *model);

/* Rebuild cumulative and decoding tables from serialized frequencies. */
int n148_rans_prepare_model(N148RansModel *model);

/* The caller owns the returned stream and releases it with free(). */
int n148_rans_encode(const uint8_t *symbols, size_t symbol_count,
                     const N148RansModel *model,
                     uint8_t **stream, size_t *stream_size);

/* Each symbol may select a model known independently by the decoder. */
int n148_rans_encode_mixed(const uint8_t *symbols,
                           const uint8_t *model_indexes,
                           size_t symbol_count,
                           const N148RansModel *models, size_t model_count,
                           uint8_t **stream, size_t *stream_size);

/* Format 7 adaptive coding. Models start from the serialized static image
   histograms and move toward each observed symbol by 1 / 2^rate_shift while
   retaining a frequency of at least one for every initially used symbol. */
int n148_rans_encode_mixed_adaptive(
    const uint8_t *symbols, const uint8_t *model_indexes,
    size_t symbol_count, const N148RansModel *models, size_t model_count,
    size_t adaptive_model_limit, const uint8_t *adaptive_model_flags,
    size_t adaptive_model_flag_count, uint8_t adaptation_rate_shift,
    uint8_t **stream, size_t *stream_size, uint64_t *final_model_hash);

int n148_rans_adaptive_model_init(N148RansAdaptiveModel *adaptive,
                                  const N148RansModel *initial);
int n148_rans_adaptive_model_update(N148RansAdaptiveModel *adaptive,
                                    uint8_t symbol,
                                    uint8_t adaptation_rate_shift);
uint64_t n148_rans_adaptive_models_hash(
    const N148RansAdaptiveModel *models, size_t model_count);

int n148_rans_decoder_init(N148RansDecoder *decoder,
                           const N148RansModel *model,
                           const uint8_t *stream, size_t stream_size);
int n148_rans_decoder_init_mixed(N148RansDecoder *decoder,
                                 const uint8_t *stream, size_t stream_size);
int n148_rans_decoder_init_mixed_adaptive(
    N148RansDecoder *decoder, const uint8_t *stream, size_t stream_size,
    const N148RansModel *initial_models,
    N148RansAdaptiveModel *adaptive_models, size_t model_count,
    size_t adaptive_model_limit, const uint8_t *adaptive_model_flags,
    size_t adaptive_model_flag_count, uint8_t adaptation_rate_shift);
int n148_rans_decode_symbol(N148RansDecoder *decoder, uint8_t *symbol);
int n148_rans_decode_symbol_with_model(N148RansDecoder *decoder,
                                       const N148RansModel *model,
                                       uint8_t *symbol);
int n148_rans_decoder_finished(const N148RansDecoder *decoder);

#endif

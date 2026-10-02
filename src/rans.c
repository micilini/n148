/*
 * N.148i range Asymmetric Numeral System coder
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * This is a deliberately compact educational byte-rANS implementation. The
 * encoder visits symbols in reverse and grows its output backwards; decoding
 * then consumes both symbols and renormalization bytes in forward order.
 */

#include "rans.h"

#include <limits.h>
#ifdef N148_RANS_DIAGNOSTICS
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#define RANS_LOWER_BOUND N148_RANS_LOWER_BOUND

typedef struct {
    uint16_t frequency;
    uint16_t cumulative;
} N148RansSymbolState;

static int adaptive_model_enabled(size_t model,
                                  size_t adaptive_model_limit,
                                  const uint8_t *flags,
                                  size_t flag_count) {
    if (flags)
        return model < flag_count &&
            ((flags[model >> 3] >> (model & 7u)) & 1u) != 0;
    return model < adaptive_model_limit;
}

static uint32_t read_u32_le(const uint8_t *source) {
    return (uint32_t) source[0] |
        ((uint32_t) source[1] << 8) |
        ((uint32_t) source[2] << 16) |
        ((uint32_t) source[3] << 24);
}

static void write_u32_le(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8);
    destination[2] = (uint8_t) (value >> 16);
    destination[3] = (uint8_t) (value >> 24);
}

static uint32_t adaptive_prefix_sum(const N148RansAdaptiveModel *model,
                                    unsigned int symbol) {
    uint32_t sum = 0;
    for (unsigned int index = symbol; index; index &= index - 1u)
        sum += model->fenwick[index];
    return sum;
}

static void adaptive_add(N148RansAdaptiveModel *model,
                         unsigned int symbol, int delta) {
    for (unsigned int index = symbol + 1u;
         index <= N148_RANS_ALPHABET_SIZE;
         index += index & (0u - index)) {
        model->fenwick[index] = (uint16_t)(
            (int) model->fenwick[index] + delta);
    }
}

static inline int bonus_higher(uint8_t left, uint8_t right,
                               const uint32_t remainder[256]) {
    /* The original scan gives the smaller symbol the first tied bonus. */
    return remainder[left] > remainder[right] ||
        (remainder[left] == remainder[right] && left < right);
}

static inline void bonus_heap_sift_down(uint8_t heap[256], int count,
                                        int start,
                                        const uint32_t remainder[256]) {
    int parent = start;
    uint8_t value = heap[parent];
    while (2 * parent + 1 < count) {
        int child = 2 * parent + 1;
        if (child + 1 < count &&
            bonus_higher(heap[child + 1], heap[child], remainder))
            child++;
        if (!bonus_higher(heap[child], value, remainder)) break;
        heap[parent] = heap[child];
        parent = child;
    }
    heap[parent] = value;
}

int n148_rans_adaptive_model_init(N148RansAdaptiveModel *adaptive,
                                  const N148RansModel *initial) {
    if (!adaptive || !initial || initial->used_symbols == 0) return 0;
    memset(adaptive, 0, sizeof(*adaptive));
    uint32_t total = 0;
    for (unsigned int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE;
         symbol++) {
        uint16_t frequency = initial->frequency[symbol];
        adaptive->frequency[symbol] = frequency;
        if (frequency) {
            adaptive->active_symbols[adaptive->used_symbols] = (uint8_t) symbol;
            adaptive->used_symbols++;
            adaptive_add(adaptive, symbol, frequency);
            total += frequency;
        }
    }
    return adaptive->used_symbols == initial->used_symbols &&
        total == N148_RANS_TOTAL;
}

int n148_rans_adaptive_model_update(N148RansAdaptiveModel *model,
                                    uint8_t observed,
                                    uint8_t adaptation_rate_shift) {
    if (!model || adaptation_rate_shift == 0 ||
        adaptation_rate_shift > 15 || model->frequency[observed] == 0 ||
        model->used_symbols == 0) return 0;
    if (model->used_symbols == 1) return 1;

    /* The total stays fixed, and every active symbol retains one count.
       Excluding the observed symbol leaves exactly this many transferable
       counts in the other active symbols. */
    uint32_t available = N148_RANS_TOTAL - model->used_symbols -
        ((uint32_t) model->frequency[observed] - 1u);
    uint32_t transfer = available >> adaptation_rate_shift;
    if (transfer == 0) return 1;
    /* If available = transfer * 2^shift + remainder, division of
       transfer * capacity needs only a shift and one signed correction. */
    uint32_t divisor_remainder =
        available - (transfer << adaptation_rate_shift);

    uint16_t deduction[N148_RANS_ALPHABET_SIZE] = {0};
    uint32_t remainder[N148_RANS_ALPHABET_SIZE] = {0};
    uint8_t bonus_heap[N148_RANS_ALPHABET_SIZE];
    uint32_t assigned = 0;
    for (unsigned int active = 0; active < model->used_symbols; active++) {
        unsigned int symbol = model->active_symbols[active];
        if (symbol == observed || model->frequency[symbol] <= 1) continue;
        uint32_t capacity = (uint32_t) model->frequency[symbol] - 1u;
        uint32_t base = capacity >> adaptation_rate_shift;
        int32_t excess = (int32_t)(
            transfer * (capacity - (base << adaptation_rate_shift))) -
            (int32_t)(base * divisor_remainder);
        deduction[symbol] = (uint16_t)(excess < 0 ? base - 1u : base);
        remainder[symbol] =
            (uint32_t)(excess < 0 ? excess + (int32_t) available : excess);
        assigned += deduction[symbol];
    }
    int bonus_count = 0;
    for (unsigned int active = 0; active < model->used_symbols; active++) {
        unsigned int symbol = model->active_symbols[active];
        if (symbol != observed &&
            deduction[symbol] < (uint32_t) model->frequency[symbol] - 1u)
            bonus_heap[bonus_count++] = (uint8_t) symbol;
    }
    for (int parent = bonus_count / 2 - 1; parent >= 0; parent--)
        bonus_heap_sift_down(bonus_heap, bonus_count, parent, remainder);
    while (assigned < transfer) {
        if (bonus_count == 0) return 0;
        unsigned int best = bonus_heap[0];
        deduction[best]++;
        assigned++;
        bonus_count--;
        if (bonus_count) {
            bonus_heap[0] = bonus_heap[bonus_count];
            bonus_heap_sift_down(bonus_heap, bonus_count, 0, remainder);
        }
    }

    for (unsigned int active = 0; active < model->used_symbols; active++) {
        unsigned int symbol = model->active_symbols[active];
        if (!deduction[symbol]) continue;
        model->frequency[symbol] = (uint16_t)(
            model->frequency[symbol] - deduction[symbol]);
        adaptive_add(model, symbol, -(int) deduction[symbol]);
    }
    model->frequency[observed] = (uint16_t)(
        model->frequency[observed] + transfer);
    adaptive_add(model, observed, (int) transfer);
    return adaptive_prefix_sum(model, N148_RANS_ALPHABET_SIZE) ==
        N148_RANS_TOTAL;
}

uint64_t n148_rans_adaptive_models_hash(
    const N148RansAdaptiveModel *models, size_t model_count) {
    uint64_t hash = 14695981039346656037ull;
    if (!models && model_count) return 0;
    for (size_t model = 0; model < model_count; model++) {
        for (unsigned int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE;
             symbol++) {
            uint16_t frequency = models[model].frequency[symbol];
            hash ^= (uint8_t) frequency;
            hash *= 1099511628211ull;
            hash ^= (uint8_t)(frequency >> 8);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

int n148_rans_prepare_model(N148RansModel *model) {
    if (!model) return 0;
    uint32_t cumulative = 0;
    uint16_t used = 0;
    for (int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE; symbol++) {
        model->cumulative[symbol] = (uint16_t) cumulative;
        uint32_t frequency = model->frequency[symbol];
        model->decode_pair[symbol] =
            (cumulative << 16) | frequency;
        if (frequency) {
            if (frequency > N148_RANS_TOTAL ||
                cumulative + frequency > N148_RANS_TOTAL) {
                return 0;
            }
            memset(model->symbol_for_slot + cumulative, symbol, frequency);
            cumulative += frequency;
            used++;
        }
    }
    model->cumulative[N148_RANS_ALPHABET_SIZE] = (uint16_t) cumulative;
    model->used_symbols = used;
    return used > 0 && cumulative == N148_RANS_TOTAL;
}

int n148_rans_build_model(const uint32_t counts[N148_RANS_ALPHABET_SIZE],
                          N148RansModel *model) {
    if (!counts || !model) return 0;
    memset(model, 0, sizeof(*model));

    uint64_t total = 0;
    int used = 0;
    for (int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE; symbol++) {
        total += counts[symbol];
        if (counts[symbol]) used++;
    }
    if (total == 0 || used > (int) N148_RANS_TOTAL) return 0;

    uint32_t normalized_total = 0;
    for (int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE; symbol++) {
        if (!counts[symbol]) continue;
        uint64_t scaled = (uint64_t) counts[symbol] * N148_RANS_TOTAL;
        uint32_t frequency = (uint32_t) (scaled / total);
        if (frequency == 0) frequency = 1;
        model->frequency[symbol] = (uint16_t) frequency;
        normalized_total += frequency;
    }

    /* Largest-remainder balancing expressed with integers. The score is the
       ideal scaled count minus the currently assigned count, so adding one
       lowers it by exactly `total` and subtracting one raises it likewise. */
    while (normalized_total < N148_RANS_TOTAL) {
        int best = -1;
        int64_t best_score = INT64_MIN;
        for (int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE; symbol++) {
            if (!counts[symbol]) continue;
            int64_t score =
                (int64_t)((uint64_t) counts[symbol] * N148_RANS_TOTAL) -
                (int64_t)((uint64_t) model->frequency[symbol] * total);
            if (score > best_score) {
                best_score = score;
                best = symbol;
            }
        }
        if (best < 0 || model->frequency[best] == UINT16_MAX) return 0;
        model->frequency[best]++;
        normalized_total++;
    }
    while (normalized_total > N148_RANS_TOTAL) {
        int best = -1;
        int64_t best_score = INT64_MAX;
        for (int symbol = 0; symbol < N148_RANS_ALPHABET_SIZE; symbol++) {
            if (model->frequency[symbol] <= 1) continue;
            int64_t score =
                (int64_t)((uint64_t) counts[symbol] * N148_RANS_TOTAL) -
                (int64_t)((uint64_t) model->frequency[symbol] * total);
            if (score < best_score) {
                best_score = score;
                best = symbol;
            }
        }
        if (best < 0) return 0;
        model->frequency[best]--;
        normalized_total--;
    }
    return n148_rans_prepare_model(model);
}

int n148_rans_encode_mixed(const uint8_t *symbols,
                           const uint8_t *model_indexes,
                           size_t symbol_count,
                           const N148RansModel *models, size_t model_count,
                           uint8_t **stream, size_t *stream_size) {
    if ((!symbols && symbol_count) || (!model_indexes && symbol_count) ||
        !models || model_count == 0 || model_count > 256 || !stream ||
        !stream_size) {
        return 0;
    }
    *stream = NULL;
    *stream_size = 0;
    if (symbol_count > (SIZE_MAX - 16) / 2) return 0;
    size_t capacity = symbol_count * 2 + 16;
    uint8_t *storage = (uint8_t *) malloc(capacity);
    if (!storage) return 0;
    uint8_t *begin = storage;
    uint8_t *cursor = storage + capacity;
    uint32_t state = RANS_LOWER_BOUND;

    for (size_t remaining = symbol_count; remaining > 0; remaining--) {
        uint8_t symbol = symbols[remaining - 1];
        uint8_t model_index = model_indexes[remaining - 1];
        if (model_index >= model_count ||
            models[model_index].used_symbols == 0) {
            free(storage);
            return 0;
        }
        const N148RansModel *model = &models[model_index];
        uint32_t frequency = model->frequency[symbol];
        uint32_t cumulative = model->cumulative[symbol];
        if (frequency == 0) {
            free(storage);
            return 0;
        }
        uint32_t threshold =
            ((RANS_LOWER_BOUND >> N148_RANS_SCALE_BITS) << 8) * frequency;
        while (state >= threshold) {
            if (cursor == begin) {
                free(storage);
                return 0;
            }
            *--cursor = (uint8_t) state;
            state >>= 8;
        }
        state = (state / frequency) * N148_RANS_TOTAL +
            state % frequency + cumulative;
    }
    if ((size_t)(cursor - begin) < 4) {
        free(storage);
        return 0;
    }
    cursor -= 4;
    write_u32_le(cursor, state);
    size_t used = (size_t)((storage + capacity) - cursor);
    memmove(storage, cursor, used);
    uint8_t *compact = (uint8_t *) realloc(storage, used);
    if (compact) storage = compact;
    *stream = storage;
    *stream_size = used;
    return 1;
}

int n148_rans_encode_mixed_adaptive(
    const uint8_t *symbols, const uint8_t *model_indexes,
    size_t symbol_count, const N148RansModel *models, size_t model_count,
    size_t adaptive_model_limit, const uint8_t *adaptive_model_flags,
    size_t adaptive_model_flag_count, uint8_t adaptation_rate_shift,
    uint8_t **stream, size_t *stream_size, uint64_t *final_model_hash) {
    if ((!symbols && symbol_count) || (!model_indexes && symbol_count) ||
        !models || model_count == 0 || model_count > 256 || !stream ||
        !stream_size || adaptive_model_limit > model_count ||
        (adaptive_model_flags &&
         adaptive_model_flag_count > model_count) ||
        adaptation_rate_shift == 0 ||
        adaptation_rate_shift > 15 ||
        symbol_count > SIZE_MAX / sizeof(N148RansSymbolState)) return 0;
    *stream = NULL;
    *stream_size = 0;
    if (final_model_hash) *final_model_hash = 0;
    uint64_t computed_model_hash = 0;

    N148RansAdaptiveModel *adaptive = (N148RansAdaptiveModel *) calloc(
        model_count, sizeof(*adaptive));
    N148RansSymbolState *states = symbol_count ?
        (N148RansSymbolState *) malloc(symbol_count * sizeof(*states)) : NULL;
    if (!adaptive || (symbol_count && !states)) {
        free(adaptive);
        free(states);
        return 0;
    }
    for (size_t model = 0; model < model_count; model++) {
        if (!n148_rans_adaptive_model_init(&adaptive[model],
                                           &models[model])) {
#ifdef N148_RANS_DIAGNOSTICS
            fprintf(stderr, "adaptive init failed for model %zu\n", model);
#endif
            free(adaptive);
            free(states);
            return 0;
        }
    }
    for (size_t index = 0; index < symbol_count; index++) {
        uint8_t model_index = model_indexes[index];
        uint8_t symbol = symbols[index];
        if (model_index >= model_count ||
            adaptive[model_index].frequency[symbol] == 0) {
#ifdef N148_RANS_DIAGNOSTICS
            fprintf(stderr, "adaptive missing symbol at %zu model %u symbol %u\n",
                    index, model_index, symbol);
#endif
            free(adaptive);
            free(states);
            return 0;
        }
        states[index].frequency = adaptive[model_index].frequency[symbol];
        states[index].cumulative = (uint16_t) adaptive_prefix_sum(
            &adaptive[model_index], symbol);
        if (adaptive_model_enabled(
                model_index, adaptive_model_limit, adaptive_model_flags,
                adaptive_model_flag_count) &&
            !n148_rans_adaptive_model_update(
                &adaptive[model_index], symbol, adaptation_rate_shift)) {
#ifdef N148_RANS_DIAGNOSTICS
            fprintf(stderr, "adaptive update failed at %zu model %u symbol %u\n",
                    index, model_index, symbol);
#endif
            free(adaptive);
            free(states);
            return 0;
        }
    }
    if (final_model_hash)
        computed_model_hash = n148_rans_adaptive_models_hash(
            adaptive, model_count);
    free(adaptive);

    if (symbol_count > (SIZE_MAX - 16) / 4) {
        free(states);
        return 0;
    }
    size_t capacity = symbol_count * 4 + 16;
    uint8_t *storage = (uint8_t *) malloc(capacity);
    if (!storage) {
        free(states);
        return 0;
    }
    uint8_t *begin = storage;
    uint8_t *cursor = storage + capacity;
    uint32_t state = RANS_LOWER_BOUND;
    for (size_t remaining = symbol_count; remaining > 0; remaining--) {
        uint32_t frequency = states[remaining - 1].frequency;
        uint32_t cumulative = states[remaining - 1].cumulative;
        uint32_t threshold =
            ((RANS_LOWER_BOUND >> N148_RANS_SCALE_BITS) << 8) * frequency;
        while (state >= threshold) {
            if (cursor == begin) {
                free(storage);
                free(states);
                return 0;
            }
            *--cursor = (uint8_t) state;
            state >>= 8;
        }
        state = (state / frequency) * N148_RANS_TOTAL +
            state % frequency + cumulative;
    }
    free(states);
    if ((size_t)(cursor - begin) < 4) {
        free(storage);
        return 0;
    }
    cursor -= 4;
    write_u32_le(cursor, state);
    size_t used = (size_t)((storage + capacity) - cursor);
    memmove(storage, cursor, used);
    uint8_t *compact = (uint8_t *) realloc(storage, used);
    if (compact) storage = compact;
    *stream = storage;
    *stream_size = used;
    if (final_model_hash) *final_model_hash = computed_model_hash;
    return 1;
}

int n148_rans_encode(const uint8_t *symbols, size_t symbol_count,
                     const N148RansModel *model,
                     uint8_t **stream, size_t *stream_size) {
    if ((!symbols && symbol_count) || !model) return 0;
    uint8_t *indexes = NULL;
    if (symbol_count) {
        indexes = (uint8_t *) calloc(symbol_count, 1);
        if (!indexes) return 0;
    }
    int result = n148_rans_encode_mixed(
        symbols, indexes, symbol_count, model, 1, stream, stream_size);
    free(indexes);
    return result;
}

int n148_rans_decoder_init(N148RansDecoder *decoder,
                           const N148RansModel *model,
                           const uint8_t *stream, size_t stream_size) {
    if (!decoder || !model || !stream || stream_size < 4 ||
        model->used_symbols == 0) {
        return 0;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->model = model;
    decoder->state = read_u32_le(stream);
    decoder->next = stream + 4;
    decoder->end = stream + stream_size;
    decoder->valid = decoder->state >= RANS_LOWER_BOUND;
    return decoder->valid;
}

int n148_rans_decoder_init_mixed(N148RansDecoder *decoder,
                                 const uint8_t *stream, size_t stream_size) {
    if (!decoder || !stream || stream_size < 4) return 0;
    memset(decoder, 0, sizeof(*decoder));
    decoder->model = NULL;
    decoder->state = read_u32_le(stream);
    decoder->next = stream + 4;
    decoder->end = stream + stream_size;
    decoder->valid = decoder->state >= RANS_LOWER_BOUND;
    return decoder->valid;
}

int n148_rans_decoder_init_mixed_adaptive(
    N148RansDecoder *decoder, const uint8_t *stream, size_t stream_size,
    const N148RansModel *initial_models,
    N148RansAdaptiveModel *adaptive_models, size_t model_count,
    size_t adaptive_model_limit, const uint8_t *adaptive_model_flags,
    size_t adaptive_model_flag_count, uint8_t adaptation_rate_shift) {
    if (!decoder || !initial_models || !adaptive_models ||
        model_count == 0 || model_count > 256 ||
        adaptive_model_limit > model_count ||
        (adaptive_model_flags && adaptive_model_flag_count > model_count) ||
        adaptation_rate_shift == 0 || adaptation_rate_shift > 15 ||
        !n148_rans_decoder_init_mixed(decoder, stream, stream_size)) return 0;
    for (size_t model = 0; model < model_count; model++) {
        if (!n148_rans_adaptive_model_init(&adaptive_models[model],
                                           &initial_models[model])) {
            decoder->valid = 0;
            return 0;
        }
    }
    decoder->model_base = initial_models;
    decoder->adaptive_models = adaptive_models;
    decoder->adaptive_model_count = model_count;
    decoder->adaptive_model_limit = adaptive_model_limit;
    decoder->adaptive_model_flags = adaptive_model_flags;
    decoder->adaptive_model_flag_count = adaptive_model_flag_count;
    decoder->adaptation_rate_shift = adaptation_rate_shift;
    return 1;
}

static int decode_symbol_adaptive(N148RansDecoder *decoder,
                                  N148RansAdaptiveModel *model,
                                  int update_model, uint8_t *symbol) {
    uint32_t slot = decoder->state & (N148_RANS_TOTAL - 1u);
    unsigned int index = 0;
    uint32_t cumulative = 0;
    for (unsigned int step = N148_RANS_ALPHABET_SIZE; step; step >>= 1) {
        unsigned int next = index + step;
        if (next <= N148_RANS_ALPHABET_SIZE &&
            cumulative + model->fenwick[next] <= slot) {
            index = next;
            cumulative += model->fenwick[next];
        }
    }
    if (index >= N148_RANS_ALPHABET_SIZE ||
        model->frequency[index] == 0) {
        decoder->valid = 0;
        return 0;
    }
    uint32_t frequency = model->frequency[index];
    decoder->state = frequency *
        (decoder->state >> N148_RANS_SCALE_BITS) + slot - cumulative;
    while (decoder->state < RANS_LOWER_BOUND) {
        if (decoder->next == decoder->end) {
            decoder->valid = 0;
            return 0;
        }
        decoder->state = (decoder->state << 8) | *decoder->next++;
    }
    *symbol = (uint8_t) index;
    if (update_model && !n148_rans_adaptive_model_update(
            model, *symbol, decoder->adaptation_rate_shift)) {
        decoder->valid = 0;
        return 0;
    }
    return 1;
}

int n148_rans_decode_symbol_with_model(N148RansDecoder *decoder,
                                       const N148RansModel *model,
                                       uint8_t *symbol) {
    if (!decoder || !model || !symbol || !decoder->valid ||
        model->used_symbols == 0) return 0;
    if (decoder->adaptive_models) {
        if (!decoder->model_base || model < decoder->model_base ||
            model >= decoder->model_base + decoder->adaptive_model_count) {
            decoder->valid = 0;
            return 0;
        }
        size_t model_index = (size_t)(model - decoder->model_base);
        return decode_symbol_adaptive(
            decoder, &decoder->adaptive_models[model_index],
            adaptive_model_enabled(
                model_index, decoder->adaptive_model_limit,
                decoder->adaptive_model_flags,
                decoder->adaptive_model_flag_count), symbol);
    }
    uint32_t slot = decoder->state & (N148_RANS_TOTAL - 1u);
    uint8_t decoded = model->symbol_for_slot[slot];
    uint32_t frequency = model->frequency[decoded];
    uint32_t cumulative = model->cumulative[decoded];
    if (frequency == 0 || slot < cumulative || slot >= cumulative + frequency) {
        decoder->valid = 0;
        return 0;
    }
    decoder->state = frequency *
        (decoder->state >> N148_RANS_SCALE_BITS) + slot - cumulative;
    while (decoder->state < RANS_LOWER_BOUND) {
        if (decoder->next == decoder->end) {
            decoder->valid = 0;
            return 0;
        }
        decoder->state = (decoder->state << 8) | *decoder->next++;
    }
    *symbol = decoded;
    return 1;
}

int n148_rans_decode_symbol(N148RansDecoder *decoder, uint8_t *symbol) {
    if (!decoder || !decoder->model) return 0;
    return n148_rans_decode_symbol_with_model(
        decoder, decoder->model, symbol);
}

int n148_rans_decoder_finished(const N148RansDecoder *decoder) {
    return decoder && decoder->valid && decoder->next == decoder->end &&
        decoder->state == RANS_LOWER_BOUND;
}

#include <string.h>

#include "huffman.h"
#include "tables.h"

#define RESERVED_SYMBOL 256
#define MAX_CODE_LENGTH 32

static void copy_spec(HuffSpec *spec, const int bits[17],
                      const unsigned char *values, int value_count) {
    memcpy(spec->bits, bits, sizeof(spec->bits));
    memcpy(spec->values, values, (size_t)value_count);
    spec->value_count = value_count;
}

void huffman_default_specs(HuffSpec specs[HUFFMAN_TABLE_COUNT]) {
    copy_spec(&specs[HUFFMAN_DC_LUMA],
              BITS_DC_LUMA, VAL_DC_LUMA, 12);
    copy_spec(&specs[HUFFMAN_AC_LUMA],
              BITS_AC_LUMA, VAL_AC_LUMA, 162);
    copy_spec(&specs[HUFFMAN_DC_CHROMA],
              BITS_DC_CHROMA, VAL_DC_CHROMA, 12);
    copy_spec(&specs[HUFFMAN_AC_CHROMA],
              BITS_AC_CHROMA, VAL_AC_CHROMA, 162);
}

// JPEG Annex K.2 computes code lengths without allocating a tree. The
// others array links every symbol already merged into the same branch.
static void compute_code_sizes(const long frequency_input[256],
                               int code_size[257]) {
    long frequency[257];
    int others[257];

    for (int i = 0; i < 256; i++) {
        frequency[i] = frequency_input[i];
    }
    frequency[RESERVED_SYMBOL] = 1;

    for (int i = 0; i <= RESERVED_SYMBOL; i++) {
        code_size[i] = 0;
        others[i] = -1;
    }

    for (;;) {
        int first = -1;
        int second = -1;
        long first_frequency = 0;
        long second_frequency = 0;

        // Ties deliberately select the highest symbol, matching Annex K.
        for (int i = 0; i <= RESERVED_SYMBOL; i++) {
            if (frequency[i] == 0) {
                continue;
            }
            if (first < 0 || frequency[i] <= first_frequency) {
                second = first;
                second_frequency = first_frequency;
                first = i;
                first_frequency = frequency[i];
            } else if (second < 0 || frequency[i] <= second_frequency) {
                second = i;
                second_frequency = frequency[i];
            }
        }

        if (second < 0) {
            break;
        }

        frequency[first] += frequency[second];
        frequency[second] = 0;

        code_size[first]++;
        while (others[first] != -1) {
            first = others[first];
            code_size[first]++;
        }
        others[first] = second;

        code_size[second]++;
        while (others[second] != -1) {
            second = others[second];
            code_size[second]++;
        }
    }
}

// Squeeze codes longer than 16 bits down to JPEG's canonical-table limit.
static int limit_to_16_bits(int bits[33]) {
    for (int length = MAX_CODE_LENGTH; length > 16; length--) {
        while (bits[length] > 0) {
            int shorter = length - 2;
            while (shorter > 0 && bits[shorter] == 0) {
                shorter--;
            }
            if (shorter == 0 || bits[length] < 2) {
                return 0;
            }

            bits[length] -= 2;
            bits[length - 1]++;
            bits[shorter + 1] += 2;
            bits[shorter]--;
        }
    }
    return 1;
}

int huffman_build_optimized(const long frequencies[256], HuffSpec *spec) {
    int code_size[257];
    int bits[33] = {0};
    int symbol_count = 0;

    compute_code_sizes(frequencies, code_size);

    for (int symbol = 0; symbol <= RESERVED_SYMBOL; symbol++) {
        int length = code_size[symbol];
        if (length < 0 || length > MAX_CODE_LENGTH) {
            return 0;
        }
        if (length > 0) {
            bits[length]++;
        }
        if (symbol < RESERVED_SYMBOL && frequencies[symbol] > 0) {
            symbol_count++;
        }
    }

    if (symbol_count == 0 || !limit_to_16_bits(bits)) {
        return 0;
    }

    // Remove the reserved all-ones code from the longest occupied length.
    int longest = 16;
    while (longest > 0 && bits[longest] == 0) {
        longest--;
    }
    if (longest == 0) {
        return 0;
    }
    bits[longest]--;

    memset(spec, 0, sizeof(*spec));
    for (int length = 1; length <= 16; length++) {
        spec->bits[length] = bits[length];
    }

    // Annex K orders equal-length symbols by symbol value. Length limiting
    // changes the counts, but the original order remains canonical.
    for (int length = 1; length <= MAX_CODE_LENGTH; length++) {
        for (int symbol = 0; symbol < 256; symbol++) {
            if (code_size[symbol] == length) {
                spec->values[spec->value_count++] =
                    (unsigned char)symbol;
            }
        }
    }

    return spec->value_count == symbol_count &&
           huffman_spec_is_valid(spec);
}

int huffman_spec_is_valid(const HuffSpec *spec) {
    if (!spec || spec->value_count <= 0 ||
        spec->value_count > HUFFMAN_MAX_SYMBOLS) {
        return 0;
    }

    int count = 0;
    int code = 0;
    for (int length = 1; length <= 16; length++) {
        int length_count = spec->bits[length];
        if (length_count < 0 || length_count > 255 ||
            code + length_count > (1 << length)) {
            return 0;
        }
        count += length_count;
        code = (code + length_count) << 1;
    }
    if (count != spec->value_count) {
        return 0;
    }

    unsigned char seen[256] = {0};
    for (int i = 0; i < spec->value_count; i++) {
        if (seen[spec->values[i]]) {
            return 0;
        }
        seen[spec->values[i]] = 1;
    }
    return 1;
}

long huffman_tables_size(const HuffSpec specs[HUFFMAN_TABLE_COUNT]) {
    long size = 0;
    for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
        size += 16 + specs[table].value_count;
    }
    return size;
}

int write_huffman_tables(FILE *file,
                         const HuffSpec specs[HUFFMAN_TABLE_COUNT]) {
    for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
        if (!huffman_spec_is_valid(&specs[table])) {
            return 0;
        }
        for (int length = 1; length <= 16; length++) {
            if (fputc(specs[table].bits[length], file) == EOF) {
                return 0;
            }
        }
        if (fwrite(specs[table].values, 1,
                   (size_t)specs[table].value_count, file) !=
            (size_t)specs[table].value_count) {
            return 0;
        }
    }
    return ferror(file) == 0;
}

int read_huffman_tables(FILE *file,
                        HuffSpec specs[HUFFMAN_TABLE_COUNT]) {
    for (int table = 0; table < HUFFMAN_TABLE_COUNT; table++) {
        HuffSpec *spec = &specs[table];
        memset(spec, 0, sizeof(*spec));

        for (int length = 1; length <= 16; length++) {
            int count = fgetc(file);
            if (count == EOF) {
                return 0;
            }
            spec->bits[length] = count;
            spec->value_count += count;
            if (spec->value_count > HUFFMAN_MAX_SYMBOLS) {
                return 0;
            }
        }

        if (spec->value_count == 0 ||
            fread(spec->values, 1, (size_t)spec->value_count, file) !=
                (size_t)spec->value_count ||
            !huffman_spec_is_valid(spec)) {
            return 0;
        }
    }
    return 1;
}

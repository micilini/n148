#include <math.h>

#include "dct.h"

#define BLOCK_SIZE 8
#define PI 3.14159265358979323846

static double cos_table[BLOCK_SIZE][BLOCK_SIZE];
static double alpha[BLOCK_SIZE];
static int tables_ready = 0;

void init_dct_tables(void) {
    if (tables_ready) {
        return;
    }

    for (int frequency = 0; frequency < BLOCK_SIZE; frequency++) {
        alpha[frequency] =
            (frequency == 0) ? (1.0 / sqrt(2.0)) : 1.0;

        for (int position = 0; position < BLOCK_SIZE; position++) {
            cos_table[frequency][position] =
                cos((2.0 * position + 1.0) * frequency * PI /
                    (2.0 * BLOCK_SIZE));
        }
    }

    tables_ready = 1;
}

static void dct_1d(double input[BLOCK_SIZE], double output[BLOCK_SIZE]) {
    for (int frequency = 0; frequency < BLOCK_SIZE; frequency++) {
        double sum = 0.0;
        for (int position = 0; position < BLOCK_SIZE; position++) {
            sum += input[position] * cos_table[frequency][position];
        }
        output[frequency] = 0.5 * alpha[frequency] * sum;
    }
}

void dct_block(double block[8][8], double coefficients[8][8]) {
    double temporary[8][8];

    for (int row = 0; row < 8; row++) {
        double input[8];
        double output[8];
        for (int column = 0; column < 8; column++) {
            input[column] = block[row][column] - 128.0;
        }
        dct_1d(input, output);
        for (int column = 0; column < 8; column++) {
            temporary[row][column] = output[column];
        }
    }

    for (int column = 0; column < 8; column++) {
        double input[8];
        double output[8];
        for (int row = 0; row < 8; row++) {
            input[row] = temporary[row][column];
        }
        dct_1d(input, output);
        for (int row = 0; row < 8; row++) {
            coefficients[row][column] = output[row];
        }
    }
}

static void idct_1d(double input[BLOCK_SIZE], double output[BLOCK_SIZE]) {
    for (int position = 0; position < BLOCK_SIZE; position++) {
        double sum = 0.0;
        for (int frequency = 0; frequency < BLOCK_SIZE; frequency++) {
            sum += alpha[frequency] * input[frequency] *
                   cos_table[frequency][position];
        }
        output[position] = 0.5 * sum;
    }
}

void idct_block(double coefficients[8][8], double block[8][8]) {
    double temporary[8][8];

    // Columns first, then rows: the mirror image of the forward pass.
    for (int column = 0; column < 8; column++) {
        double input[8];
        double output[8];
        for (int row = 0; row < 8; row++) {
            input[row] = coefficients[row][column];
        }
        idct_1d(input, output);
        for (int row = 0; row < 8; row++) {
            temporary[row][column] = output[row];
        }
    }

    for (int row = 0; row < 8; row++) {
        double input[8];
        double output[8];
        for (int column = 0; column < 8; column++) {
            input[column] = temporary[row][column];
        }
        idct_1d(input, output);
        for (int column = 0; column < 8; column++) {
            block[row][column] = output[column] + 128.0;
        }
    }
}

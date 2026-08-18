#ifndef HEADER_H
#define HEADER_H

#include <stdint.h>
#include <stdio.h>

#define N148I_MAGIC "N148I"
#define N148I_MAGIC_LEN 5
#define N148I_VERSION 2

// Chroma subsampling modes.
#define CHROMA_444 0
#define CHROMA_420 2

typedef struct {
    uint8_t version;
    uint32_t width;
    uint32_t height;
    uint8_t quality;
    uint8_t chroma;
    uint32_t data_size; // Number of compressed bytes after the header.
} N148iHeader;

void write_u8(FILE *file, uint8_t value);
void write_u32(FILE *file, uint32_t value);
uint8_t read_u8(FILE *file);
uint32_t read_u32(FILE *file);

int write_header(FILE *file, N148iHeader *header);
int read_header(FILE *file, N148iHeader *header);
const char *chroma_name(uint8_t chroma);

#endif

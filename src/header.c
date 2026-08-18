#include <string.h>

#include "header.h"

void write_u8(FILE *file, uint8_t value) {
    fputc(value, file);
}

void write_u32(FILE *file, uint32_t value) {
    fputc((value) & 0xFF, file); // Little-endian, byte by byte.
    fputc((value >> 8) & 0xFF, file);
    fputc((value >> 16) & 0xFF, file);
    fputc((value >> 24) & 0xFF, file);
}

uint8_t read_u8(FILE *file) {
    return (uint8_t)fgetc(file);
}

uint32_t read_u32(FILE *file) {
    uint32_t b0 = (uint32_t)fgetc(file);
    uint32_t b1 = (uint32_t)fgetc(file);
    uint32_t b2 = (uint32_t)fgetc(file);
    uint32_t b3 = (uint32_t)fgetc(file);

    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

int write_header(FILE *file, N148iHeader *header) {
    fwrite(N148I_MAGIC, 1, N148I_MAGIC_LEN, file);
    write_u8(file, header->version);
    write_u32(file, header->width);
    write_u32(file, header->height);
    write_u8(file, header->quality);
    write_u8(file, header->chroma);
    write_u32(file, header->data_size);
    return 1;
}

int read_header(FILE *file, N148iHeader *header) {
    char magic[6] = {0};
    if (fread(magic, 1, N148I_MAGIC_LEN, file) != N148I_MAGIC_LEN) {
        return 0;
    }
    if (memcmp(magic, N148I_MAGIC, N148I_MAGIC_LEN) != 0) {
        return 0;
    }

    header->version = read_u8(file);
    header->width = read_u32(file);
    header->height = read_u32(file);
    header->quality = read_u8(file);
    header->chroma = read_u8(file);
    header->data_size = read_u32(file);
    return 1;
}

const char *chroma_name(uint8_t chroma) {
    switch (chroma) {
        case CHROMA_444:
            return "4:4:4";
        case 1:
            return "4:2:2";
        case CHROMA_420:
            return "4:2:0";
        default:
            return "unknown";
    }
}

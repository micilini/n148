#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The signature (magic number) of our format.
#define N148I_MAGIC "N148I"
#define N148I_MAGIC_LEN 5

// The "box label": everything the decoder needs to know.
typedef struct {
    char magic[6]; // "N148I" + '\0' terminator
    uint8_t version;
    uint32_t width;
    uint32_t height;
    uint8_t quality;
    uint8_t chroma; // 0 = 4:4:4, 1 = 4:2:2, 2 = 4:2:0
} N148iHeader;

// ============================================================
// WRITING NUMBERS (byte by byte, little-endian)
// ============================================================
//
// We write each byte by hand to guarantee the same order on
// any computer. Never dump the struct straight into the file!

void write_u8(FILE *file, uint8_t value) {
    fputc(value, file);
}

void write_u32(FILE *file, uint32_t value) {
    fputc((value) & 0xFF, file);       // Least significant byte first.
    fputc((value >> 8) & 0xFF, file);
    fputc((value >> 16) & 0xFF, file);
    fputc((value >> 24) & 0xFF, file); // Most significant byte last.
}

// ============================================================
// SAVE THE HEADER TO A FILE
// ============================================================

int save_header(const char *path, N148iHeader *header) {
    FILE *file = fopen(path, "wb"); // "wb" = write binary.
    if (!file) {
        printf("Could not create the file!\n");
        return 0;
    }

    fwrite(N148I_MAGIC, 1, N148I_MAGIC_LEN, file);
    write_u8(file, header->version);
    write_u32(file, header->width);
    write_u32(file, header->height);
    write_u8(file, header->quality);
    write_u8(file, header->chroma);

    fclose(file);
    return 1;
}

// ============================================================
// READING NUMBERS (in the same order we wrote them)
// ============================================================

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

// ============================================================
// LOAD THE HEADER FROM A FILE
// ============================================================

int load_header(const char *path, N148iHeader *header) {
    FILE *file = fopen(path, "rb"); // "rb" = read binary.
    if (!file) {
        printf("Could not open the file!\n");
        return 0;
    }

    // First, check the signature.
    char magic[6] = {0};
    fread(magic, 1, N148I_MAGIC_LEN, file);
    if (memcmp(magic, N148I_MAGIC, N148I_MAGIC_LEN) != 0) {
        printf("This is NOT a valid N148i file!\n");
        fclose(file);
        return 0;
    }

    // The signature matches: read the remaining fields in storage order.
    strcpy(header->magic, N148I_MAGIC);
    header->version = read_u8(file);
    header->width = read_u32(file);
    header->height = read_u32(file);
    header->quality = read_u8(file);
    header->chroma = read_u8(file);

    fclose(file);
    return 1;
}

const char *chroma_name(uint8_t chroma) {
    switch (chroma) {
        case 0:
            return "4:4:4";
        case 1:
            return "4:2:2";
        case 2:
            return "4:2:0";
        default:
            return "unknown";
    }
}

// ============================================================
// MAIN
// ============================================================

int main(void) {
    // Build an example header: 640x480 image, quality 50, 4:2:0.
    N148iHeader header;
    strcpy(header.magic, N148I_MAGIC);
    header.version = 1;
    header.width = 640;
    header.height = 480;
    header.quality = 50;
    header.chroma = 2;

    // Save to disk in output/, one level above src/.
    if (!save_header("../output/image.n148i", &header)) {
        return EXIT_FAILURE;
    }
    printf("File '../output/image.n148i' saved successfully!\n\n");

    // Read the file back and display the decoded header.
    N148iHeader loaded;
    if (!load_header("../output/image.n148i", &loaded)) {
        return EXIT_FAILURE;
    }

    printf("--- Header read from file ---\n");
    printf("Signature: %s\n", loaded.magic);
    printf("Version:   %d\n", loaded.version);
    printf("Size:      %u x %u pixels\n", loaded.width, loaded.height);
    printf("Quality:   %d\n", loaded.quality);
    printf("Chroma:    %s\n", chroma_name(loaded.chroma));

    return EXIT_SUCCESS;
}

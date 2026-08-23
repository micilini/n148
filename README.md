# N.148

N.148 is a handcrafted image and video codec written in C and built from first principles. The project follows the complete compression pipeline—from pixels and color channels to DCT, quantization, zig-zag ordering, run-length encoding, Huffman coding, and a portable binary file format.

This repository grows alongside the [Micilini codec series](https://micilini.com/conteudos/codecs), where every stage is developed and explained step by step.

---

## About the project

N.148 is an educational exploration of how modern media codecs work internally. Instead of treating compression as a black box, it implements every building block in plain C so the transformations, trade-offs, and binary representation remain visible.

The project is split into two planned formats:

| Format | Purpose | File extension |
| --- | --- | --- |
| **N.148i** | Static images such as photographs, illustrations, and screenshots | `.n148i` |
| **N.148v** | Video streams and moving images | To be defined |

The current milestone refines that complete round trip with selectable chroma subsampling, bilinear reconstruction, and image-specific Huffman tables.

---

## Complete round trip

The executable now performs both directions of the codec:

```text
ENCODER                                  DECODER
P6 PPM input                             N.148i v3 header
    ↓                                         ↓
RGB → YCbCr                              Huffman decoding
    ↓                                         ↓
4:4:4 / 4:2:2 / 4:2:0 chroma            Run-length expansion
    ↓                                         ↓
8 × 8 blocks + edge padding              Inverse zig-zag
    ↓                                         ↓
DCT → quantization                       Dequantization → IDCT
    ↓                                         ↓
Zig-zag → RLE → optimized Huffman        Block reconstruction
    ↓                                         ↓
Header + tables + bitstream              Bilinear chroma upsampling
                                              ↓
                                         YCbCr → RGB
                                              ↓
                                         Decoded P6 PPM
```

The decoder reverses the encoder in the exact opposite order. Plane ordering is part of the format contract: all Y blocks are stored first, followed by Cb and then Cr, with an independent DC predictor for each plane.

---

## Current refinement milestone

This milestone adds:

- bilinear chroma upsampling with pixel-centre alignment;
- selectable 4:4:4, 4:2:2, and 4:2:0 chroma modes;
- generic chroma downsampling with independent horizontal and vertical steps;
- two-pass entropy encoding with a single shared tokenization path;
- image-specific, canonical Huffman tables limited to 16-bit codes;
- compact serialization of the four custom Huffman tables;
- an N.148i v3 header that records whether custom tables are present;
- continued support for the standard JPEG Huffman tables as an option.

The output is lossy by design. `decoded.ppm` has the same dimensions as the original, but its pixel values reflect quantization and chroma subsampling.

---

## Shared codec modules

Encoder and decoder correctness depends on both sides using identical tables and transforms. Shared definitions therefore live in dedicated modules:

- `tables.c` owns quantization tables, zig-zag order, Huffman definitions, and quality scaling;
- `huffman.c` builds, validates, serializes, and reads canonical Huffman specifications;
- `dct.c` owns the cosine table, forward DCT, and inverse DCT;
- `encoder.c` tokenizes blocks once for both frequency counting and bit writing;
- `decoder.c` contains only the reverse entropy and reconstruction pipeline.

This prevents a table change on one side from silently making newly encoded files incompatible with the decoder.

---

## Reference image

The bundled [`images/example.ppm`](images/example.ppm) is the official 320 × 240 P6 fixture from the Micilini lessons. Its original source is available at [micilini.com/assets/img/example.ppm](https://micilini.com/assets/img/example.ppm).

| Property | Value |
| --- | ---: |
| Dimensions | 320 × 240 pixels |
| Pixel representation | RGB24, binary P6 |
| File size | 230,415 bytes |
| SHA-256 | `0bee9996ff9e43c9429a896cfba98dd183419426d28d31bbfcd65ef08ab2a404` |

---

## N.148i version 3 format

The decoder reads a 21-byte little-endian header:

| Offset | Size | Field | Reference value | Description |
| ---: | ---: | --- | --- | --- |
| `0` | 5 bytes | Signature | `N148I` | Identifies the N.148i format |
| `5` | 1 byte | Version | `3` | Current file-format version |
| `6` | 4 bytes | Width | `320` | Original image width |
| `10` | 4 bytes | Height | `240` | Original image height |
| `14` | 1 byte | Quality | `50` | Quantization quality required for decoding |
| `15` | 1 byte | Chroma | `2` | `0` = 4:4:4, `1` = 4:2:2, `2` = 4:2:0 |
| `16` | 1 byte | Optimized | `1` | Custom Huffman tables follow the header |
| `17` | 4 bytes | Data size | `2145` | Entropy payload size, excluding custom tables |

Reference header bytes at quality 50:

```text
4e 31 34 38 49 03 40 01 00 00 f0 00 00 00 32 02 01 61 08 00 00
```

When `optimized` is one, four tables follow the header. Each table stores 16 code-length counts followed by the corresponding canonical symbol list. The declared payload size still lets the program reject truncated entropy data.

---

## Repository structure

```text
n148/
├── src/
│   ├── main.c          # Runs the encode/decode cycle and reports PSNR
│   ├── header.h/.c     # N.148i v3 header serialization
│   ├── ppm.h/.c        # PPM I/O, color conversion, and channel planes
│   ├── tables.h/.c     # Shared quantization, zig-zag, and Huffman tables
│   ├── huffman.h/.c    # Optimized Huffman construction and table I/O
│   ├── dct.h/.c        # Shared forward and inverse DCT
│   ├── encoder.h/.c    # Forward block and entropy pipeline
│   └── decoder.h/.c    # Reverse pipeline and RGB reconstruction
├── images/
│   └── example.ppm     # Official 320 × 240 Micilini fixture
├── output/
│   ├── image.n148i     # Compressed N.148i v3 file
│   └── decoded.ppm     # Reconstructed P6 image
├── .gitattributes
├── .gitignore
├── LICENSE
└── README.md
```

---

## Build and run

### Requirements

- A C compiler with C11 support, such as GCC or Clang
- The standard C math library
- No third-party codec libraries

From the `src/` directory:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic \
  main.c header.c ppm.c tables.c dct.c huffman.c encoder.c decoder.c \
  -o n148i -lm
./n148i
```

Expected output at quality 50:

```text
=== N.148i encoder ===
Input:    ../images/example.ppm  (320 x 240)
Quality:  50
Chroma:   4:2:0
Huffman:  optimized for this image

Y plane:      320 x 240
Cb/Cr planes: 160 x 120

Encoded 1800 blocks
Huffman tables: 124 bytes stored in the file
Entropy data:   2145 bytes
Wrote ../output/image.n148i

=== N.148i decoder ===
Header:  v3, 320 x 240, quality 50, chroma 4:2:0, custom tables
Decoded 1800 blocks, consumed 2145 of 2145 bytes
Upsampling: bilinear
Wrote ../output/decoded.ppm

=== Results ===
Original PPM:   230415 bytes (225.0 KB)
N148i file:       2290 bytes (2.2 KB)
Compression:  100.6:1
PSNR:         34.70 dB
```

Both PPM files are 320 × 240 RGB24 images and can be opened by applications that support the Netpbm format.

---

## Quality and reconstruction

Quality can be overridden at compile time without editing the source:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -DQUALITY=90 \
  main.c header.c ppm.c tables.c dct.c huffman.c encoder.c decoder.c \
  -o n148i -lm
```

The other compile-time controls are `CHROMA_MODE` (`0`, `1`, or `2`), `OPTIMIZE` (`0` or `1`), and `SMOOTH_UPSAMPLING` (`0` or `1`). With quality 50, optimized tables, and bilinear upsampling, the official fixture reproduces the article's chroma matrix:

| Chroma | N.148i size | PSNR |
| ---: | ---: | ---: |
| 4:4:4 | 3,257 bytes | 36.71 dB |
| 4:2:2 | 2,656 bytes | 35.74 dB |
| 4:2:0 | 2,290 bytes | 34.70 dB |

At 4:2:0, disabling optimized Huffman tables grows the file to 3,048 bytes without changing reconstructed pixels. Disabling bilinear upsampling keeps the same 2,290-byte file but reduces PSNR to 33.79 dB.

---

## Corruption handling

The executable validates the file before reconstruction:

- an incorrect magic signature is rejected as a non-N.148i file;
- incomplete fixed-size header fields are rejected;
- unsupported versions, dimensions, chroma modes, or optimization flags are rejected;
- malformed, oversubscribed, duplicate-symbol, or truncated custom Huffman tables are rejected;
- a payload shorter than the header's `data_size` is reported as truncated;
- invalid or prematurely exhausted entropy data causes decoding to fail safely.

These checks prevent malformed input from being treated as valid image data.

---

## Validation

Compile with strict warnings and runtime sanitizers:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  main.c header.c ppm.c tables.c dct.c huffman.c encoder.c decoder.c \
  -o /tmp/n148i-sanitized -lm
```

Important invariants for the reference round trip:

- both directions process exactly 1,800 blocks;
- all 2,145 entropy bytes are consumed;
- the four custom tables occupy 124 bytes;
- the complete N.148i file occupies 2,290 bytes;
- `decoded.ppm` is a valid 320 × 240 P6 image;
- quality-50 reconstruction measures 34.70 dB PSNR;
- DCT followed by IDCT reproduces unquantized blocks within floating-point tolerance;
- partial edge blocks and odd image dimensions preserve the original dimensions.

---

## Current limitations

The refined learning round trip works, but several improvements remain:

- optimized Huffman tables can cost more than they save for very small images, so callers must choose the appropriate mode;
- quality evaluation currently reports PSNR but not SSIM or VMAF;
- N.148i is a custom format and is not intended to be opened by JPEG viewers.

---

## Learning along with the code

The repository is designed to be read together with the lessons published at [micilini.com/conteudos/codecs](https://micilini.com/conteudos/codecs). The articles explain the reasoning behind every stage, while the Git history records the implementation milestone by milestone.

If you are new to codecs or binary file handling in C, start with the series and follow the repository changes in order.

---

## Contributing

Issues and pull requests are welcome as the format develops. Keep changes focused, preserve the binary contract, and compile with strict warnings before submitting code.

---

## License

N.148 is open-source software released under the MIT License. See [`LICENSE`](LICENSE) for details.

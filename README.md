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

The current milestone completes the first N.148i round trip: a PPM image is encoded into a compact `.n148i` file, decoded back into RGB pixels, written as a new PPM image, and compared with the original using PSNR.

---

## Complete round trip

The executable now performs both directions of the codec:

```text
ENCODER                                  DECODER
P6 PPM input                             N.148i v2 header
    ↓                                         ↓
RGB → YCbCr                              Huffman decoding
    ↓                                         ↓
4:2:0 chroma subsampling                 Run-length expansion
    ↓                                         ↓
8 × 8 blocks + edge padding              Inverse zig-zag
    ↓                                         ↓
DCT → quantization                       Dequantization → IDCT
    ↓                                         ↓
Zig-zag → RLE → Huffman                  Block reconstruction
    ↓                                         ↓
N.148i header + bitstream                Chroma upsampling
                                              ↓
                                         YCbCr → RGB
                                              ↓
                                         Decoded P6 PPM
```

The decoder reverses the encoder in the exact opposite order. Plane ordering is part of the format contract: all Y blocks are stored first, followed by Cb and then Cr, with an independent DC predictor for each plane.

---

## Current decoder milestone

This milestone adds:

- canonical Huffman decoding using minimum and maximum codes by bit length;
- restoration of signed coefficient amplitudes;
- inverse run-length expansion, including EOB and ZRL symbols;
- inverse zig-zag ordering and coefficient dequantization;
- two-dimensional inverse DCT with the original level shift restored;
- block reconstruction while discarding padded pixels outside the image;
- nearest-neighbour chroma upsampling from 4:2:0 to full resolution;
- YCbCr-to-RGB conversion;
- P6 PPM output through `save_ppm()`;
- signature, version, dimension, chroma, payload-length, and bitstream checks;
- PSNR measurement between the source and reconstructed images.

The output is lossy by design. `decoded.ppm` has the same dimensions as the original, but its pixel values reflect quantization and chroma subsampling.

---

## Shared codec modules

Encoder and decoder correctness depends on both sides using identical tables and transforms. Shared definitions therefore live in dedicated modules:

- `tables.c` owns quantization tables, zig-zag order, Huffman definitions, and quality scaling;
- `dct.c` owns the cosine table, forward DCT, and inverse DCT;
- `encoder.c` contains only the forward entropy and block pipeline;
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

## N.148i version 2 format

The decoder reads the same 20-byte little-endian header introduced with the full encoder:

| Offset | Size | Field | Reference value | Description |
| ---: | ---: | --- | --- | --- |
| `0` | 5 bytes | Signature | `N148I` | Identifies the N.148i format |
| `5` | 1 byte | Version | `2` | Current file-format version |
| `6` | 4 bytes | Width | `320` | Original image width |
| `10` | 4 bytes | Height | `240` | Original image height |
| `14` | 1 byte | Quality | `50` | Quantization quality required for decoding |
| `15` | 1 byte | Chroma | `2` | Chroma mode: `2` means 4:2:0 |
| `16` | 4 bytes | Data size | `3027` | Number of compressed bytes after the header |

Reference header bytes at quality 50:

```text
4e 31 34 38 49 02 40 01 00 00 f0 00 00 00 32 02 d3 0b 00 00
```

The declared payload size lets the program reject truncated files before attempting to decode incomplete data.

---

## Repository structure

```text
n148/
├── src/
│   ├── main.c          # Runs the encode/decode cycle and reports PSNR
│   ├── header.h/.c     # N.148i v2 header serialization
│   ├── ppm.h/.c        # PPM I/O, color conversion, and channel planes
│   ├── tables.h/.c     # Shared quantization, zig-zag, and Huffman tables
│   ├── dct.h/.c        # Shared forward and inverse DCT
│   ├── encoder.h/.c    # Forward block and entropy pipeline
│   └── decoder.h/.c    # Reverse pipeline and RGB reconstruction
├── images/
│   └── example.ppm     # Official 320 × 240 Micilini fixture
├── output/
│   ├── image.n148i     # Compressed N.148i v2 file
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
  main.c header.c ppm.c tables.c dct.c encoder.c decoder.c \
  -o n148i -lm
./n148i
```

Expected output at quality 50:

```text
=== N.148i encoder ===
Input:   ../images/example.ppm  (320 x 240)
Quality: 50

Encoded 1800 blocks into 3027 bytes
Wrote ../output/image.n148i

=== N.148i decoder ===
Header:  version 2, 320 x 240, quality 50, chroma 4:2:0
Payload: 3027 bytes
Decoded 1800 blocks, consumed 3027 of 3027 bytes
Wrote ../output/decoded.ppm

=== Results ===
Original PPM:   230415 bytes (225.0 KB)
N148i file:       3047 bytes (3.0 KB)
Compression:  75.6:1
PSNR:         33.79 dB
```

Both PPM files are 320 × 240 RGB24 images and can be opened by applications that support the Netpbm format.

---

## Quality and reconstruction

Quality can be overridden at compile time without editing the source:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -DQUALITY=90 \
  main.c header.c ppm.c tables.c dct.c encoder.c decoder.c \
  -o n148i -lm
```

The official fixture reproduces the article's size and PSNR matrix:

| Quality | N.148i size | Compression | PSNR |
| ---: | ---: | ---: | ---: |
| 90 | 5,866 bytes | 39.3:1 | 36.17 dB |
| 50 | 3,047 bytes | 75.6:1 | 33.79 dB |
| 20 | 1,670 bytes | 138.0:1 | 31.45 dB |
| 10 | 1,465 bytes | 157.3:1 | 29.52 dB |

Higher quality preserves more frequency information and improves reconstruction at the cost of a larger payload. Lower quality increases quantization and compression loss.

---

## Corruption handling

The executable validates the file before reconstruction:

- an incorrect magic signature is rejected as a non-N.148i file;
- incomplete fixed-size header fields are rejected;
- unsupported versions, dimensions, or chroma modes are rejected;
- a payload shorter than the header's `data_size` is reported as truncated;
- invalid or prematurely exhausted entropy data causes decoding to fail safely.

These checks prevent malformed input from being treated as valid image data.

---

## Validation

Compile with strict warnings and runtime sanitizers:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  main.c header.c ppm.c tables.c dct.c encoder.c decoder.c \
  -o /tmp/n148i-sanitized -lm
```

Important invariants for the reference round trip:

- both directions process exactly 1,800 blocks;
- all 3,027 compressed bytes are consumed;
- the complete N.148i file occupies 3,047 bytes;
- `decoded.ppm` is a valid 320 × 240 P6 image;
- quality-50 reconstruction measures 33.79 dB PSNR;
- DCT followed by IDCT reproduces unquantized blocks within floating-point tolerance;
- partial edge blocks and odd image dimensions preserve the original dimensions.

---

## Current limitations

The complete learning round trip works, but several improvements remain:

- chroma upsampling uses nearest-neighbour replication rather than bilinear interpolation;
- Huffman tables are fixed rather than optimized per image;
- 4:4:4 and 4:2:2 are not exposed as user-selectable encoder modes;
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

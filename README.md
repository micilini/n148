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

The current milestone is the first complete **N.148i encoder**. It reads a real PPM image, converts it to YCbCr with 4:2:0 chroma subsampling, compresses every 8 × 8 block, and writes a versioned `.n148i` file containing a header and entropy-coded image payload.

---

## Current milestone: full-image encoding

The encoder now connects the stages introduced throughout the series into one end-to-end pipeline:

```text
P6 PPM input
    ↓
RGB → YCbCr
    ↓
4:2:0 chroma subsampling
    ↓
8 × 8 blocks with edge padding
    ↓
DCT → quantization → zig-zag
    ↓
DC differential coding + AC run-length coding
    ↓
JPEG-compatible Huffman tables
    ↓
N.148i v2 header + compressed bitstream
```

This milestone includes:

- binary P6 PPM loading with whitespace and comment handling;
- RGB-to-YCbCr conversion using JPEG/ITU-R BT.601 coefficients;
- 4:2:0 chroma subsampling by averaging each 2 × 2 pixel region;
- block traversal with edge replication for dimensions not divisible by eight;
- two-dimensional DCT with precomputed cosine tables;
- quality-scaled luminance and chrominance quantization tables;
- zig-zag coefficient ordering;
- differential DC prediction reset independently for Y, Cb, and Cr;
- AC run-length encoding with EOB and ZRL symbols;
- separate luminance and chrominance Huffman tables;
- a dynamically growing bit writer;
- N.148i format version 2 with the compressed payload length in its header.

---

## Reference image

The bundled [`images/example.ppm`](images/example.ppm) is the official 320 × 240 P6 fixture from the Micilini lesson. Its original source is available at [micilini.com/assets/img/example.ppm](https://micilini.com/assets/img/example.ppm).

| Property | Value |
| --- | ---: |
| Dimensions | 320 × 240 pixels |
| Pixel representation | RGB24, binary P6 |
| File size | 230,415 bytes |
| SHA-256 | `0bee9996ff9e43c9429a896cfba98dd183419426d28d31bbfcd65ef08ab2a404` |

Using the same fixture makes the encoder output directly comparable with the measurements in the article.

---

## N.148i version 2 format

The encoder writes a 20-byte little-endian header followed immediately by the compressed bitstream:

| Offset | Size | Field | Reference value | Description |
| ---: | ---: | --- | --- | --- |
| `0` | 5 bytes | Signature | `N148I` | Identifies the N.148i format |
| `5` | 1 byte | Version | `2` | Current file-format version |
| `6` | 4 bytes | Width | `320` | Original image width |
| `10` | 4 bytes | Height | `240` | Original image height |
| `14` | 1 byte | Quality | `50` | Quantization quality |
| `15` | 1 byte | Chroma | `2` | Chroma mode: `2` means 4:2:0 |
| `16` | 4 bytes | Data size | `3027` | Number of compressed bytes after the header |

For the bundled image at quality 50, the header bytes are:

```text
4e 31 34 38 49 02 40 01 00 00 f0 00 00 00 32 02 d3 0b 00 00
```

The full reference file is 3,047 bytes: 20 header bytes plus a 3,027-byte compressed payload.

---

## Planes, blocks, and padding

The 320 × 240 luminance plane remains at full resolution. With 4:2:0 subsampling, each chroma plane is reduced to 160 × 120.

| Plane | Dimensions | Horizontal blocks | Vertical blocks | Total blocks |
| --- | ---: | ---: | ---: | ---: |
| Y | 320 × 240 | 40 | 30 | 1,200 |
| Cb | 160 × 120 | 20 | 15 | 300 |
| Cr | 160 × 120 | 20 | 15 | 300 |
| **Total** |  |  |  | **1,800** |

When a future input dimension is not divisible by eight, `plane_sample()` clamps coordinates to the closest valid edge pixel. Partial blocks are therefore padded by repeating the image boundary rather than discarding pixels.

The planes are encoded sequentially in the bitstream: all Y blocks, then all Cb blocks, then all Cr blocks. Each plane starts with a fresh DC predictor.

---

## Repository structure

```text
n148/
├── src/
│   ├── main.c          # Coordinates loading, encoding, and file output
│   ├── header.h        # N.148i v2 header contract
│   ├── header.c        # Little-endian header serialization
│   ├── ppm.h           # RGB image and channel-plane types
│   ├── ppm.c           # PPM loading, YCbCr conversion, and 4:2:0
│   ├── encoder.h       # Public encoder API and statistics
│   └── encoder.c       # DCT, quantization, zig-zag, RLE, and Huffman
├── images/
│   └── example.ppm     # Official 320 × 240 Micilini fixture
├── output/
│   └── image.n148i     # Generated N.148i v2 reference file
├── .gitattributes      # Binary media and codec artifact rules
├── .gitignore
├── LICENSE
└── README.md
```

---

## Build and run

### Requirements

- A C compiler with C11 support, such as GCC or Clang
- The standard C math library
- A terminal or command prompt
- No third-party codec libraries

From the repository root:

```bash
cd src
gcc -std=c11 -Wall -Wextra -Wpedantic \
  main.c header.c ppm.c encoder.c -o n148i -lm
./n148i
```

The program intentionally uses paths relative to `src/`, matching the lesson commands.

Expected result with the bundled fixture and quality 50:

```text
=== N.148i encoder ===

Input:    ../images/example.ppm
Size:     320 x 240 pixels
Quality:  50

Y plane:      320 x 240
Cb/Cr planes: 160 x 120  (4:2:0)

Blocks encoded:  Y=1200  Cb=300  Cr=300  (total 1800)
Compressed data: 3027 bytes

Wrote ../output/image.n148i

PPM  original:    230415 bytes  (225.0 KB)
N148i file:         3047 bytes  (3.0 KB)
Header:               20 bytes
Compression:    75.6:1  (98.7% smaller)
```

Inspect the version 2 header on Linux or macOS:

```bash
od -A d -t x1 -N 20 ../output/image.n148i
```

---

## Quality settings

Quality controls the scaled quantization tables. The default is 50, but it can be overridden at compile time:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -DQUALITY=90 \
  main.c header.c ppm.c encoder.c -o n148i -lm
```

The official fixture reproduces the article's size matrix:

| Quality | Payload | Complete N.148i file | Compression ratio |
| ---: | ---: | ---: | ---: |
| 90 | 5,846 bytes | 5,866 bytes | 39.3:1 |
| 50 | 3,027 bytes | 3,047 bytes | 75.6:1 |
| 10 | 1,445 bytes | 1,465 bytes | 157.3:1 |

Higher quality preserves more coefficients and produces a larger payload. Lower quality quantizes more aggressively and produces a smaller file.

---

## Validation

Compile with strict warnings and runtime sanitizers:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  main.c header.c ppm.c encoder.c -o /tmp/n148i-sanitized -lm
```

Useful invariants for the bundled fixture:

- the input is a valid 320 × 240 raw P6 pixmap;
- the Y plane contains 1,200 blocks;
- Cb and Cr contain 300 blocks each;
- the v2 header occupies exactly 20 bytes;
- `data_size` equals the number of remaining bytes in the file;
- the quality-50 output occupies exactly 3,047 bytes.

---

## Current limitations

The encoder is complete for the current learning milestone, but N.148i is not finished yet:

- there is no decoder, so `.n148i` files cannot yet be reconstructed into images;
- the encoder uses the standard JPEG Huffman tables rather than generating optimized tables per image;
- `main.c` currently writes only 4:2:0, although the header reserves other chroma modes;
- the format is custom and is not intended to be opened by JPEG viewers.

The next milestone is the decoder: parsing the compressed payload, reversing Huffman and run-length coding, dequantizing coefficients, applying IDCT, and reconstructing RGB pixels.

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

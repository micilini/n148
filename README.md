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

The current milestone completes the optimization cycle with a persistent worker pool, fused Huffman decode actions, direct sparse and dense inverse reconstruction, a contiguous coefficient cache, vectorized chroma processing, and runtime-dispatched AVX2/FMA paths.

---

## Complete round trip

The executable now performs both directions of the codec:

```text
ENCODER                                  DECODER
P6 PPM input                             N.148i v3 header
    ↓                                         ↓
RGB → factored fixed-point YCbCr         Fused Huffman action lookup
    ↓                                         ↓
4:4:4 / 4:2:2 / 4:2:0 chroma            Run-length expansion
    ↓                                         ↓
8 × 8 blocks + vector loads              Inverse zig-zag
    ↓                                         ↓
AAN DCT → AVX2 quantization              Live-only dequantization → direct IDCT stores
    ↓                                         ↓
Zig-zag → RLE → optimized Huffman        Block reconstruction
    ↓                                         ↓
Header + tables + bitstream              Bilinear chroma upsampling
                                              ↓
                                         AVX2/FMA YCbCr → RGB
                                              ↓
                                         Decoded P6 PPM
```

The decoder reverses the encoder in the exact opposite order. Plane ordering is part of the format contract: all Y blocks are stored first, followed by Cb and then Cr, with an independent DC predictor for each plane.

---

## Parallel and final optimization milestone

This milestone adds:

- a persistent pthread worker pool that divides independent rows into contiguous slices, lets the calling thread process the first slice, and runs tiny jobs inline;
- runtime worker-count control, a 32-worker cap, a threadless build, and platform-aware CPU/core detection;
- parallel color conversion, chroma subsampling, DCT/quantization, private Huffman-frequency counting with a final reduction, and RGB reconstruction;
- intentionally sequential entropy writing/reading and DC prediction, whose ordering is part of the format;
- a fused Huffman action table that resolves short codes, amplitudes, signs, runs, EOB, and ZRL in one lookup, backed by a 32-bit refill and canonical fallback;
- live-only natural-order dequantization that avoids clearing and walking all 64 coefficients for DC-only blocks;
- packed DC block fills and direct AVX2 dense IDCT stores into complete interior blocks;
- direct sparse AVX2 IDCT reconstruction for blocks with one, two, or three surviving AC coefficients;
- AVX2 vertical blending and horizontal 9:3:3:1 chroma stretching, processing 16 samples per pass;
- an AVX2/FMA inverse color path selected by a new CPU capability level;
- fused 4:2:0 conversion and subsampling that processes two source rows while they are still cache-hot;
- batched AC Huffman writing with separate sparse and dense paths and a per-block capacity reservation;
- an AVX2 nonzero scan plus cached AC counts, masks, and coefficients in one contiguous allocation;
- direct AVX2 DCT dispatch after the CPU path has already been selected for a worker band;
- a proportional initial bitstream allocation and an explicit eight-AC dense/sparse threshold;
- factored RGB-to-YCbCr arithmetic that removes one 32-bit multiply per channel;
- an inline merge cutoff for images below 512 × 1024 pixels, where pool handover costs more than it saves;
- runtime CPU detection with CPUID and operating-system AVX state checks;
- portable scalar fallback on processors without AVX2;
- AVX2 AAN forward and inverse transforms that process eight columns together;
- fixed-point RGB/YCbCr conversion for eight pixels at a time, with byte shuffles, saturating packing, and scalar tails;
- contiguous AVX2 quantization followed by 16-bit zig-zag reordering, avoiding processor-dependent gather performance;
- vector loading of complete interior 8 × 8 blocks while partial edge blocks retain clamped scalar sampling;
- per-function AVX2 targeting, so the rest of the binary keeps its baseline ISA;
- a test override for comparing scalar and vector paths in the same binary.

The N.148i format, compressed bytes, reconstructed pixels, and measured PSNR remain unchanged across the validated scalar/AVX2 paths and worker counts. The FMA path preserves the non-FMA ordering for the sensitive green-channel rounding step and keeps the scalar tail for incomplete vector groups.

---

## Shared codec modules

Encoder and decoder correctness depends on both sides using identical tables and transforms. Shared definitions therefore live in dedicated modules:

- `tables.c` owns quantization tables, zig-zag order, Huffman definitions, and quality scaling;
- `huffman.c` builds, validates, serializes, and reads canonical Huffman specifications;
- `cpu.c` detects and reports the SIMD level supported by both the processor and operating system;
- `parallel.c` owns the persistent worker pool, runtime worker count, inline fallback, and threadless build;
- `dct.c` owns the reference, scalar AAN, AVX2 AAN, sparse, and direct-store inverse transforms;
- `ppm.c` owns scalar and AVX2 fixed-point color separation, including the fused parallel 4:2:0 path;
- `encoder.c` transforms block rows in parallel, caches masks/counts/coefficients contiguously, reduces private symbol counts, and keeps ordered DC/bitstream writing sequential;
- `decoder.c` owns fused entropy actions, live-only dequantization, direct IDCT stores, vectorized chroma upsampling, and parallel AVX2/FMA reconstruction.

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
│   ├── bench_cli.c     # Repeatable codec timing driver
│   ├── validate_cli.c  # CPU-path, worker-count, and odd-edge validator
│   ├── compare_cli.c   # Direct libjpeg-turbo comparison driver
│   ├── header.h/.c     # N.148i v3 header serialization
│   ├── ppm.h/.c        # PPM I/O and scalar/AVX2 RGB color separation
│   ├── tables.h/.c     # Shared quantization, zig-zag, and Huffman tables
│   ├── huffman.h/.c    # Optimized Huffman construction and table I/O
│   ├── cpu.h/.c        # Runtime SIMD capability detection
│   ├── parallel.h/.c   # Persistent pool and contiguous worker ranges
│   ├── dct.h/.c        # Scalar and AVX2 AAN forward and inverse DCT
│   ├── encoder.h/.c    # Parallel transform/reduction and entropy pipeline
│   └── decoder.h/.c    # Sparse IDCT and parallel AVX2/FMA reconstruction
├── tools/              # Corpus benchmarking and result-analysis helpers
├── images/
│   └── example.ppm     # Official 320 × 240 Micilini fixture
├── output/
│   ├── image.n148i     # Compressed N.148i v3 file
│   └── decoded.ppm     # Reconstructed P6 image
├── .gitattributes
├── .gitignore
├── Makefile
├── LICENSE
└── README.md
```

---

## Build and run

### Requirements

- A C compiler with C11 support, such as GCC or Clang
- The standard C math library
- POSIX threads on Unix-like systems for the default parallel build
- No third-party codec libraries for `n148i`, `bench`, or `validate`
- libjpeg development headers only for the optional `compare` target

From the repository root:

```bash
make
./n148i images/example.ppm 50 2
```

Expected output at quality 50:

```text
=== N.148i encoder ===
Input:    images/example.ppm  (320 x 240)
Quality:  50
Chroma:   4:2:0
Huffman:  optimized for this image

Y plane:      320 x 240
Cb/Cr planes: 160 x 120

Encoded 1800 blocks
Huffman tables: 124 bytes stored in the file
Entropy data:   2145 bytes
Wrote output/image.n148i

=== N.148i decoder ===
Header:  v3, 320 x 240, quality 50, chroma 4:2:0, custom tables
Decoded 1800 blocks, consumed 2145 of 2145 bytes
Upsampling: bilinear
Wrote output/decoded.ppm

=== Results ===
Original PPM:   230415 bytes (225.0 KB)
N148i file:       2290 bytes (2.2 KB)
Compression:  100.6:1
PSNR:         34.70 dB
```

Both PPM files are 320 × 240 RGB24 images and can be opened by applications that support the Netpbm format.

---

## Quality and reconstruction

The executable accepts input path, quality, and chroma mode without a rebuild:

```bash
./n148i images/example.ppm 90 2
```

The compile-time defaults remain `QUALITY`, `CHROMA_MODE` (`0`, `1`, or `2`), `OPTIMIZE` (`0` or `1`), and `SMOOTH_UPSAMPLING` (`0` or `1`). Benchmark and validation drivers select the worker count at runtime with `n148_set_thread_count`; `make NOTHREADS=1` produces the inline-only build. With quality 50, optimized tables, and bilinear upsampling, the official fixture reproduces the article's chroma matrix:

| Chroma | N.148i size | PSNR |
| ---: | ---: | ---: |
| 4:4:4 | 3,255 bytes | 36.71 dB |
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
- zero or unsupported payload metadata is rejected before allocation;
- a payload shorter than the header's declared `data_size` is rejected before entropy decoding.

These checks prevent malformed container metadata and truncated files from being treated as valid image data.

---

## Validation

Build and run the repository validator:

```bash
make validate
./validate

make clean
make NOTHREADS=1 validate
./validate
```

Important invariants for the reference round trip:

- both directions process exactly 1,800 blocks;
- all 2,145 entropy bytes are consumed;
- the four custom tables occupy 124 bytes;
- the complete N.148i file occupies 2,290 bytes;
- `decoded.ppm` is a valid 320 × 240 P6 image;
- quality-50 reconstruction measures 34.70 dB PSNR;
- normalized AAN coefficients match the separable DCT within floating-point tolerance;
- AAN DCT followed by AAN IDCT reproduces unquantized blocks within floating-point tolerance;
- scalar and AVX2 transforms, color conversion, quantization, upsampling, and sparse reconstruction produce byte-identical compressed and reconstructed output;
- one worker and multiple workers produce byte-identical compressed and reconstructed output;
- partial edge blocks and odd image dimensions preserve the original dimensions.

---

## Current limitations

The optimized learning round trip works, with these known boundaries:

- entropy writing, entropy reading, and each plane's DC predictor chain remain sequential by construction, limiting parallel speedup according to Amdahl's law;
- work is divided into equal slices, which can underuse performance cores when a processor mixes fast and efficiency cores;
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

# N.148

N.148 is a handcrafted image and video codec written in C and built from first principles. The project follows the complete compression pipeline—from pixels and color channels to DCT, quantization, zig-zag ordering, run-length encoding, Huffman coding, and a portable binary file format.

This repository grows alongside the [Micilini codec series](https://micilini.com/conteudos/codecs), where every stage is developed and explained step by step.

---

## About the project

N.148 is an educational exploration of how modern media codecs work internally. Instead of treating compression as a black box, it implements each building block in plain C so the data transformations, trade-offs, and binary representation remain visible.

The project is split into two planned formats:

| Format | Purpose | File extension |
| --- | --- | --- |
| **N.148i** | Static images such as photographs, illustrations, and screenshots | `.n148i` |
| **N.148v** | Video streams and moving images | To be defined |

The current N.148i milestone loads a real image into memory, reads its interleaved RGB pixels, converts them to YCbCr, and exports the three channels for visual inspection. This prepares the image data for the block-based compression pipeline that follows.

---

## Current milestone: PPM input and YCbCr channels

The program in `src/main.c` reads the bundled binary PPM image at `images/example.ppm`. It validates the P6 header, supports flexible whitespace and comment lines, loads all RGB24 pixel data, and applies the JPEG/ITU-R BT.601 conversion to produce separate Y, Cb, and Cr planes.

This milestone demonstrates:

- parsing a hybrid file format with a text header and binary pixel payload;
- validating the P6 magic number, dimensions, and maximum channel value;
- handling legal PPM whitespace and `#` header comments;
- loading interleaved 24-bit RGB pixels into dynamically allocated memory;
- converting RGB into luminance and chrominance channels;
- saving individual channels as binary PGM images;
- releasing all allocated memory after processing.

The generated channel files under `output/` are intentionally versionable reference artifacts. They make the color-space transformation visible and easy to verify.

---

## Why PPM?

Most familiar image formats are compressed. Loading JPEG, PNG, WebP, or HEIC directly would first require implementing or integrating a complete decoder. PPM provides a deliberately simple starting point: its P6 variant stores a short text header followed immediately by raw RGB bytes.

A binary PPM begins like this:

```text
P6
320 240
255
[R, G, B, R, G, B, ...]
```

| Header value | Meaning |
| --- | --- |
| `P6` | Binary Portable Pixmap signature |
| `320 240` | Image width and height |
| `255` | Maximum channel value; each RGB channel occupies one byte |

N.148 uses P6 rather than the text-based P3 variant because the pixel payload is smaller, faster to read, and already matches the RGB24 representation used by the codec.

---

## RGB to YCbCr

Each RGB pixel is transformed into one luminance channel and two color-difference channels:

```text
Y  =  0.299000R + 0.587000G + 0.114000B
Cb = -0.168736R - 0.331264G + 0.500000B + 128
Cr =  0.500000R - 0.418688G - 0.081312B + 128
```

- **Y** carries brightness and most of the visually important structure.
- **Cb** describes the blue color difference.
- **Cr** describes the red color difference.

Each output value is rounded and clamped to the `0–255` byte range. The resulting planes are written as P5 PGM files so standard image tools can display them as grayscale images.

---

## N.148i header foundation

The previous milestone established the portable 16-byte header that will prefix every `.n148i` file:

| Offset | Size | Field | Example | Description |
| ---: | ---: | --- | --- | --- |
| `0` | 5 bytes | Signature | `N148I` | Identifies a valid N.148i file |
| `5` | 1 byte | Version | `1` | File-format version |
| `6` | 4 bytes | Width | `640` | Image width in pixels, little-endian |
| `10` | 4 bytes | Height | `480` | Image height in pixels, little-endian |
| `14` | 1 byte | Quality | `50` | Compression-quality setting |
| `15` | 1 byte | Chroma | `2` | Chroma mode: `0` = 4:4:4, `1` = 4:2:2, `2` = 4:2:0 |

The reference `output/image.n148i` remains in the repository. Future milestones will append the compressed image payload after this header.

---

## Repository structure

```text
n148/
├── src/
│   ├── main.c              # PPM reader and RGB-to-YCbCr conversion
│   ├── header.c            # Reserved for extracted header logic
│   └── header.h            # Reserved for the public header API
├── images/
│   └── example.ppm         # 320 × 240 fictional binary P6 fixture
├── output/
│   ├── image.n148i         # 16-byte header from the previous milestone
│   ├── channel_y.pgm       # Generated luminance plane
│   ├── channel_cb.pgm      # Generated blue-difference plane
│   └── channel_cr.pgm      # Generated red-difference plane
├── .gitattributes          # Binary media and codec artifact rules
├── LICENSE
└── README.md
```

---

## Build and run

### Requirements

- A C compiler with C11 support, such as GCC or Clang
- A terminal or command prompt
- No third-party C libraries

From the repository root, enter the source directory, compile the program, and run it:

```bash
cd src
gcc -std=c11 -Wall -Wextra -Wpedantic main.c -o n148i
./n148i
```

On Windows with GCC, the executable may be named `n148i.exe`. From PowerShell, run it with `./n148i.exe`.

With the bundled example, the output begins as follows:

```text
=== PPM loaded successfully ===
Size:        320 x 240 pixels
Pixels:      76800
Pixel data:  230400 bytes (225.0 KB)

First 3 pixels (RGB):
  pixel 0: R=255 G=  0 B=  0
  pixel 1: R=255 G=  0 B=  0
  pixel 2: R=255 G=  0 B=  0

Same 3 pixels after RGB -> YCbCr:
  pixel 0: Y= 76 Cb= 85 Cr=255
  pixel 1: Y= 76 Cb= 85 Cr=255
  pixel 2: Y= 76 Cb= 85 Cr=255
```

The program then writes `channel_y.pgm`, `channel_cb.pgm`, and `channel_cr.pgm` under `output/`.

---

## Using your own image

Convert a JPEG, PNG, WebP, or another supported image to a binary PPM named `example.ppm`. With ImageMagick:

```bash
magick photo.jpg -resize 320x240 images/example.ppm
```

Older ImageMagick installations may expose the command as `convert`. FFmpeg is another option:

```bash
ffmpeg -i photo.png images/example.ppm
```

Confirm that the file starts with `P6`; a `P3` image stores pixels as text and is intentionally rejected by this reader.

---

## Validation

Compile with strict warnings:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror src/main.c -o /tmp/n148i
```

For memory and undefined-behavior checks on GCC or Clang:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  src/main.c -o /tmp/n148i-sanitized
```

Run the executable from `src/` because the lesson intentionally uses paths relative to that directory.

---

## Roadmap

N.148 is being developed incrementally. Upcoming N.148i stages will:

1. divide the Y, Cb, and Cr planes into 8 × 8 blocks, including edge handling;
2. apply DCT and quantization to every block;
3. serialize coefficients through zig-zag ordering, RLE, and Huffman coding;
4. append the compressed payload after the N.148i file header;
5. implement the reverse path in a decoder and reconstruct the image.

The longer-term goal is to carry the same first-principles approach into **N.148v**, the video-oriented member of the format family.

---

## Learning along with the code

The repository is designed to be read together with the lessons published at [micilini.com/conteudos/codecs](https://micilini.com/conteudos/codecs). The articles explain the reasoning behind every stage, while the Git history records the implementation as it evolves milestone by milestone.

If you are new to codecs or binary file handling in C, start with the series and follow the repository changes in order.

---

## Contributing

Issues and pull requests are welcome as the format develops. Keep changes focused, preserve the explicit binary layouts, and compile with warnings enabled before submitting code.

---

## License

N.148 is open-source software released under the MIT License. See [`LICENSE`](LICENSE) for details.

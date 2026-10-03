# N.148i file format specification

Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.

This document is the normative, consolidated description of every N.148i
bitstream accepted by the library. The format-version byte is part of the
file syntax and is independent of the library release number. Multibyte
integers are little-endian unless a section explicitly says otherwise.

The five-byte file signature is always the ASCII sequence `N148I`. Byte 5
selects one of the seven format parsers below. Decoders must reject unknown
versions, unknown normative feature bits, noncanonical encodings where a
canonical form is required, truncated input, and trailing payload data.

## Format 1

Format 1 is the frozen public V1 bitstream. Its fixed header is 21 bytes:

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 5 | signature | ASCII `N148I` |
| 5 | 1 | format version | `1` |
| 6 | 4 | width | RGB canvas width |
| 10 | 4 | height | RGB canvas height |
| 14 | 1 | quality | encoder quality, 1 through 100 |
| 15 | 1 | chroma | `0` 4:4:4, `1` 4:2:2, `2` 4:2:0 |
| 16 | 1 | optimized tables | zero for fixed tables, one for serialized tables |
| 17 | 4 | payload size | entropy payload bytes after any tables |

The image pipeline converts RGB to YCbCr, optionally subsamples chroma, visits
all 8x8 Y blocks followed by Cb and Cr blocks in raster order, applies the
normative integer AAN DCT and quality-scaled JPEG quantization tables, and
entropy-codes JPEG-style DC differences and AC run/category symbols. Each
plane has an independent DC predictor. Magnitude bits use the JPEG complement
representation for negative values and are written most-significant bit first.

When `optimized tables` is one, four canonical Huffman specifications precede
the payload in luma-DC, luma-AC, chroma-DC, chroma-AC order. Each specification
contains sixteen one-byte code counts for lengths 1 through 16 followed by the
symbols in canonical order. When it is zero, the decoder uses the compiled
baseline tables and no table bytes occur. The payload ends on a zero-padded
byte boundary and must consume exactly the declared payload size.

The compatibility sentinel is the repository's `images/example.ppm` encoded
at quality 50, 4:2:0, with optimized tables. Its stream is exactly 2,290 bytes;
that size and its byte hash are release gates.

## Format 2

The default feature mask is `0x000000c7`: rANS, context modeling, reconstructed-neighbor
prediction, encoder-side RDO, and the normative loop filter. Perceptual color, adaptive
quantization, and variable transforms remain decodable opt-in profiles.

### Compatibility contract

The five-byte ASCII magic remains `N148I`. Byte 5 selects the format parser:
version 1 keeps the published 21-byte header and Huffman payload unchanged,
while version 2 uses the extensible header below. A format 2 library must decode both
versions. The public `format_version` option selects format 2 or the frozen format 1 stream.

### Fixed format 2 header

All multibyte integers use little-endian byte order.

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 5 | magic | ASCII `N148I` |
| 5 | 1 | version | `2` |
| 6 | 2 | header size | Fixed header plus feature metadata |
| 8 | 4 | width | RGB canvas width |
| 12 | 4 | height | RGB canvas height |
| 16 | 4 | payload size | Bytes after the complete header |
| 20 | 4 | feature flags | Normative tools used by this stream |
| 24 | 1 | quality | Encoder quality from 1 through 100 |
| 25 | 1 | chroma mode | `0` 4:4:4, `1` 4:2:2, `2` 4:2:0 |
| 26 | 1 | effort | Encoder decision effort from 0 through 9 |
| 27 | 1 | reserved | Must be zero; readers reject other values |
| 28 | 4 | metadata size | Bytes between the fixed header and payload |

The fixed header is 32 bytes. `header size` must equal 32 plus `metadata size`,
and the complete file length must equal `header size` plus `payload size`.
Unknown flag bits are rejected because silently skipping a normative decoding
tool would produce incorrect pixels.

### Feature flags

| Bit | Name | Profile | Normative effect |
|---:|---|---:|---|
| 0 | `RANS` | rANS entropy | Coefficient symbols use normalized rANS models |
| 1 | `CONTEXT` | context modeling | Coefficient models are selected from decoded context |
| 2 | `INTRA` | reconstructed-neighbor prediction | Blocks transform reconstructed-neighbor prediction residuals |
| 3 | `PERCEPTUAL_COLOR` | perceptual color | Planes use the format 2 perceptual color transform |
| 4 | `ADAPTIVE_QUANT` | adaptive quantization | Region quantization multipliers are signaled |
| 5 | `VARIABLE_TRANSFORM` | variable transforms | Transform partition sizes are signaled |
| 6 | `RDO` | RDO | Encoder decisions used rate-distortion search; decoder syntax unchanged |
| 7 | `LOOP_FILTER` | causal loop filtering | Normative deblocking and deringing are enabled |

Bits 8 through 31 are reserved. Feature metadata is stored in ascending feature-bit order so a stream has one canonical representation. Each feature will extend
this document with its exact metadata and payload syntax before its flag is
enabled by default.

### rANS entropy syntax

rANS entropy deliberately changes only the representation of the format 1 quantized
coefficients. Color conversion, chroma sampling, 8x8 DCT, quantization, block
order, and coefficient values are identical to format 1. The `RANS` flag selects
the syntax in this section. Clearing it selects the format 2 Huffman fallback
described below, which exists so the entropy representation can be disabled in
isolation.

#### Entropy metadata

The metadata begins with this four-byte header:

| Offset | Bytes | Field | Value |
|---:|---:|---|---:|
| 0 | 1 | entropy metadata version | `1` |
| 1 | 1 | coder | `1` for rANS |
| 2 | 1 | model count | Number implied by the active flags |
| 3 | 1 | rANS scale bits | `12` |

When `CONTEXT` is clear, four models follow in this fixed order: luma DC, luma
AC, chroma DC, and chroma AC. Cb and Cr share the chroma models. Each histogram
is normalized to exactly 4096 slots, and every symbol observed by the encoder
retains a nonzero frequency. When `CONTEXT` is set, the coefficient model count
is 22 and the ordering comes from the next section. `INTRA` appends twelve
prediction-mode models, producing totals of 16 models without `CONTEXT` or 34
with it.

Each model uses the following compact representation:

1. One byte gives the number of used symbols, where zero represents 256.
2. One byte gives the first used symbol.
3. Remaining symbols are strictly increasing. Their deltas are packed as
   four-bit values: codes 0 through 14 represent deltas 1 through 15; code 15
   is followed by the absolute symbol in eight bits. Zero padding completes
   the final byte.
4. The frequency of every symbol except the last is an unsigned base-128
   variable-length integer, least-significant group first, in its shortest
   representation. The last frequency is implied by
   `4096 - sum(previous frequencies)`.

Invalid ordering, a nonminimal delta escape or variable-length integer, zero
frequencies, a sum other than 4096, nonzero padding, or trailing metadata
bytes makes the stream corrupt.

#### Coefficient tokens

Planes are visited in Y, Cb, Cr order and blocks in raster order. Within a
block, coefficients use the existing zig-zag order.

- A DC token is the bit length, from 0 through 16, of the difference from the
  previous block's DC value in the same plane.
- An AC token has the JPEG-compatible `(zero run << 4) | magnitude bits`
  form. `0x00` is end of block and `0xf0` skips sixteen zeros. AC magnitudes
  use 1 through 15 bits.
- Magnitude bits are stored separately, most-significant bit first, using the
  same complement representation as format 1 for negative values. Unused bits in
  the last byte are zero.

With `CONTEXT` clear, the model for every symbol follows solely from plane and
token kind. Model indexes are never signaled in either mode. The encoder
accumulates the complete token sequence and feeds it to rANS in reverse; the
decoder consumes tokens in normal plane and block order.

### Context model selection

The `CONTEXT` flag requires `RANS`; a stream that sets context without rANS is
invalid. Context changes only which probability table codes a token. Token
values, amplitude bits, coefficients, and the rANS entropy payload descriptor do
not change.

The decoder derives every model from symbols and coefficients it already has.
No model index is stored in the file. Plane class is zero for Y and one for
the shared Cb/Cr class.

#### DC models 0 through 5

The previous DC category starts at zero independently for each plane and is
updated after every DC token. It maps to bucket zero for category 0, bucket
one for categories 1 through 3, and bucket two for categories 4 through 16.
The model is:

```text
model = plane_class * 3 + previous_dc_category_bucket
```

#### AC models 6 through 21

The next coefficient position maps to one of four zig-zag bands:

| Band | Zig-zag positions |
|---:|---|
| 0 | 1 through 5 |
| 1 | 6 through 14 |
| 2 | 15 through 27 |
| 3 | 28 through 63 |

Activity is one when either the previous nonzero AC category in the current
block is at least 3, or the coefficient at the current zig-zag position has
absolute value at least 4 in the already decoded block immediately above or
to the left. Otherwise activity is zero. A `0xf0` zero-run token resets the
previous AC category to zero. Top and left image boundaries simply omit their
missing neighbor. The model is:

```text
model = 6 + ((plane_class * 4 + zigzag_band) * 2) + activity
```

Blocks are decoded in raster order, so both referenced neighbors are always
available. Looking at any coefficient in the current block that has not yet
been decoded, or at a block below or to the right, would make the stream
impossible to decode and is forbidden.

All 22 histograms use the rANS entropy compact representation. If an encoder emits
no token for a logical context, it stores the canonical one-symbol placeholder
with symbol zero and frequency 4096; because that model is never selected, it
costs metadata but no payload bits.

#### rANS/context entropy payload

The payload begins with a 16-byte descriptor:

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | payload version | `1` |
| 1 | 1 | plane count | `3` |
| 2 | 2 | reserved | zero |
| 4 | 4 | symbol count | Number of DC and AC tokens |
| 8 | 4 | rANS byte count | Includes the initial state |
| 12 | 4 | amplitude bit count | Exact number of raw magnitude bits |

The descriptor is followed by the rANS stream and then by
`ceil(amplitude bit count / 8)` amplitude bytes. The rANS stream starts with a
32-bit little-endian state. It uses a 12-bit probability scale, byte
renormalization, and a lower state bound of `1 << 23`. A valid decoder must
consume every declared symbol, rANS byte, and amplitude bit, and must finish
at the canonical lower state.

### Reconstructed-neighbor prediction

`INTRA` requires `RANS`, but it does not require `CONTEXT`, so either earlier
entropy configuration can be measured independently. Planes and 8x8 blocks
retain their existing order. For each block, the encoder chooses one of four
predictions, subtracts it from the source samples, and transforms and quantizes
that residual. The decoder reverses the transform and adds the same prediction.

Both sides build predictions exclusively from already reconstructed bytes.
With `PERCEPTUAL_COLOR` clear, a missing top or left sample has value 128. With
it set, the missing-edge values for intensity, red-green, and blue-yellow are
161, 94, and 115 respectively. References beyond the right or bottom edge of a
partial block clamp to the last stored sample in that row or column. The four
two-dimensional predictions are:

| Value | Mode | Sample at `(x, y)` |
|---:|---|---|
| 0 | DC | Rounded mean of the available eight top and/or eight left samples; the plane's missing-edge value when neither exists |
| 1 | Vertical | Top sample at `x`, or the plane's missing-edge value when the top edge is missing |
| 2 | Horizontal | Left sample at `y`, or the plane's missing-edge value when the left edge is missing |
| 3 | TrueMotion | `clamp(left[y] + top[x] - top_left, 0, 255)` |

For TrueMotion, unavailable top, left, or top-left inputs use the applicable
plane's missing-edge value. This makes it reduce naturally to vertical or
horizontal prediction on a single image edge. Blocks are reconstructed in
raster order before the next mode is chosen. The scalar AAN inverse transform
is normative inside this feedback loop; forward DCT dispatch may use SIMD
because its quantized output is required to match the scalar path exactly.

The reconstructed-neighbor prediction encoder uses integer 8x8 Hadamard SATD to choose a mode. That
decision rule is not normative: another encoder may choose any of the four
modes. Only the signaled mode and reconstruction arithmetic constrain a
decoder.

Prediction residual DC is coded as its signed absolute value rather than as a
difference from the preceding block's DC. Its token still carries the
magnitude category and uses the same raw amplitude representation. AC token
syntax is unchanged. With `INTRA` clear, DC remains differential exactly as in
the corresponding feature profiles.

#### Prediction mode models

Prediction modes are appended to the rANS symbol sequence after all Y, Cb,
and Cr coefficient tokens, in Y, Cb, Cr block order. They use twelve models:
six for luma followed by six shared by both chroma planes. If `C` is the
coefficient model count (4 or 22), the mode model is:

```text
model = C + plane_class * 6 + mode_context
```

Context zero is used when neither neighbor block exists. When exactly one
neighbor exists, or both neighbors chose the same mode, context `1 + mode` is
used. Context five is used when the top and left modes disagree. All context
inputs have already been decoded. Mode symbols are the values 0 through 3 and
use the same normalized histogram representation as coefficient symbols.

#### Reconstructed-neighbor entropy payload

With `INTRA` set, the payload uses this 20-byte descriptor instead of the
rANS entropy descriptor:

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | payload version | `2` |
| 1 | 1 | plane count | `3` |
| 2 | 1 | prediction mode coder | `1` for modes in the rANS stream |
| 3 | 1 | reserved | zero |
| 4 | 4 | symbol count | Coefficient tokens plus prediction mode symbols |
| 8 | 4 | rANS byte count | Includes the initial state |
| 12 | 4 | amplitude bit count | Coefficient magnitude bits only |
| 16 | 4 | prediction mode count | One mode for every Y, Cb, and Cr block |

The descriptor is followed by the single mixed-model rANS stream and then the
amplitude bytes. The mode count must equal the block count implied by the image
dimensions and chroma sampling. A decoder rejects a mismatched count, an
out-of-range mode, trailing data, or an unfinished rANS state.

### Perceptual opponent color space

`PERCEPTUAL_COLOR` requires `RANS` and may be combined with or without
`CONTEXT` and `INTRA`. It changes RGB-to-plane conversion, inverse conversion,
quantization tables, and the reconstructed-neighbor prediction missing-edge constants, but it adds no
metadata bytes. The header flag alone selects the normative path.

This transform is an original deterministic integer approximation. It is
inspired by human cone-opponent processing, but it is not JPEG XL's XYB and is
not bit-compatible with any external color space. Integer arithmetic avoids
platform-dependent stream changes through `libm`.

#### Forward transform

For each 8-bit sRGB component `s`, first compute a 16-bit gamma-expanded value:

```text
linear(s) = floor((s * s * 257 + 127) / 255)
```

The three linear values are mixed into cone-like responses. Matrix products
are rounded by adding 2048 before the 12-bit shift:

```text
             [1700  2160   236] [linear(R)]
cone = 1/4096 [ 900  2900   296] [linear(G)]
             [ 120   500  3476] [linear(B)]
```

Each cone value then passes through `response(v)`, a deterministic cube-root
lookup. For table index `i` from 0 through 4097, let
`x = min(i * 16, 65535)` and let the table entry be the nearest integer `r`
whose cube is nearest to `x * 65535 * 65535`. For an input with low four bits
`f`, linearly interpolate adjacent entries and round by adding 8 before
dividing by 16. The input 65535 maps explicitly to 65535.

Let the three nonlinear responses be `r0`, `r1`, and `r2`, and let signed
division round the magnitude to nearest before restoring the sign. The stored
8-bit planes are:

```text
p = floor((r0 + r1 + 1) / 2)
I = clamp_byte(floor((p + 128) / 257))
X = clamp_byte(94  + round_signed((r0 - r1) / 59))
B = clamp_byte(115 + round_signed((r2 - p) / 253))
```

`I`, `X`, and `B` replace Y, Cb, and Cr in the unchanged plane order. Chroma
mode applies to `X` and `B` using the existing 4:4:4, 4:2:2, or 4:2:0 sampling
rules. The current perceptual intensity quantization table is byte-for-byte
equal to the format 1 luma table, and the two opponent planes use a dedicated table
currently equal to the format 1 chroma table. Keeping separately named normative
tables permits a future format version to recalibrate them without changing
format 1 or earlier format 2 streams.

#### Inverse transform

The existing smooth chroma upsampler supplies `X` and `B` with four fractional
bits. For integer, non-subsampled samples this is equivalent to shifting each
stored byte left by four. Reconstruct the nonlinear responses as follows:

```text
p  = I * 257
dx = round_signed(((X16 - 94  * 16) * 59)  / 16)
db = round_signed(((B16 - 115 * 16) * 253) / 16)
r0 = p + round_signed(dx / 2)
r1 = p - round_signed(dx / 2)
r2 = p + db
```

Clamp response values to 0 through 65535 and map each through the nearest
inverse of the forward response lookup. Recover linear RGB with the Q20 matrix
below, rounding signed products by `1 << 20`, then clamp and choose the nearest
8-bit value in the forward `linear(s)` table:

```text
                    [ 4170622 -3103133   -18912] [cone0]
linear RGB = 1/2^20 [-1298702  2469386  -122107] [cone1]
                    [   42830  -248077  1253823] [cone2]
```

The 8-bit plane boundary makes this color conversion intentionally lossy even
before transform quantization. The exhaustive 16-level RGB cube in the unit
gate measures 46.242195944944 dB and a maximum component error of 14. This
conversion floor is part of the experiment and must not be mistaken for DCT
loss.

### Adaptive quantization

`ADAPTIVE_QUANT` requires both `RANS` and `INTRA`. It divides the full-resolution
canvas into fixed 16x16 regions and signals one quantization level per region.
The region grid has `ceil(width / 16)` columns and `ceil(height / 16)` rows;
partial regions at the right and bottom remain one ordinary map entry.

Four normative levels modify each already quality-scaled quantization-table
entry `q`. Integer division rounds to nearest and the result clamps to 1 through
255:

| Level | Multiplier | Quantized table entry |
|---:|---:|---|
| 0 | 0.90 | `clamp(round(q * 90 / 100), 1, 255)` |
| 1 | 0.97 | `clamp(round(q * 97 / 100), 1, 255)` |
| 2 | 1.08 | `clamp(round(q * 108 / 100), 1, 255)` |
| 3 | 1.18 | `clamp(round(q * 118 / 100), 1, 255)` |

The encoder's activity estimator and level decision are nonnormative. The
reference encoder measures horizontal and vertical second differences in the
source intensity plane. For three consecutive samples, one contribution is
`min(abs(current - 2 * previous + previous_previous), 32)`. It averages every
available horizontal and vertical contribution inside the region, then selects
levels 0, 1, 2, or 3 at thresholds 2, 8, and 18. Curvature leaves a linear
gradient near zero and prevents one strong edge from being mistaken for dense
texture as readily as first-order gradient magnitude did. Another encoder may
make a different choice; only the signaled map constrains reconstruction.

Every luma 8x8 block uses the level of the 16x16 region containing its top-left
sample. Chroma blocks first map their top-left sample back to full-resolution
canvas coordinates according to the stream's 4:4:4, 4:2:2, or 4:2:0 sampling,
then select the same region. Encoder predictor feedback and decoder
reconstruction must use the identical adjusted table.

#### Adaptive-map entropy syntax

One rANS model is appended after all coefficient models and, when present, the
twelve intra-mode models. This makes 17 models with global coefficient models
or 35 with context modeling contexts. Map entries use raster order. The predicted level
is the decoded entry to the left, or the entry above at the start of a row, or
level 1 for the first entry. The coded symbol is:

```text
delta = (level - predicted_level) modulo 4
```

The decoder adds the delta modulo four and stores the result before deriving
the next predictor. All four reconstructed values are valid. Encoding deltas
in the shared rANS stream lets large coherent regions cost substantially less
than a raw two-bit map without introducing a second entropy engine.

#### Adaptive-quantization payload descriptor

With `ADAPTIVE_QUANT` set, payload version is 3 and the descriptor grows to 24
bytes:

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | payload version | `3` |
| 1 | 1 | plane count | `3` |
| 2 | 1 | prediction mode coder | `1` for modes in the rANS stream |
| 3 | 1 | adaptive-map coder | `1` for predicted levels in the rANS stream |
| 4 | 4 | symbol count | Coefficients, prediction modes, and map entries |
| 8 | 4 | rANS byte count | Includes the initial state |
| 12 | 4 | amplitude bit count | Coefficient magnitude bits only |
| 16 | 4 | prediction mode count | One mode for every plane block |
| 20 | 4 | adaptive-map count | Must equal the region count implied by dimensions |

The rANS symbol order is all coefficient tokens, all prediction modes, then
all adaptive-map deltas. The amplitude bytes still follow the rANS stream.
A decoder rejects version 3 without the flag, another map coder, a mismatched
map count, an unfinished map, trailing payload bytes, or a noncanonical rANS
final state.

### Variable transforms

`VARIABLE_TRANSFORM`
requires both `RANS` and `INTRA`. Each stored plane is divided into 16x16
macroblocks in that plane's own sample coordinates. One strategy value is
signaled per macroblock:

| Value | Strategy | Transform layout |
|---:|---|---|
| 0 | `DCT4` | Four independent 4x4 transforms inside each present 8x8 cell |
| 1 | `DCT8` | One existing AAN 8x8 transform per present cell |
| 2 | `DCT16` | One 16x16 transform over a complete 2x2 cell footprint |

`DCT16` is invalid when the macroblock does not own all four underlying 8x8
coefficient cells. Source samples at a partial right or bottom image edge are
still clamped normally. `DCT4` and `DCT8` cover every present cell, including
one-cell edge macroblocks.

The physical coefficient allocation remains 64 signed values per underlying
8x8 cell. This preserves the rANS entropy and 2 entropy machinery without making
the transform choice implicit:

- `DCT8` retains the existing coefficient matrix and 8x8 zig-zag scan.
- `DCT4` concatenates four 16-value zig-zag scans in top-left, top-right,
  bottom-left, bottom-right order. That 64-value sequence is placed in the
  slots visited by the existing 8x8 zig-zag scan.
- `DCT16` forms one 256-value 16x16 zig-zag sequence. Values 0 through 63,
  64 through 127, 128 through 191, and 192 through 255 are placed in the
  top-left, top-right, bottom-left, and bottom-right coefficient cells,
  respectively, again in each cell's 8x8 zig-zag slot order.

The four chunks of a `DCT16` block remain independent entropy blocks. This is
not optimal modeling, but it keeps earlier rANS streams unchanged and makes
the isolated transform experiment auditable. A later format revision may add
strategy-specific coefficient contexts if measurement justifies the added
syntax.

Prediction still signals one mode per underlying 8x8 cell. `DCT4` uses one
8x8 prediction followed by four transforms. `DCT8` is unchanged. `DCT16`
uses one 16x16 prediction and repeats its mode in all four owned mode slots;
a decoder rejects four values that differ. Macroblocks are reconstructed in
raster order so encoder and decoder have the same causal top and left samples.
Adaptive quantization, when separately enabled, maps each transform origin
back to the adaptive quantization canvas map before selecting a table.

#### Generic integer DCT

The 4x4 and 16x16 paths use a separable orthonormal DCT-II matrix represented
in signed Q14 integers:

```text
T[k,x] = round(16384 * alpha(k) *
               cos(pi * (2*x + 1) * k / (2*N)))
alpha(0) = sqrt(1/N)
alpha(k) = sqrt(2/N), k > 0
```

The encoder and decoder use the literal integer tables in
`src/variable_transform.c`; they never evaluate this formula while processing
a file. A two-dimensional forward or inverse matrix product accumulates in a
signed 64-bit integer and rounds the Q28 result to nearest, with halves away
from zero. The inverse uses the transpose. variable transforms validation records a hash
of both literal tables so accidental coefficient changes cannot silently alter
the format.

For transform size `N`, a frequency coordinate `i` selects the existing 8x8
base-table coordinate `round(i * 7 / (N - 1))`. Quality scaling is applied
first, then an optional adaptive quantization multiplier. DCT4 and DCT16 use these sampled
values directly with their orthonormal coefficients; DCT8 retains the existing
AAN scale factors and therefore remains byte-identical when selected alone.

#### Transform-map entropy syntax

Maps are concatenated in Y, Cb, Cr order, each in macroblock raster order.
The predicted strategy is the decoded entry to the left, the entry above at
the start of a row, or `DCT8` for the first entry in a plane. One additional
rANS model codes `(strategy - prediction) modulo 3`. Transform symbols follow
coefficient tokens, prediction modes, and an optional adaptive quantization map in the mixed
rANS symbol sequence.

With `VARIABLE_TRANSFORM` set, payload version is 4. The descriptor contains
the reconstructed-neighbor prediction prediction-mode count, the adaptive quantization adaptive-map count only when
that flag is set, then a four-byte transform-map count. The transform count
must equal the sum of the three plane macroblock grids implied by dimensions
and chroma sampling. A decoder rejects another payload version, an impossible
`DCT16` edge entry, a mismatched count, differing repeated 16x16 modes, or an
unfinished map.

Byte 3 is a map-coder bit field in payload version 4: bit 0 is one when the
adaptive quantization adaptive map is present and bit 1 is one for the transform map. Both
maps use the shared rANS stream. Without `ADAPTIVE_QUANT`, the descriptor is:

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | payload version | `4` |
| 1 | 1 | plane count | `3` |
| 2 | 1 | prediction mode coder | `1` |
| 3 | 1 | map coders | `2`, transform map only |
| 4 | 4 | symbol count | Coefficients, modes, and transform entries |
| 8 | 4 | rANS byte count | Includes the initial state |
| 12 | 4 | amplitude bit count | Coefficient magnitude bits only |
| 16 | 4 | prediction mode count | One per underlying 8x8 cell |
| 20 | 4 | transform-map count | Sum of the three plane macroblock grids |

When both adaptive quantization and variable transforms are set, byte 3 is `3`, the adaptive-map count
occupies offset 20, the transform-map count occupies offset 24, and the
descriptor is 28 bytes. The transform model follows the optional adaptive
model, giving 17 or 35 models without adaptive quantization and 18 or 36 with
it, depending on whether context modeling contexts are disabled or enabled.

#### Encoder policy and experimental status

Transform selection is not normative. The retained variable transforms encoder measures
clipped second-difference activity in each macroblock. It permits `DCT16`
only for a complete, exactly flat luma macroblock, permits `DCT4` only when
the average reaches 31 on the zero-to-32 clipped activity scale, and otherwise
uses the established `DCT8` path. This deliberately conservative policy was
selected after the broad variance heuristic failed measurement.

On the fixed ten-image gate, the retained policy changed BD-rate relative to
reconstructed-neighbor prediction by **+0.0108%** for RGB PSNR, **+0.0104%** for luma PSNR, **+0.0218%**
for SSIM, **+0.0250%** for MS-SSIM, **+0.0243%** for SSIMULACRA2, and
**+0.0212%** for Butteraugli. Positive means a regression. Encode and decode
time rose to **1.2114x** and **1.2647x** reconstructed-neighbor prediction. The first broad selector,
which used 16x16 below activity 3 and 4x4 above activity 14, regressed every
curve by 11.81% through 20.11% and cost 5.54x encode and 9.44x decode.

Therefore variable transforms is implemented, decodable, and exhaustively validated but
remains outside the default feature mask. The negative result is retained
because it shows that transform size cannot be selected independently of the
very effective reconstructed-neighbor predictor. Raw evidence is in
`https://github.com/micilini/n148/blob/499a34eb06bd0ef990e05c0d2fdcf84c7b80cc35/benchmarks/format 2-progress/variable transforms.json`.

### Rate-distortion optimization

`RDO` requires both `RANS` and `INTRA`. It is an encoder-decision flag: it
does not append metadata, change payload version, or add a decoder operation.
The selected prediction modes and coefficients already travel in the reconstructed-neighbor prediction
syntax, so a decoder validates the flag and then follows that existing syntax.
The fixed-header effort byte records how extensively the encoder searched.

The effort bands are deliberately coarse:

| Effort | Encoder search |
|---:|---|
| 0 through 2 | reconstructed-neighbor prediction SATD mode choice and nearest coefficient rounding |
| 3 through 7 | reconstructed-neighbor prediction SATD mode followed by one AC trellis |
| 8 through 9 | Joint evaluation of all four modes, with one trellis per mode |

This makes the fast setting byte-compatible with reconstructed-neighbor prediction after the fixed
header. Effort 5 is the format 2 default. Effort 9 spends more encoder time and can
select a different quality/rate trade-off, while decoding work is unchanged.

#### Distortion and rate proxy

For an 8x8 candidate, the mode search reconstructs the actual integer pixels
and measures squared error against the source. The coefficient trellis uses a
separable distortion proxy. For every zig-zag position, the encoder sends a
unit quantized coefficient through the normative scalar inverse AAN transform
and records the squared energy of that spatial basis. If `x` is the
unrounded quantized level, `q` is a candidate integer, and `E` is that basis
energy, its proxy distortion is `(x - q)^2 * E`.

The rate estimate follows the structure of the existing rANS token stream. It
counts amplitude bits exactly, charges 2.25 fractional bits for a nonzero AC
symbol, 1.25 for each zero-run-length symbol, 0.08 per residual run position,
and 0.75 for end-of-block. These are decision weights, not claims about the
final byte count; the real per-image rANS histograms are still built from the
chosen complete token sequence.

The base Lagrange multiplier is `0.85 * E[DC]`, tying the price of a bit to
the active quantization table. Mode signaling uses one quarter of that value.
The retained trellis uses 4% after measurement showed that a stronger value
removed visible high-frequency detail. A dynamic program minimizes
`distortion + lambda * estimated_bits` over the complete AC scan, including
zero runs and EOB. It can retain nearest rounding, move a nonzero coefficient
one step toward zero, or make it zero. Efforts 8 and 9 may also move one step
away from zero or introduce a coefficient whose unrounded magnitude is at
least 0.35. The original rounded path is always a candidate, and validation
rejects a result whose proxy cost is higher.

DC is deliberately fixed. Changing it affects reconstructed-neighbor
prediction in later blocks, so its real cost is not separable. The opt-in 4x4
and 16x16 variable transforms paths likewise retain their measured quantization; when
`VARIABLE_TRANSFORM` and `RDO` are combined, RDO applies to DCT8 cells only.
The failed variable transforms selector showed that a trustworthy transform-size search
needs a larger joint macroblock model, so this implementation does not label
a variance decision as rate-distortion optimization.

### Causal loop filter

`LOOP_FILTER` requires both `RANS` and `INTRA`. It adds no entropy model,
metadata, or payload field: the fixed-header feature bit selects a normative
reconstruction process over the reconstructed-neighbor prediction syntax. The quality byte and, when
present, adaptive quantization's adaptive level determine the strength, so no independent
filter parameter needs signaling.

Filtering happens immediately after each coding square is reconstructed, in
the same raster order used by prediction. A normal DCT8 cell and a cell split
into four DCT4 regions are filtered as one 8x8 square. An experimental DCT16
macroblock is filtered as one 16x16 square, without inventing an internal 8x8
boundary. Partial right and bottom squares use their visible dimensions.

The exact operation order is:

1. Limit isolated ringing inside the newly reconstructed square.
2. Filter its already available left boundary, if one exists.
3. Filter its already available top boundary, if one exists.

The boundary passes may update samples on both sides. Those samples have
already served any earlier block but become the references seen by all later
blocks. Encoder and decoder therefore must perform these steps at the same
time, with the same integer rounding. Applying the filter only to the final
image is not equivalent and produces prediction drift.

#### Normative strength

For quality `Q` below 96, the initial luma strength is
`ceil((100 - Q) / 20)`. Quality 96 through 100 selects zero. Adaptive level 0
subtracts one when the strength exceeds one, level 1 and 2 leave it unchanged,
and level 3 adds one. Chroma strengths above one are multiplied by three
quarters with nearest-integer rounding. The final strength is capped at five.

The edge threshold begins at `3 + ceil((100 - Q) / 4)`. It is multiplied by
the active adaptive-level percentage `{90, 97, 108, 118}`, rounded to nearest,
and clamped to at least three. Without adaptive quantization, level 1 is used.

#### Deringing pass

The pass takes an immutable snapshot of the visible square and visits only
its interior samples. For each center it compares the horizontal and vertical
opposing pairs, chooses the pair with the smaller span, and skips pairs whose
span exceeds the edge threshold. If the center lies outside the pair's range
expanded by the filter strength, it moves toward the weighted value
`(first + 2 * center + second + 2) / 4`, by no more than the strength. This
limits isolated overshoot without averaging across the sharper direction.

#### Deblocking passes

Each boundary sample uses `p1, p0 | q0, q1`, where the bar is the transform
boundary. It is eligible only when `abs(q0 - p0)` is from 2 through the edge
threshold and the two outer gradients sum to no more than threshold plus
strength. The primary adjustment is the signed, nearest-rounded value
`[3 * (q0 - p0) + p1 - q1] / 8`, clamped to plus or minus the strength;
`p0` gains that value and `q0` loses it. At strength three or above, a very
flat neighborhood also applies half that adjustment to `p1` and `q1`.
Vertical boundaries are processed before horizontal ones.

### Huffman control stream

When `RANS` is clear, metadata version remains 1 and coder is 0. The next two
bytes contain table count 4 and reserved zero, followed by the four format 1
canonical Huffman specifications. Each specification stores sixteen code
length counts followed by its symbols. The payload is the unchanged format 1
entropy payload. This stream is a controlled experiment rather than the format 2
default.

## Format 3

The default feature mask is `0x000007c7`, adding directional/sub-block intra prediction
and contextual RDO. The perceptual coefficient trellis remains a decodable opt-in
profile.

This document is the normative description of the additions made by format
version 3. Unchanged entropy, amplitude, loop-filter, color, and container
rules are inherited from [format 2](#format-2).

### Compatibility contract

The first five bytes remain the ASCII magic `N148I`. Byte 5 selects a fully
separate parser:

- `1` selects the published Huffman format 1 stream;
- `2` selects the format 2 stream;
- `3` selects the syntax in this document.

A conforming decoder accepts formats 1 through 3. The public `format_version`
option can request any of them. A decoder must reject an unknown version or
unknown normative feature bit; it must never guess a layout.

### Fixed header

format 3 reuses the length-delimited 32-byte format 2 container header. All multibyte
integers are unsigned little-endian values.

| Offset | Bytes | Field | format 3 meaning |
|---:|---:|---|---|
| 0 | 5 | magic | ASCII `N148I` |
| 5 | 1 | version | `3` |
| 6 | 2 | header size | 32 plus entropy metadata bytes |
| 8 | 4 | width | RGB canvas width |
| 12 | 4 | height | RGB canvas height |
| 16 | 4 | payload size | Bytes after the complete header |
| 20 | 4 | feature flags | Normative tools used by this stream |
| 24 | 1 | quality | 1 through 100 |
| 25 | 1 | chroma mode | `0` 4:4:4, `1` 4:2:2, `2` 4:2:0 |
| 26 | 1 | effort | Encoder decision effort, 0 through 9 |
| 27 | 1 | reserved | Must be zero |
| 28 | 4 | metadata size | Bytes between the fixed header and payload |

`header size` must equal `32 + metadata size`. The file must end exactly after
`payload size`; truncation and trailing bytes are errors.

### Feature flags and accepted profiles

format 3 inherits the accepted format 2 entropy, context, intra, RDO, and loop-filter bits
and adds four flags.

| Bit | Name | Effect in format 3 |
|---:|---|---|
| 0 | `RANS` | Required byte-rANS coefficient coding |
| 1 | `CONTEXT` | Decoded-neighbor coefficient contexts |
| 2 | `INTRA` | Required reconstructed-neighbor prediction |
| 6 | `RDO` | Encoder decisions use rate-distortion search |
| 7 | `LOOP_FILTER` | The normative causal format 2 loop filter is active |
| 8 | `DIRECTIONAL_INTRA` | Six directional modes extend the original four |
| 9 | `INTRA_4X4` | Luma cells may contain four 4x4 transforms |
| 10 | `CONTEXTUAL_RDO` | Two-pass decisions use current-frame symbol costs |
| 11 | `PERCEPTUAL_TRELLIS` | Experimental contextual coefficient trellis |

Bits 3 through 5 are format 2 experiments and are invalid in a format 3 stream. Bits 12
through 31 are reserved. `RANS` and `INTRA` are mandatory. Bit 10 requires
bits 1 and 6, while bit 11 requires bit 10.

The measured profiles are canonical masks:

| Profile | Mask | Status |
|---|---:|---|
| directional intra prediction, complete prediction | `0x03c7` | Measured prerequisite, rejected alone |
| contextual RDO, contextual RDO | `0x07c7` | Accepted format 3 default |
| perceptual trellis, coefficient trellis | `0x0fc7` | Experimental, off by default |

### Entropy metadata

Metadata retains the format 2 compact model representation. Its first four bytes
are `1` (metadata version), `1` (rANS coder), model count, and `12` (rANS
scale bits). Every normalized model totals 4096 slots and retains every
observed symbol with nonzero frequency.

Models occur in this order:

1. four global coefficient models, or 22 contextual coefficient models when
   `CONTEXT` is set;
2. 24 prediction-mode models, twelve for luma followed by twelve shared by Cb
   and Cr;
3. one partition model when `INTRA_4X4` is set.

The default profile therefore carries 47 models. Empty contexts use a
one-symbol zero placeholder, which preserves a fixed model index layout
without consuming rANS bits.

### Payload descriptor

The format 3 payload begins with the following descriptor. The internal payload
revision is deliberately distinct from the outer file-format version.

| Offset | Bytes | Field | Value or meaning |
|---:|---:|---|---|
| 0 | 1 | payload revision | `5` |
| 1 | 1 | plane count | `3` |
| 2 | 1 | mode coder | `1`, modes are rANS symbols |
| 3 | 1 | map coder flags | `4` when the partition map is present, else `0` |
| 4 | 4 | symbol count | Total symbols in the mixed rANS stream |
| 8 | 4 | rANS byte count | Length of the following rANS stream |
| 12 | 4 | amplitude bit count | Number of valid raw magnitude bits |
| 16 | 4 | mode-slot count | Four slots for every 8x8 cell in Y, Cb, and Cr |
| 20 | 4 | partition count | Present only with `INTRA_4X4` |

The rANS byte stream follows the descriptor, then
`ceil(amplitude bit count / 8)` magnitude bytes. Magnitudes retain the format 2
most-significant-bit-first complement representation. Unused low bits in the
last byte must be zero.

RANS symbols are logically consumed in this order:

1. the complete partition map, when present;
2. coefficient units for Y, Cb, and Cr;
3. prediction modes for Y, Cb, and Cr.

The encoder accumulates that logical sequence and emits rANS in reverse. The
decoder must consume exactly `symbol count`, all magnitude bits, the entire
rANS state, and the complete payload.

### Partition map

Each 8x8 coefficient cell has one binary partition value. Zero means one 8x8
transform; one means four independent 4x4 transforms in top-left, top-right,
bottom-left, bottom-right order. Only luma may currently be split. Chroma map
entries are nevertheless present and must be zero, which keeps plane geometry
and future extensions unambiguous.

The map is traversed in Y, Cb, Cr order and raster order within each plane.
Its causal predictor is the left value when available, otherwise the top value
when available, otherwise zero. The coded symbol is `actual XOR prediction`.

Partial edge cells use the same syntax. Source and reconstructed samples beyond
the image boundary are clamped for the transform, but only in-bounds pixels are
written to the output plane.

### Prediction modes

The numeric mode values are normative:

| Value | Mode |
|---:|---|
| 0 | DC |
| 1 | vertical |
| 2 | horizontal |
| 3 | TrueMotion |
| 4 | down-left |
| 5 | down-right |
| 6 | vertical-right |
| 7 | horizontal-down |
| 8 | vertical-left |
| 9 | horizontal-up |

Prediction always uses already reconstructed samples above and to the left,
never original or future pixels. A missing edge has value 128. Extended edge
references are clamped to the causal cell boundary, and every predicted sample
is clipped to 0 through 255. DC averages available edges. TrueMotion computes
`left + top - top_left`. Directional modes use integer two-tap and three-tap
interpolation; the executable reference equations are in `src/directional_intra.c`.

Every 8x8 cell reserves four mode slots arranged as a virtual 2x2 grid. A
split luma cell signals all four modes. An unsplit cell signals one mode and
the decoder copies it into the other three slots. Cb and Cr make independent
mode decisions instead of inheriting luma's choice.

Mode probability context is derived only from decoded virtual neighbors. The
first virtual block uses context 0. If one neighbor exists, or top and left
agree, context is `1 + neighbor_mode`. If both exist and disagree, context 11
is used. Luma and chroma use separate sets of twelve models.

### Coefficient units and transforms

An unsplit cell contains one 64-coefficient 8x8 unit in the existing zig-zag
order. A split cell packs four independent 16-coefficient 4x4 units into the
same 64 coefficient slots. Each unit has its own absolute DC category, AC
run/category symbols, and end-of-block symbol. Treating the four DC values as
one transform is invalid.

The 8x8 path keeps the format 1 AAN DCT and scaled JPEG-derived quantization tables.
The 4x4 path uses the project's deterministic integer Q14 transform and a
4x4 table sampled from the corresponding 8x8 luma table. Reconstruction adds
the inverse-transform residual to the same causal prediction and clips to one
byte. When `LOOP_FILTER` is set, the inherited format 2 causal filter runs after the
cell is reconstructed, identically in encoder and decoder.

Coefficient probability contexts retain format 2's luma/chroma class, previous DC
category bucket, four local zig-zag bands, and decoded-neighbor activity.
Positions inside each packed 4x4 unit restart at zero for band selection.

### Encoder-only decisions

The following rules explain the reference encoder but do not add decoder
operations.

At effort 5 or higher, `CONTEXTUAL_RDO` first makes a causal pass to collect
current-frame symbol frequencies. A second pass reconstructs every candidate
mode and partition and minimizes:

```text
reconstructed plane-sample SSE + 0.85 * dc_step^2 * estimated_bits
```

`estimated_bits` includes coefficient events, raw magnitude bits, prediction
mode, and partition signaling under the collected contexts. Lower efforts use
the SATD shortcut and test fewer alternatives.

`PERCEPTUAL_TRELLIS` applies dynamic programming over AC run/category states.
Its distortion weight is transform-basis energy plus one quarter of spatial
gradient energy, and it prices the same contextual symbols as the stream. The
flag remains experimental because the measured perceptual trellis profile regressed
aggregate Butteraugli by 0.95% against contextual RDO and made encoding 2.18 times
slower than contextual RDO.

### Decoder rejection requirements

A conforming decoder rejects at least: invalid flag dependencies, an invalid
model count or normalized total, a partition symbol outside 0/1, a nonzero
chroma split, a mode outside the enabled set, an impossible coefficient run,
truncated magnitude data, nonzero amplitude padding, an unfinished rANS state,
unused or missing symbols, and trailing descriptor bytes.

## Format 4

The default feature mask is `0x000057c7`, adding reconstruction-aware chroma preparation
and calibrated chroma quantization. Joint chroma intra, segmentation, and the segment-
adaptive filter are opt-in.

This document describes additions made by file-format version 4. Container,
entropy, prediction, transform, amplitude, and loop-filter rules not changed
here are inherited from [format 3](#format-3).

### Compatibility contract

The first five bytes remain ASCII `N148I`. Byte 5 selects the parser:

- `1` selects the published Huffman format 1 stream;
- `2` selects the format 2 stream;
- `3` selects the directional/RDO format 3 stream;
- `4` selects the colour-path syntax in this document.

A conforming decoder accepts formats 1 through 4 and never guesses an unknown
layout. Callers select format 4 and its feature mask explicitly when producing
this stream revision.

### Fixed header

format 4 retains the length-delimited 32-byte container introduced by format 2. Integer
fields use unsigned little-endian representation.

| Offset | Bytes | Field | format 4 meaning |
|---:|---:|---|---|
| 0 | 5 | magic | ASCII `N148I` |
| 5 | 1 | version | `4` |
| 6 | 2 | header size | 32 plus entropy metadata bytes |
| 8 | 4 | width | RGB canvas width |
| 12 | 4 | height | RGB canvas height |
| 16 | 4 | payload size | Bytes after the complete header |
| 20 | 4 | feature flags | Tools used to create or decode this stream |
| 24 | 1 | quality | 1 through 100 |
| 25 | 1 | chroma mode | `0` 4:4:4, `1` 4:2:2, `2` 4:2:0 |
| 26 | 1 | effort | Encoder decision effort, 0 through 9 |
| 27 | 1 | chroma quality | 1 through 100 when bit 14 is set; zero otherwise |
| 28 | 4 | metadata size | Bytes between fixed header and payload |

`header size` must equal `32 + metadata size`, and the file must end exactly
after `payload size` bytes.

### Feature flags and profiles

Bits 0 through 11 retain their format 3 definitions and dependencies. format 4 adds:

| Bit | Public name | Effect |
|---:|---|---|
| 12 | `N148I_FEATURE_RECONSTRUCTION_AWARE_CHROMA` | Encoder prepared a subsampled chroma plane against the normative reconstruction kernel |
| 13 | `N148I_FEATURE_JOINT_CHROMA_INTRA` | One prediction-mode grid is jointly selected and signaled for Cb and Cr |
| 14 | `N148I_FEATURE_CALIBRATED_CHROMA_QUANT` | Use the format 4 chroma-residual table and the quality stored in header byte 27 |
| 15 | `N148I_FEATURE_SEGMENTATION` | Decode a four-class 16x16 map and its signaled luma quantizer scales |
| 16 | `N148I_FEATURE_ADAPTIVE_LOOP_FILTER` | Select the segment-dependent normative luma loop-filter class |

Bit 12 is valid with chroma modes 1 and 2. It changes the values presented to
the transform but requires no new decoder operation. In 4:2:2 it optimizes
against the horizontal 3:1 reconstruction kernel; in 4:2:0 it optimizes
against the separable horizontal and vertical kernel. There is no reduction
in 4:4:4, so the encoder clears the bit automatically and a decoder rejects a
4:4:4 stream that signals it. This canonicalization lets callers use the format 4
default profile with every chroma layout without creating two representations
of the same 4:4:4 profile.

The measured reconstruction-aware chroma mask is `0x17c7`: accepted format 3 contextual-RDO profile
`0x07c7` plus bit 12. The experimental joint chroma intra mask is `0x37c7`, which adds
bit 13. Joint chroma prediction requires contextual RDO because the encoder
prices both reconstructed residuals and the shared entropy symbol. The
accepted calibrated chroma quantization and default mask is `0x57c7`, reconstruction-aware chroma plus bit 14. Mask
`0x77c7` combines the accepted tools with the decodable joint chroma intra experiment.
The experimental frame segmentation mask is `0xd7c7`, calibrated chroma quantization plus bit 15. Mask `0xf7c7`
also combines the rejected joint-chroma syntax. Segmentation requires
contextual RDO and calibrated chroma quantization; the reference profile does
not admit it on an older or partially configured pipeline. The experimental
segment-adaptive loop filtering mask is `0x1d7c7`, frame segmentation plus bit 16. Adaptive filtering requires
both segmentation and the inherited loop-filter bit, because its normative
strength class is derived from the decoded segment. Bits 17 through 31 are
reserved and rejected.

When bit 14 is present, byte 27 is normative and must contain a chroma quality
from 1 through 100. The reference encoder's zero-valued API option selects
`min(luma quality + 5, 100)`, but the computed value is stored in the stream,
so decoding never depends on that encoder policy. An explicit API value from
1 through 100 overrides the policy. Byte 27 must remain zero without bit 14,
which preserves every existing format 1, format 2, format 3, reconstruction-aware chroma, and joint chroma intra byte stream.

### Reconstruction-aware 4:2:0 preparation

The decoder's normative 4:2:0 interpolation is unchanged. Each full-resolution
coordinate uses separable integer weights summing to four per axis. Interior
samples use the familiar 3:1 split, producing two-dimensional weights
`9:3:3:1` and a total scale of 16. Image edges replicate the nearest stored
sample.

The reference format 4 encoder first converts RGB to full-resolution BT.601 Y, Cb,
and Cr with the established fixed-point equations. It seeds each stored Cb and
Cr value with the rounded 2x2 average, then performs two forward/reverse
integer coordinate-descent sweeps. For one stored sample at a time it chooses
the byte value minimizing squared component error after the exact decoder
kernel. The reconstructed plane is updated after every decision, so residual
rounding error influences the next decision rather than disappearing locally.

This selection algorithm is encoder behavior, not a decoder conformance
requirement. Other encoders may choose different stored values. The flag
records the measured profile and permits controlled experiments; decoded
pixels depend only on the stored coefficient and prediction syntax.

### Entropy payload in reconstruction-aware chroma

reconstruction-aware chroma deliberately reuses format 3 payload revision 5. After stripping the
encoder-side bit 12, model count, coefficient tokens, mode tokens, partition
map, and reconstruction are exactly the syntax specified by [format 3](#format-3).
A format 4 stream with bit 12 clear and mask `0x07c7` is byte-identical to the format 3
stream after the outer version byte.

### Experimental joint chroma intra payload revision 6

Bit 13 selects payload revision 6. Coefficient tokens, amplitude bits, entropy
models, plane order, and the three-plane partition-map layout remain those of
format 3 payload revision 5. The difference is the prediction-mode grammar.

Let `Y` be the luma 8x8 block count and `C` the block count of one subsampled
chroma plane. Every block reserves four internal mode slots, as in format 3. Revision
5 stores `(Y + C + C) * 4` slots. Revision 6 stores `(Y + C) * 4`: the luma
grid followed by one joint chroma grid. The 32-bit `prediction mode count`
field must equal that reduced value exactly. Cb and Cr still have independent
coefficient streams, decoded-neighbor contexts, reconstruction buffers, and
loop filtering.

The encoder evaluates the same candidate predictor against reconstructed Cb
and Cr, sums their contextual coefficient costs, and pays one contextual mode
cost. RGB impact is approximated from the established inverse BT.601 matrix,
including the Cb/Cr cross term. The measured effort-5 experiment evaluates all
ten existing predictors; a four-mode DC/vertical/horizontal/TrueMotion trial
was measured first and rejected before the complete experiment.

The decoder entropy-decodes the joint grid once, allocates the unchanged
internal three-plane layout, and copies that grid into the Cr slots before
calling the mature format 3 reconstruction path. A revision-6 payload without bit 13,
a revision-5 payload with bit 13, a legacy mode count, or a joint-chroma flag
without contextual RDO is corrupt and must be rejected. Chroma partition-map
entries are present for grammar compatibility and are zero in this profile.

### Calibrated chroma-residual quantization

Bit 14 does not change the entropy grammar. With bit 13 clear, calibrated chroma quantization keeps
payload revision 5; with both bits set, it keeps the experimental revision 6.
The change is the normative inverse quantization used for Cb and Cr and its
independently signaled quality. Luma continues to use `Q_LUMA_BASE` and header
byte 24.

The format 4 chroma base table, in natural 8x8 coefficient order, is:

```text
17 18 24 40 70 80 90 99
18 21 26 48 70 85 95 99
24 26 40 60 80 90 99 99
40 48 60 75 90 99 99 99
70 70 80 90 99 99 99 99
80 85 90 99 99 99 99 99
90 95 99 99 99 99 99 99
99 99 99 99 99 99 99 99
```

Scaling, rounding, 4x4 derivation, coefficient order, prediction, and loop
filtering use the inherited format 3 rules. Cb and Cr use byte 27 as their quality
input, including when loop-filter strength is derived. The table retains more
medium and high-frequency chroma residual than the historical JPEG Annex K
table, whose many early 99 entries were calibrated for direct blocks rather
than causal prediction residuals. The bit is valid with 4:4:4, 4:2:2, and
4:2:0; reconstruction-aware chroma's reconstruction-aware sampling bit applies only to the two
subsampled layouts.

### Experimental segmentation payloads

Bit 15 changes the entropy payload revision. Revision 7 extends the normal format 3
three-mode-grid grammar, while revision 8 extends revision 6 when the joint
chroma experiment is also enabled. All coefficient, amplitude, prediction,
and partition-map streams retain their established order and meaning.

The payload descriptor begins with:

| Field | Encoding | Required value or meaning |
|---|---|---|
| payload revision | 1 byte | `7`, or `8` with bit 13 |
| plane count | 1 byte | `3` |
| prediction-mode coder | 1 byte | Existing rANS coder value |
| map coder flags | 1 byte | Existing flags plus bit `0x08` for the segment-map rANS stream |
| total symbol count | 4 bytes | All map, coefficient, and prediction symbols |
| rANS byte size | 4 bytes | Length of the single mixed-model rANS stream |
| amplitude bit count | 4 bytes | Inherited |
| prediction-mode count | 4 bytes | Inherited revision-5 or revision-6 count |
| partition-map count | 4 bytes | Present when 4x4 intra is enabled |
| segment-map count | 4 bytes | `ceil(width / 16) * ceil(height / 16)` exactly |
| segment quantizer scales | 4 bytes | One value per ordered class, each from 75 through 125 |

The single mixed-model rANS stream and packed amplitude bytes follow that
descriptor in their existing order. Within the symbol sequence, the segment
map is appended after the partition map and before coefficients. It contains
one symbol per 16x16 full-resolution region in raster order. The
first prediction is class 1; subsequent entries predict the left class, or
the upper class at the start of a row. The coded symbol is
`(actual - prediction) mod 4`. A decoder reconstructs each class before it
predicts the next one, so the grammar is causal and deterministic.

Entropy metadata retains its existing layout. Its model-count byte increases
by one, and the final model is the four-symbol segment-delta distribution.

Only luma quantization changes in this experiment. For every luma 8x8 or 4x4
block, the decoder resolves its containing full-resolution 16x16 region and
scales the already quality-adjusted luma table by the signaled class value:

```text
segmented_step = clamp((base_step * scale + 50) / 100, 1, 255)
```

Cb and Cr use a neutral scale of 100. Subsampled block coordinates are still
mapped through the full-resolution canvas, which makes the rule valid for
4:4:4, 4:2:2, and 4:2:0. The encoder's frame analysis and four-centre
clustering are non-normative: only the transmitted class map and scales affect
decoding. This permits a future encoder to make a better segmentation decision
without changing the format.

A revision-7/8 payload without bit 15, a revision-5/6 payload with bit 15, a
missing `0x08` coder flag, a mismatched map count, a scale outside 75 through
125, an invalid map symbol, or a segmentation feature without its required
calibrated chroma quantization tools is corrupt and must be rejected.

### Experimental segment-adaptive loop filter

Bit 16 selects payload revision 9, or revision 10 when the joint-chroma bit is
also present. Apart from the revision byte, their descriptor shape, entropy
models, segment maps, coefficient tokens, and prediction grids use the
revision-7/8 grammar unchanged. A new revision is nevertheless required
because the loop filter is inside the causal
prediction loop: changing its result changes the normative pixels available to
the next block. Clearing bit 16 or changing revision 9/10 to 7/8 must therefore
be rejected rather than decoded under a different reconstruction rule.

The ordered luma segment class maps to the inherited loop-filter adaptive
level as follows:

```text
segment:         0  1  2  3
adaptive level:  2  1  1  0
```

Segment 0 is the smoothest class and receives the slightly wider level-2 edge
threshold. Middle classes retain the accepted level 1. The most irregular
class uses level 0, which protects detail by reducing filter strength when the
base strength exceeds one. Cb and Cr deliberately remain at level 1; the format 4
diagnosis and calibrated chroma quantization had already repaired chroma, while the dominant remaining
classical deficit is luma. The quality-dependent base strength, threshold
rounding, deblocking sample order, and deringing operation are unchanged from
[format 2](#format-2).

The segment map is transmitted before coefficients and reconstructed before
pixel decoding, so both encoder and decoder have the same class at every
causal filtering step. The frame classifier itself remains non-normative.
Bit 16 without bits 15 or 7, revision 9/10 without bit 16, revision 7/8 with
bit 16, or any mismatch in the inherited segment syntax is invalid.

## Format 5

The default feature mask is `0x000657c7`, adding calibrated luma quantization and 16x16
variable luma transforms. Perceptual luma trellis and quant-adaptive filtering remain
opt-in.

This document specifies the differences between N.148i format version 5 and
version 4. Fields and entropy syntax not replaced here retain the normative
rules in [format 4](#format-4), which in turn inherits the earlier format documents.
An implementation of the format 5 library decodes format versions 1 through 5.



### Container identification

Byte 5 of the outer container is `5`. The fixed modern header remains 32
bytes, metadata immediately follows it, and the coefficient payload follows
the metadata. Width, height, payload size, luma quality, actual chroma layout,
effort, independently signaled chroma quality, and metadata size retain their
format 4 offsets and encodings.

### Feature map

Version 5 inherits bits 0 through 16 from version 4 and adds:

| Bit | Mask | Name | Status |
|---:|---:|---|---|
| 17 | `0x20000` | calibrated luma-residual quantization | accepted/default |
| 18 | `0x40000` | contextual 16x16 luma-transform choice | accepted/default |
| 19 | `0x80000` | quantization-step luma loop filter | rejected/opt-in |

The calibrated luma quantization mask is `0x257c7`. The variable luma transforms and format 5 default mask is `0x657c7`.
The rejected perceptual luma trellis experiment is reproducible with mask `0x65fc7`, which
adds inherited bit 11 (`PERCEPTUAL_TRELLIS`). The rejected quant-adaptive luma filtering experiment
is reproducible with mask `0xe57c7`; the implemented union is `0xff7c7`.
Bit 17 requires intra prediction and contextual RDO (bits 2 and 10). Bit 18
requires contextual RDO and the inherited 4x4 partition tool (bits 10 and 9),
because 4x4, 8x8, and 16x16 are priced in one decision. A stream with a set
bit and a missing dependency is invalid. Bit 19 requires the inherited loop
filter and calibrated luma quantization (bits 7 and 17). Bits 17 and 19 change
neither payload revision nor entropy layout; bit 18 selects the revision-11
grammar below.

#### Perceptual-luma interpretation of the inherited trellis bit

Formats 3 and 4 retain the historical bit-11 encoder behavior exactly: their
trellis may refine both luma and chroma using transform energy plus spatial
gradient energy. In format 5, bit 11 requests the perceptual luma trellis experiment only for
luma 4x4 and 8x8 residual units. Chroma and 16x16 residual units are not
modified. This is an encoder decision policy; the coefficient syntax and
decoder reconstruction equations do not change.

The format 5 dynamic program retains the contextual run/category rate model and
leaves DC unchanged. For a non-DC coefficient at natural coordinate `(r,c)`,
let `f = r + c` and let `E` be the historical gradient-augmented spatial
energy. Its distortion energy is:

```text
E' = E * (1 + 2 / (f + 1))   when f <= 2
E' = E                       otherwise
```

The rate multiplier remains `0.040 * E_DC`. Restricting the extra weight to
the first two frequency diagonals was selected on a two-image screen and then
checked on held-out images 11 through 13. The official ten-image result still
regressed aggregate SSIM and Butteraugli, so bit 11 is excluded from the format 5
default. A conforming decoder nevertheless accepts it because all choices are
represented by the ordinary stored coefficients, modes, and maps.

#### Quantization-step luma filter

Bit 19 changes only the normative luma filter strength. Let `s` be the DC
step obtained by scaling the calibrated bit-17 luma table at the stored luma
quality. The experimental strength is integer-only:

```text
strength = 0                         when s <= 2
strength = min(5, (s + 8) / 9)       otherwise
```

Division truncates as in C, so the second expression is `ceil(s / 9)`. At
qualities 30, 45, 60, 75, and 90 the nominal DC steps are
`37, 24, 18, 11, 4`, producing strengths `5, 3, 2, 2, 1`. The inherited
strengths are `4, 3, 2, 2, 1`; consequently only q30 changes in the official
five-point matrix. The edge threshold and causal deblock/dering equations are
unchanged.

The policy is deliberately independent of segmentation. If the rejected format 4
adaptive-filter bit is also present, its segment level does not alter luma
under bit 19. Chroma always retains the historical quality/plane strength and
never uses the new curve. No coefficient, entropy, or payload syntax changes.

The ten-image q30 audit found small regressions after composing the four
provably unchanged quality points: Y `+0.0131%`, SSIM `+0.0475%`, MS-SSIM
`+0.0272%`, SSIMULACRA2 `+0.0372%`, and Butteraugli `+0.0908%`. Bit 19 is
therefore excluded from the default but remains decodable for reproducibility.

### Calibrated luma-residual quantization

When bit 17 is clear, luma uses the inherited JPEG Annex K base table and format 5
payloads are compatible with the format 4 coefficient grammar. When it is set, all
luma 8x8 residual transforms use this base table in natural row-major order:

```text
22 15 14 20 28  44 53 60
16 16 18 23 29  58 58 53
18 16 19 27 42  56 65 53
18 20 25 32 51  80 73 57
21 25 39 55 65  96 90 67
27 36 54 61 74  91 95 77
49 60 71 77 89 101 97 81
67 80 82 83 92  82 82 77
```

Header byte 24 supplies the luma quality. The inherited quality mapping is
normative: quality is clamped to 1 through 100, a value below 50 uses scale
`5000 / quality`, otherwise scale is `200 - 2 * quality`, and every step is
`clamp((base * scale + 50) / 100, 1, 255)` with integer arithmetic.

For an inherited split cell, each 4x4 transform derives its sixteen steps by
sampling the even row and column positions of this 8x8 table, exactly as in
format 3. Forward quantization, inverse quantization, zig-zag order, prediction,
causal reconstruction, entropy contexts, and loop filtering otherwise remain
unchanged. Formats 1 through 4 must always use their historical luma table,
regardless of which decoder library version opens them.

### Variable luma transform

Bit 18 divides luma into a raster-ordered grid of 16x16 regions. Let
`Bx = ceil(width / 8)` and `By = ceil(height / 8)` be the inherited luma-cell
dimensions. The transform-map dimensions are `ceil(Bx / 2)` by
`ceil(By / 2)`. A complete region chooses one of these strategies:

- value `1`, the inherited path: each of its four 8x8 cells independently
  selects an 8x8 transform or four 4x4 transforms;
- value `2`, one 16x16 prediction and transform covering all four cells.

Value `0` remains reserved by the historical transform enum and is invalid in
this map. An incomplete right or bottom region is forced to value `1`. Chroma
has no transform map and retains its accepted 8x8-only format 4 path.

#### Transform-map coding

The map uses one luma symbol per 16x16 region. Its initial prediction is value
`1`; subsequent entries predict the left value, or the top value at the start
of a row. The coded symbol is `(actual - prediction) mod 3`. Symbols outside
the two valid reconstructed strategy values are corrupt. The transform model
is the final entropy model, has a three-symbol alphabet, and follows the
optional segmentation model in metadata.

#### Payload revision 11

Bit 18 always selects payload revision `11`, including when one of format 4's
decodable experimental bits is combined with it. The descriptor is:

| Field | Encoding | Required value or meaning |
|---|---|---|
| payload revision | 1 byte | `11` |
| plane count | 1 byte | `3` |
| prediction-mode coder | 1 byte | `1`, the inherited rANS coder |
| map coder flags | 1 byte | inherited flags, plus `0x02` for the transform map |
| total symbol count | 4 bytes | all map, coefficient, and mode symbols |
| rANS byte size | 4 bytes | length of the mixed-model rANS stream |
| amplitude bit count | 4 bytes | inherited packed magnitude bits |
| prediction-mode count | 4 bytes | inherited conceptual mode-slot count |
| partition-map count | 4 bytes | present with bit 9; full three-plane count |
| segment-map count | 4 bytes | present with bit 15 |
| transform-map count | 4 bytes | exactly `ceil(Bx / 2) * ceil(By / 2)` |
| segment scales | 4 bytes | present with bit 15, after all count fields |

Thus the accepted `0x657c7` profile has a 28-byte descriptor: its map coder
flags are `0x06`, the partition count begins at offset 20, and the transform
count begins at offset 24. The rANS stream and `ceil(amplitude_bits / 8)` raw
bytes follow the complete descriptor and any segment scales.

RANS symbols are consumed in this order:

1. the luma transform map;
2. the partition map, omitting the four inferred-zero luma entries under each
   selected 16x16 region;
3. the segment map, when present;
4. coefficient units for luma, Cb, and Cr;
5. prediction modes for luma followed by the inherited chroma grid or grids.

Luma coefficient and mode units use 16x16-region raster order. Within a value
`1` region, cells occur top-left, top-right, bottom-left, bottom-right, with
missing edge cells omitted; each cell retains the format 3 4x4/8x8 grammar. Chroma
retains ordinary plane raster order. The descriptor's partition and mode
counts describe the full conceptual arrays even though inferred entries do
not consume symbols. A decoder restores zeros for skipped partition entries
and copies a single 16x16 mode into the sixteen internal mode slots.

#### 16x16 coefficient unit and quantization

A value `2` region contains one 256-coefficient unit in the deterministic
16x16 zig-zag order from `src/variable_transform.c`. It has one absolute DC
category, AC run/category symbols over positions 1 through 255, and one
optional end-of-block symbol. Runs longer than fifteen use repeated `0xf0`
symbols. DC context uses the preceding coefficient unit's decoded DC category.
AC context uses the inherited luma frequency bands and whether the preceding
AC category is at least three; it does not consult a spatial coefficient
neighbor across transform sizes.

The transform is the project's separable, orthonormal Q14 integer DCT. Its
literal 16x16 matrix and symmetric rounding are normative. To build the
16x16 quantizer, first quality-scale the selected 8x8 luma base table. Natural
16x16 coordinate `(r, c)` samples natural 8x8 coordinate
`((r * 7 + 7) / 15, (c * 7 + 7) / 15)` with integer division. The sampled
step is then `clamp((step * 115 + 50) / 100, 1, 255)`. This 115% scale was
selected on held-out luminance curves; it is part of the bitstream, not an
encoder hint.

One prediction mode covers the complete 16x16 region. Prediction uses the
same numeric modes and integer equations as format 3, reconstructed top/left
references, and neutral value 128 for unavailable samples. Encoder and
decoder traverse regions, then inherited cells inside a region, in the same
causal order. Directional extension for an inherited cell is clamped at the
current 16x16 right boundary so a bottom cell cannot read a future region.
After reconstruction, the accepted loop filter runs once at size 16 for a
16x16 unit, or once per inherited 8x8 cell for the value-1 strategy.

The reference encoder evaluates every complete region using reconstructed
sample SSE plus contextual estimates for coefficient, raw amplitude, mode,
partition, and transform-map bits. It performs an initial format 3 pass, a
variable-transform pass, rebuilds symbol models, and refines luma once. This
search procedure is non-normative; the stored map, coefficients, and modes
fully determine decoding.

### Chroma layouts

Version 5 includes the format 4 layout correction. Reconstruction-aware reduction
is meaningful for 4:2:0 and 4:2:2: the former models horizontal and vertical
3:1 reconstruction weights, while the latter models the same horizontal
weights and an identity vertical axis. An encoder canonicalizes the feature
off for 4:4:4, where no reduction exists. A stored 4:4:4 stream that signals
the reconstruction-aware reduction bit is invalid.

The calibrated chroma table, independent chroma quality, and chroma entropy
syntax are exactly those of format 4. No luma feature changes chroma decoding.

#### Encoder-only automatic selection

`N148I_CHROMA_AUTO` (`3`) is a format 5 encoder request, not a fourth bitstream
layout. The encoder evaluates complete 4:2:0, 4:2:2, and 4:4:4 candidate
streams, decodes each candidate, and minimizes

```text
RGB SSE / 3 + 0.85 * (scaled luma DC step)^2 * exact stream bits
```

This is the image-level form of the contextual RDO objective. Candidate order
is 4:2:0, 4:2:2, then 4:4:4; an exact cost tie retains the smaller stream and
then the earlier candidate. Header byte 25 always stores the chosen concrete
value `0`, `1`, or `2`; a decoder rejects `3` like any other unknown stored
layout. No feature flag or payload syntax is added.

Effort zero deliberately skips the search and resolves the request directly
to 4:2:0. Explicit layout requests also bypass it. Formats 1 through 4 reject
the encoder-only automatic value, because they retain their historical option
contract.

### Decoder rejection requirements

In addition to all inherited length, revision, model-count, partition-count,
mode-count, and feature-dependency checks, a format 5 decoder rejects:

- any feature bit outside the format 5 implemented mask;
- calibrated luma quantization without intra prediction and contextual RDO;
- 16x16 luma transforms without contextual RDO or 4x4 partition support;
- a quantization-step luma filter without both the inherited loop filter and
  calibrated luma quantization;
- an actual chroma layout other than 4:4:4, 4:2:2, or 4:2:0;
- reconstruction-aware reduction signaled with stored 4:4:4 chroma;
- revision 11 without bit 18, bit 18 with another payload revision, a missing
  transform-map coder flag, a mismatched transform count, an invalid strategy,
  a 16x16 strategy on a partial region, or any nonzero inferred partition.

## Format 6

The default feature mask is `0x002657c7`, adding weighted planar prediction. The other
assigned format 6 feature bits are retained only where this section marks them
decodable; reserved combinations must be rejected.

This document specifies the differences between N.148i format version 6 and
version 5. Fields and entropy syntax not replaced here retain the normative
rules in [format 5](#format-5), which inherits the earlier format documents. A format 6
library decodes format versions 1 through 6.



### Container identification

Byte 5 of the outer container is `6`. The fixed modern header remains 32
bytes. All header offsets, the metadata placement, the independently stored
luma and chroma qualities, and the concrete chroma-layout field are unchanged
from format 5.

The accepted format 6 stream uses payload revision 11 and the format 5 transform-map
grammar. It adds no header or descriptor field. A decoder must select the format 6
prediction equations from the format byte and feature map; it must never apply
them while decoding a format 1-5 stream.

### Feature map

Version 6 inherits bits 0 through 19 from version 5 and adds:

| Bit | Mask | Name | Status |
|---:|---:|---|---|
| 20 | `0x100000` | filtered intra references | rejected/opt-in |
| 21 | `0x200000` | weighted planar intra prediction | accepted/default |
| 22 | `0x400000` | structural RDO term | rejected/opt-in |
| 23 | `0x800000` | quality-calibrated RDO lambda | rejected/opt-in |
| 24 | `0x1000000` | 32x32 luma transform | rejected/opt-in |
| 25 | `0x2000000` | second-order DC predictor | rejected/opt-in |
| 26 | `0x4000000` | finer directional predictors | rejected/opt-in |

The accepted/default mask is `0x2657c7`: the accepted format 5 mask `0x657c7` plus
bit 21. The principal experimental masks are:

- `0x10657c7`: 32x32-transform experiment;
- `0x1657c7`: filtered-reference experiment;
- `0x3657c7`: filtered references plus planar prediction;
- `0x6657c7`: the accepted planar profile plus structural RDO;
- `0xa657c7`: the accepted planar profile plus quality-calibrated lambda.

All format 6 bits require inherited intra prediction and contextual RDO (bits 2 and
10). Bit 21 additionally requires directional intra (bit 8). Bit 24 requires
the format 5 16x16 transform and calibrated luma quantization (bits 18 and 17). A
stream with a missing dependency, a bit outside the implemented format 6 map, or a
format 6-only bit under an older format byte is invalid.

### Accepted weighted planar prediction

Bit 21 changes only the reconstruction associated with inherited prediction
mode 3 (`TRUE_MOTION`) on the luma plane when the selected transform size is
8, 16, or 32. It does not add a prediction-mode symbol or entropy context.
Chroma and luma 4x4 retain their historical mode-3 equations. Preserving the
4x4 predictor is normative: replacing it was the cause of the Butteraugli
regression in the first full intra-prediction experiments candidate.

Let `N` be the transform size, `T[x]` the reconstructed top reference,
`L[y]` the reconstructed left reference, `T[N]` the top reference one sample
beyond the block, and `L[N]` the analogous left reference. For
`0 <= x,y < N`, the predicted sample is:

```text
P(x,y) = ((N-1-x) * L[y] + (x+1) * T[N]
        + (N-1-y) * T[x] + (y+1) * L[N] + N) / (2*N)
```

Division is integer truncation after the positive rounding offset `N`, and
the result is clamped to 0 through 255. Unavailable boundaries and causal
right-edge extension use the same neutral-128 and clamping rules as format 5.

This is a normative decoder operation. The encoder remains free to choose any
stored mode using any conforming search, but the stored coefficients are
residuals against this predictor whenever bit 21 and mode 3 apply.

### Rejected prediction experiments

These tools remain normative when explicitly signaled, despite not belonging
to the default profile.

#### Filtered references (bit 20)

For luma transforms of size 8 or larger, each available top and left reference
array is replaced by its three-tap filtered version before calculating any
mode:

```text
F[i] = (R[clamp(i-1)] + 2*R[i] + R[clamp(i+1)] + 2) / 4
```

The array endpoints are extended by replication. Four-by-four luma and all
chroma predictions remain unchanged. The ten-image result regressed
SSIMULACRA2 and Butteraugli, so bit 20 is excluded from the default.

#### Second-order DC (bit 25)

When both boundaries exist, let `A` be the ordinary DC value, `U` and `V` the
rounded top and left means, and `C` the top-left sample. The experimental DC
value is `clamp((A + U + V - C + 1) / 2)`. If either boundary is
missing, the inherited DC value is used. Its measured screen regressed and it
is excluded from the default.

#### Fine directions (bit 26)

Bit 26 changes the half-sample index used by the inherited vertical-left and
horizontal-up modes, using half the orthogonal displacement instead of its
full value. All stored mode numbers remain unchanged. The measured screen
regressed and the bit is excluded from the default.

### Rejected encoder-search experiments

Bits 22 and 23 affect encoder decisions only. The decoder reconstructs from
the ordinary stored transform map, partition map, modes, and coefficients.

Bit 22 adds a deterministic local-contrast penalty to luma region SSE. For a
region with `n` samples, define `R(X) = sqrt(n*sum(X^2)-sum(X)^2)` with the
integer square root used by the reference encoder. The retained experimental
cost adds:

```text
(R(source) - R(reconstruction))^2 / ((n + 2) / 4)
```

with integer rounding and saturation. The coefficient was screened at
multiple strengths; its incremental benefit remained below the 0.5% gate.

Bit 23 changes the inherited `lambda = scale * dc_step^2` scale from 0.85 to
0.70 at qualities up to 45, 0.75 up to 60, and 0.80 up to 75. Qualities above
75 retain 0.85. Its measured trade did not improve both SSIM and PSNR over
the accepted planar profile, so it is excluded from the default.

### Rejected 32x32 luma transform

Bit 24 extends the revision-11 luma transform grammar without changing its
three-symbol alphabet. A canonical aligned 2x2 group of transform-map entries
with strategy value 0 (`DCT4`, unreachable in the format 5 luma map) represents one
32x32 luma transform. The upper-left entry must have even map coordinates,
all four entries must exist, and the corresponding 4x4 grid of 8x8 cells must
be complete. An isolated or misaligned strategy-0 entry is corrupt. Chroma
still permits only strategy 1 (`DCT8`).

One 32x32 unit has one prediction mode and 1,024 coefficients in the
deterministic zig-zag order. The sequence is stored across the sixteen
existing 8x8 coefficient slots, 64 consecutive values per slot in cell-raster
order. All sixteen partition values are inferred zero; only the first
conceptual mode consumes an entropy symbol and the decoder copies it to the
remaining mode slots. Tokenization otherwise follows the format 5 large-unit rules:
one contextual DC category, contextual AC run/category symbols, repeated
`0xf0` for long runs, and an optional end-of-block symbol.

The transform is the separable orthonormal Q14 integer DCT whose literal
matrix is in `src/dct32_q14.inc`. Its FNV-1a byte hash is
`0xb27f5da0e4d360b1`; symmetric Q28 rounding is identical to the 16x16 path.
To derive its quantizer, quality-scale the selected 8x8 luma base table, then
map natural coordinate `(r,c)` to
`((r*7+15)/31, (c*7+15)/31)` using integer division. The sampled step is
`clamp((step*115+50)/100, 1, 255)`.

The reference encoder searches 32x32 only at effort 5 or above. It was
rejected because its best measured three-image candidate improved SSIM by
only 0.20% while making encoding about 3.5 times slower. The syntax remains
decodable to preserve experimental reproducibility.

### Chroma and compatibility

Version 6 inherits the formats 5/4 4:2:0, 4:2:2, and 4:4:4 paths unchanged. The
automatic encoder request still evaluates concrete streams and stores only
the selected layout. A full 50-point forced-layout audit found that the
official portrait sample legitimately favors 4:2:0 in every point under the
existing exact-stream RDO cost; a saturated high-frequency fixture selects
4:4:4. No chroma equation, table, signal, or default changes in format 6.

Formats 1 through 5 retain their historical feature maps, payload grammars,
prediction equations, quantizers, and loop filters. A format 6 implementation must
dispatch by the stored format byte before interpreting format 6 bits. Unknown
versions remain unsupported rather than guessed.

## Format 7

V2 defaults to the detail profile `0xd8a6d7ff`. The compatible base mask
`0x18a657c7` combines adaptive
entropy probabilities, rich coefficient contexts, and bit 23's calibrated
luma search. The rich-context mask `0x182657c7` remains valid and decodable.
The optional spectral profile is `0x18a657cf`. The refined spectral
profile is `0x18a657df`; the structural profile is `0x18a6d7ff`. Multiple intra
references remain opt-in. Bit 31 selects the balanced reconstruction
profile, `0x98a6d7ff`. Bit 30 extends balanced reconstruction with the detail
profile, `0xd8a6d7ff`. The formerly planned
directional scan/transform profile masks remain unsupported.

This document specifies the differences between N.148i format version 7 and
version 6. Fields and syntax not replaced here retain the normative rules in
[format 6](#format-6), which inherits the earlier format documents. A format 7 library
decodes format versions 1 through 7.



### Container identification and feature map

Byte 5 of the outer container is `7`. The fixed modern header remains 32
bytes, with the same offsets, luma/chroma quality fields, concrete chroma
layout, and little-endian integer representation as format 6.

Version 7 inherits bits 0 through 26 from version 6 except for bits 3, 4, and 5.
Bit 3 selects the spectral luma quantization table only in format 7; format 2
retains its historical perceptual-color meaning for that bit. Bit 4 refines
the spectral table only in format 7; format 2 retains its historical
adaptive-quantization meaning. Bit 5 selects the structural luma table in
format 7; format 2 retains its historical variable-transform meaning.
Format 7 assigns:

| Bit | Mask | Name | Status |
|---:|---:|---|---|
| 3 | `0x00000008` | spectral luma quantization | accepted; requires calibrated luma quantization, bit 17 |
| 4 | `0x00000010` | refined spectral luma quantization | accepted; requires bit 3 |
| 5 | `0x00000020` | structural luma quantization | accepted; requires bits 4, 10, 14, 15, and 18, including their inherited dependencies |
| 27 | `0x08000000` | adaptive entropy probabilities | accepted |
| 28 | `0x10000000` | rich coefficient contexts | accepted |
| 29 | `0x20000000` | multiple intra references | accepted experimental profile; opt in |
| 30 | `0x40000000` | detail reconstruction | accepted; requires balanced bit 31 and its dependencies |
| 31 | `0x80000000` | balanced reconstruction | accepted; requires structural bit 5 and its dependencies |

The compatible base mask is `0x18a657c7`; the optional spectral masks are
`0x18a657cf` and `0x18a657df`, the structural mask is `0x18a6d7ff`, the
balanced mask is `0x98a6d7ff`, the detail mask is `0xd8a6d7ff`, and the
rich-context mask is `0x182657c7`. The
multiple-reference prediction experimental mask is `0x382657c7`. Bit 27
requires inherited rANS and context bits 0 and 1. Bit 28
additionally requires intra prediction (bit 2) and bit 27. Bit 29 requires
bit 28, directional intra prediction, and contextual RDO. Reserved bits and
format 7 bits stored under an older format byte are unsupported rather than guessed.

Bit 23 affects only encoder rate-distortion decisions. Its calibrated luma
search changes which existing prediction and transform choices are stored;
it adds no payload syntax or decoder work. The decoder reads either mask and
reconstructs the signaled choices with the same format-7 rules.

### Spectral luma quantization

When bit 3 is set, format 7 substitutes the following luma base table for the
calibrated format-5 table. Bit 17 is required. The existing quality scaling,
segment scaling, and 4×4/16×16/32×32 table derivation then apply without any
other syntax change. Bit 4 selects the refined table below when bit 3 is also
set. Bit 3 clear retains the exact pre-existing table and decoding rules.

```text
26 17 16 20 28 44 53 48
19 19 18 23 29 58 46 42
21 16 19 27 42 45 52 42
18 20 25 32 41 64 58 46
21 25 39 44 52 77 72 46
27 36 43 49 59 73 65 52
49 48 57 62 71 69 66 55
54 64 66 66 63 56 56 52
```

The refined table raises the low-band base steps by about 8% at positions
whose row and column indices sum to at most 2, retains the middle band, and
reduces high-band steps by about 4% where the index sum is at least 7.
Integer rounding produces the normative values below. The table is selected
only when both bits 3 and 4 are set; older format-7 readers reject bit 4.

```text
28 18 17 20 28 44 53 46
21 21 18 23 29 58 44 40
23 16 19 27 42 43 50 40
18 20 25 32 39 61 56 44
21 25 39 42 50 74 69 44
27 36 41 47 57 70 62 50
49 46 55 60 68 66 63 53
52 61 63 63 60 54 54 50
```

### Structural luma quantization

Bit 5 selects this separate luma base table, taking precedence over the
spectral and refined tables. Bits 3 and 4 remain required. Its values are
derived from N.148i's own spatial basis error energy with six protected low
frequencies. They are normative decoder constants:

```text
28 18 17 28 33 41 45 42
21 21 27 30 34 47 41 39
23 26 27 32 40 41 44 39
27 28 31 35 39 49 47 41
29 31 39 40 44 54 52 41
32 37 40 43 47 52 49 44
44 42 46 48 51 51 49 45
45 49 49 49 48 46 46 44
```

Existing quality scaling, signaled segment scales, and 4x4/16x16/32x32
quantizer derivation apply to this base table. The feature also requires
contextual RDO, calibrated chroma quantization, segmentation, and 16x16
luma-transform support. Reconstruction-aware chroma is allowed but is not
required: full-chroma canonicalization clears that feature. No metadata,
coefficient alphabet, transform or prediction syntax is added. Older
format-7 readers reject the new bit rather than decoding with an old table.
The default, spectral and refined tables retain their exact values.

The structural-profile encoder uses source-variance segmentation with eighth-root
quantizer allocation, six quantized luma mode candidates at effort 3,
contextual coefficient refinement, and five-eighths of the RGB squared-error
luma projection from reconstructed chroma. These are encoder decisions;
the decoder consumes only their existing signaled choices and coefficients.
The public API resolves zero chroma quality to luma quality for this profile;
the actual resolved quality remains explicitly stored in the header.
Structural streams carry their decoding choices explicitly.

### Balanced reconstruction

Format 7 bit 31 selects the balanced luma base table and output chroma
filter. It requires structural bit 5 and all of that bit's transitive
dependencies. Its profile mask is `0x98a6d7ff`. Bit 30 additionally selects
detail reconstruction, specified below. The previously planned directional scan/transform profile masks remain
unsupported. Readers without balanced reconstruction reject bit 31. Existing profile masks
continue to select their original tables and reconstruction rules.

The following table takes precedence over the structural table when bit 31
is set. These integer values are normative. Existing quality/segment scaling
and derivation of the other transform sizes apply to this base table.

```text
28 25 25 28 29 31 32 31
26 26 28 28 29 32 31 30
27 27 28 29 31 31 31 30
28 28 29 30 30 32 32 31
28 29 30 31 31 33 33 31
29 30 31 31 32 33 32 31
31 31 32 32 33 33 32 32
32 32 32 32 32 32 32 31
```

After all causal reconstruction and existing loop filtering finish, apply
the following filter independently to the stored Cb and Cr planes. Y is
unchanged by this stage. Every neighborhood reads the unmodified input of
this stage; filtering does not feed future prediction references. Extend
each plane by clamping coordinates to its nearest border sample.

For a center sample `c` and the sum `s` of its eight immediate neighbors,
including diagonal positions and any samples repeated by border extension:

```text
output = floor((28*c + s + 18) / 36)
```

This is one-quarter mixing with the local 3x3 mean, rounded to nearest with
half ties toward positive infinity. It operates at the stored chroma
resolution for all layouts, before the established interpolation and RGB
conversion. All inputs and outputs are bytes. The convex combination stays
within 0..255 without a separate clip. The same equation applies at every
quality; there is no additional filter strength field.

No entropy alphabet, transform definition, predictor equation, metadata
revision or payload grammar is added. The container feature bit selects
both normative changes. The balanced-profile encoder policy also uses quarter-strength
luma projection and contextual coefficient refinement for chroma at effort 3;
these choices need no additional syntax. Zero chroma quality continues to
resolve to luma quality through the inherited structural-profile rule.

### Detail reconstruction

Format 7 bit 30 selects a finer 16x16 luma quantizer derivation. It requires
balanced bit 31 and all of that bit's transitive dependencies. The profile
mask is `0xd8a6d7ff`; the public names are
`N148I_FORMAT_7_DETAIL_RECONSTRUCTION` and
`N148I_PROFILE_DETAIL_RECONSTRUCTION`. Readers without detail reconstruction reject this
marker. The old public directional scan/transform feature constants keep
their numeric values for source compatibility, but their planned profile
masks remain unsupported and do not satisfy the new dependencies.

First obtain the balanced 8x8 table after the established quality scaling
and the signaled segment scaling. For each 16x16 frequency `(r,c)`, with
zero-based indices, use:

```text
source_r = floor((7*r + 7) / 15)
source_c = floor((7*c + 7) / 15)
step16[r,c] = table8[source_r,source_c]
```

Thus the final 16x16 scale is 100%, replacing the previous
`min(255, floor((115*table8[source_r,source_c] + 50)/100))` only when bit 30
is set. The 4x4, 8x8 and optional 32x32 rules, balanced output chroma filter,
transform definitions and entropy grammar retain their existing equations.
No extra header, map symbol or coefficient alphabet is introduced. Bit 30
clear selects the established 115% rule at every effort and quality.

The V2 encoder at effort 3 ranks four native 4x4 predictors by quantized
distortion and native syntax cost, then refines coefficients of the selected
predictor with the existing contextual dynamic program. Spatial costs use
the continuous synthesis energy of N.148i's own Q14 basis. Frame allocation
limits local variance using source prediction error and clusters log activity
before deriving the existing four signaled quantizer scales. The luma block
decision multiplier is 0.9 times its prior value. These encoder policies do
not add decoder syntax; the decoder follows the stored maps and coefficients.

### Dual entropy representation

A stream whose outer feature map contains bits 27 and 28 can use either of two
entropy representations. The first metadata byte selects the representation:

- metadata revision `1`: exact inherited format 6 model and payload syntax;
- metadata revision `2`: the rich adaptive syntax defined below.

Revision 1 is a normative per-image fallback. Without bit 29, all prediction
modes are below 10 and their syntax is inherited from format 6. Format 7
quantization-table selections and the balanced output filter apply independently
of the metadata revision. With bit 29, revision 1 retains the multiple-reference prediction syntax below
but uses inherited fixed entropy tables and the inherited payload revision
(revision 11 in the multiple-reference prediction profile). This lets the encoder retain the smaller
complete representation without a separate flag. A
decoder must dispatch from metadata revision before interpreting the entropy
payload; it must not require revision 2 merely because bits 27 and 28 are set.

Rich streams use payload revision `12`. Their token order is transform map,
partition map, optional segment map, prediction modes, then Y/Cb/Cr
coefficients. This differs from revision 11, whose coefficient tokens precede
prediction modes. Decoding modes first makes the prediction class available
to the coefficient context without transmitting it twice.

### Rich coefficient contexts

There are 119 logical coefficient models. Luma uses 108 adaptive child models:

- 18 DC models: `3 size classes * 2 prediction classes * 3 previous-DC`
  buckets;
- 90 AC models: `3 size classes * 2 prediction classes * 5 position`
  buckets `* 3 activity` buckets.

The size classes are 4x4, 8x8, and 16x16/32x32. Prediction class zero is DC
mode; class one is every directional mode. AC positions 1, 2, and 3 have exact
buckets, positions 4 through 14 share a bucket, and positions 15 onward share
the last bucket. The activity value is the sum of the magnitude of the
previous nonzero coefficient and the same packed coefficient position in the
already coded left and top blocks. Sums 0, 1--3, and 4 or more map to buckets
0, 1, and 2.

Chroma is deliberately unchanged in this round and occupies 11 logical
models: three inherited DC buckets and four position bands by two binary
activity classes. The remaining mode, partition, segment, and transform-map
models retain their inherited definitions.

Logical coefficient children do not carry independent initial tables. Each
inherits one of the 22 format 2 context distributions. Luma size and prediction
classes collapse into the matching luma parent; the first three exact AC
positions map to parent band zero, positions 4--14 to band one, and the final
bucket to band two. Activity bucket two maps to active and buckets zero/one to
inactive. Chroma maps directly to its historical parents.

### Metadata revision 2

Revision-2 metadata begins with these bytes:

```text
u8 revision = 2
u8 coder = 1                 # byte-rANS
u8 logical_model_count
u8 scale_bits = 12
u8 parent_model_count = 22
u8 adaptive_mask_bytes = 14
u8 adaptive_mask[14]
```

Bits beyond the 108 luma children in the last mask byte must be zero. One bit
selects adaptation for the child with the same zero-based index. Chroma and
non-coefficient models never adapt in the accepted profile.

The 22 parent models follow in inherited sparse-frequency-table syntax. Each
logical coefficient child reconstructs its initial model by copying its
specified parent. Models after logical coefficient model 118 are then stored
normally in logical order. The reconstructed model count must equal the count
declared above; metadata must be consumed exactly.

### Normative adaptive update

Every model has 256 unsigned frequencies whose sum is exactly 4,096. Symbols
with a nonzero initial frequency must never reach zero. For an observed symbol
`s`, let

```text
available = sum(max(freq[x] - 1, 0)) for every x != s
transfer  = available >> 6
```

If `transfer` is zero or the model has one symbol, the model is unchanged.
Otherwise distribute `transfer` deductions across symbols other than `s` in
proportion to `freq[x]-1`. Integer quotients are assigned first. Remaining
units go, one per eligible symbol, in descending fractional-remainder order;
ties go to the lower symbol value. Subtract those deductions and add their sum
to `freq[s]`. The total remains 4,096. A Fenwick tree is an implementation
detail; it does not change this rule.

For a mask-selected child, the encoder and decoder apply the update immediately
after consuming that symbol. Unselected children and all models outside the
108-child range remain fixed. Encoder and decoder arithmetic is unsigned
integer arithmetic; floating point is forbidden in the normative update.

Byte-rANS encodes in reverse, whereas adaptation is causal in forward order.
The encoder must therefore replay tokens forward, save the frequency and
cumulative count used by each token, and only then emit rANS state transitions
in reverse. The decoder obtains the same pair from its current model and
updates after each forward-decoded symbol.

The reference encoder selects a child's mask bit only when the sum of natural
log frequencies from the adaptive replay exceeds the fixed replay by more than
`ln(2)`, a one-bit hysteresis. It also compares the realized adaptive rANS
stream with a fixed-child rANS stream and clears the complete mask if the fixed
stream is smaller. These are encoder decisions; either mask is conforming.
At effort levels 0 through 4, the reference encoder selects revision 1
directly so the expensive dual representation search is skipped.

### Experimental multiple-reference prediction

When bit 29 is set, a luma prediction mode is represented internally as

```text
packed_mode = base_mode + 10 * reference_index
```

`base_mode` is one of the inherited values 0 through 9. `reference_index` 0,
1, or 2 selects reconstructed samples one, two, or three pixels away from the
current block. Chroma and split 4x4 luma blocks require reference index zero.
The reference encoder searches all three distances for unsplit 8x8, 16x16,
and 32x32 luma candidates only at effort 5 or above.

For distance `d = reference_index + 1`, the top array samples row `y - d`
starting at column `x`, the left array samples column `x - d` starting at row
`y`, and the corner sample is `(x - d, y - d)`. A top or left reference is
available only when its coordinate is nonnegative; an unavailable side uses
the inherited neutral value 128. Right and bottom extension retains the format 6
clamping and causal macroblock limits. No filtering is implied by bit 29.

The entropy symbol carries a neighbor-predicted ternary residual rather than
the absolute reference index. For the current base mode, inspect the causal
left and top virtual-mode neighbors:

- if both have the same base mode and reference index, predict that index;
- if exactly one has the same base mode, predict its index;
- otherwise predict index zero.

Then compute

```text
delta  = (reference_index + 3 - predicted_index) % 3
symbol = base_mode + 10 * delta
```

The decoder obtains `base_mode` and `delta` from the symbol, derives the same
neighbor prediction, reconstructs the packed mode, and only then stores it.
The inherited twelve spatial mode contexts compare neighboring base modes,
not packed values. Thus the new information is one ternary choice (less than
two raw bits) conditioned on reconstructed neighboring choices. Large
transform regions signal one packed mode at their origin and replicate it to
their member cells exactly as in formats 5/6.

The reference encoder additionally requires every selected distant-reference
candidate to have no greater reconstructed local distortion than the best
distance-one candidate. That is an encoder quality safeguard, not a bitstream
constraint. A streaming decoder needs two more reconstructed rows and columns
than format 6; the reference full-plane decoder already retains them.

### Compatibility

Formats 1 through 6 retain their historical feature maps, token order,
probability tables, prediction equations, quantizers, and reconstruction.
Format-7 files with no format 7 feature bits are identical to their format-6
counterparts except for byte 5. Unknown metadata revisions, nonzero unused mask
bits, invalid dependencies, truncated rANS states, and trailing payload bytes
are errors. Debug validation should additionally compare the final encoder and
decoder model state after each plane; that comparison is not a transmitted
bitstream field.

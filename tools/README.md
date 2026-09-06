<!--
N.148i benchmark reproduction guide
Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.
-->

# Reproducing the N.148i v1 benchmark

This directory contains the complete benchmark pipeline behind
[`BENCHMARK.md`](../BENCHMARK.md). The frozen inputs, raw measurements, run
metadata, and generated analysis are under [`benchmarks/v1/`](../benchmarks/v1/).
Run all commands from the repository root on Linux.

The final driver calls N.148i, libjpeg-turbo, and libjxl directly in one
process. Disk I/O and quality metrics are outside the timed regions. The Python
orchestrator fixes CPU affinity, avoids SMT siblings, rotates codec and quality
order, discards warm-up iterations, and reports medians with dispersion.

## 1. Install dependencies

On Ubuntu or Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake git ninja-build pkg-config \
  libjpeg-turbo8-dev libjxl-dev libjxl-tools imagemagick \
  python3-venv util-linux ffmpeg

python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install numpy scipy scikit-image Pillow sewar matplotlib
```

Package names vary by distribution. `libjpeg-dev` is an acceptable package
name when the distribution provides libjpeg-turbo through that package. Check
the JXL version before building the driver:

```bash
pkg-config --modversion libjxl
```

The benchmark requires libjxl 0.8 or newer. The published run used v0.12.0
because the distribution package was 0.7.0. When the packaged version is too
old, build the official release with its tools enabled:

```bash
N148I_JXL_ROOT=/tmp/n148-libjxl-0.12.0
git clone --recursive --branch v0.12.0 https://github.com/libjxl/libjxl \
  "$N148I_JXL_ROOT"
cmake -S "$N148I_JXL_ROOT" -B "$N148I_JXL_ROOT/build" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DJPEGXL_ENABLE_TOOLS=ON -DJPEGXL_ENABLE_BENCHMARK=OFF \
  -DJPEGXL_ENABLE_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$N148I_JXL_ROOT/install"
cmake --build "$N148I_JXL_ROOT/build" --parallel
cmake --install "$N148I_JXL_ROOT/build"
make benchmark-final JXL_PREFIX="$N148I_JXL_ROOT/install"
```

This follows libjxl's official `BUILDING.md`. A sufficiently new system
package needs only `make benchmark-final`. Build the remaining gates before
collecting data:

```bash
make compare validate
```

## 2. Reconstruct the frozen corpus

`fetch_corpus.py` reads the v1 manifest, downloads each recorded source,
replays its ImageMagick command, validates P6 dimensions, and checks both the
source and final PPM SHA-256. It never substitutes a changed or unavailable
source silently.

```bash
python tools/fetch_corpus.py
```

Valid files already present in `images/` are hash-checked and retained. To
rebuild selected entries, or every entry, use:

```bash
python tools/fetch_corpus.py --only 1 2 3 --force
python tools/fetch_corpus.py --force
```

The script produces `images/corpus-0001.ppm` through
`images/corpus-0120.ppm`. A broken URL, changed download, conversion mismatch,
or final hash mismatch is printed to stderr and causes a nonzero exit status.
If another ImageMagick executable is required, pass
`--convert-binary /path/to/convert`.

## 3. Prepare or verify the corpus

`prepare_final_corpus.py` is the corpus-authoring tool. It enforces license and
category rules, normalizes accepted images, and writes the two manifest copies
plus human-readable attribution. On the already complete v1 corpus, this
command verifies every existing hash before doing any work:

```bash
python tools/prepare_final_corpus.py --root . --per-category 15
```

It produces or updates:

- `benchmarks/v1/corpus-manifest.json`, the authoritative machine-readable
  manifest;
- `images/MANIFEST.json`, its release-friendly mirror;
- `images/CREDITS.txt`, licenses, authors, and source links;
- `images/corpus-*.ppm`, the normalized P6 inputs.

For exact v1 reproduction, do not use `--renormalize-existing`,
`--replace-pageid`, or `--resample-unchanged-jpeg`: those options curate a new
corpus and intentionally change hashes. New discovery can use
`--provider openverse` or `--provider commons`.

## 4. Capture the machine and correctness gates

`collect_benchmark_environment.py` records CPU topology and flags, kernel,
compiler, package and linked-library versions, tool hashes, power settings,
metric availability, and the libjxl source revision. It also runs the 2,290
byte sentinel and the full validator.

For a system libjxl:

```bash
python tools/collect_benchmark_environment.py \
  --jxl-source /path/to/libjxl \
  --ssimulacra2 "$(command -v ssimulacra2)" \
  --butteraugli "$(command -v butteraugli_main)"
```

For the source build shown above:

```bash
python tools/collect_benchmark_environment.py \
  --jxl-source /tmp/n148-libjxl-0.12.0 \
  --ssimulacra2 /tmp/n148-libjxl-0.12.0/build/tools/ssimulacra2 \
  --butteraugli /tmp/n148-libjxl-0.12.0/build/tools/butteraugli_main
```

It writes `benchmarks/v1/environment.json` and
`benchmarks/v1/validation.log`. Stop if any `correctness_gates` value is false.

## 5. Run the final matrix

`benchmark_final.py` runs five rate-distortion points for all codecs on the
single-thread axis, measures full-throughput configurations, and sweeps N.148i
over 1, 2, 4, 8, and the physical-core count. The fixed mapping is N.148i/JPEG
quality 30, 45, 60, 75, and 90 against JXL distance 8.0, 5.0, 3.0, 1.5, and
0.7, at JXL efforts 3 and 7.

```bash
. .venv/bin/activate
python tools/benchmark_final.py \
  --ssimulacra2 /tmp/n148-libjxl-0.12.0/build/tools/ssimulacra2 \
  --butteraugli /tmp/n148-libjxl-0.12.0/build/tools/butteraugli_main \
  --reps 15 --max-reps 255 --parallel-max-reps 31 \
  --warmups 3 --sample-ms 20 --noise-target-pct 1
```

Important parameters:

- `--phases axis_a axis_b sweep` selects efficiency, throughput, and scaling;
- `--reps` is the initial sample count, while the two maximums bound automatic
  noise refinement;
- `--warmups` controls discarded warm-up samples;
- `--sample-ms` makes the C driver calibrate inner loops to reduce timer noise;
- `--limit-images N` or `--image-ids ...` creates a diagnostic subset.

The full published run took about 8.5 hours on the documented ten-core,
twelve-thread Linux machine. Time varies substantially with image size, CPU,
and how many series need more repetitions.

The run is resumable. After every image/quality point it atomically updates
`final-results.csv`, `timing-samples.csv`, and `benchmark-metadata.json`.
Re-run the exact same command after an interruption; completed stable points
are skipped. The tool refuses to resume if the driver hash or material run
configuration changed, which prevents accidental mixing of experiments.

The output files are:

- `benchmarks/v1/final-results.csv`: one unrounded summary row per image,
  codec, quality point, and axis;
- `benchmarks/v1/timing-samples.csv`: every individual encode/decode timing;
- `benchmarks/v1/benchmark-metadata.json`: configuration, hashes, commands,
  resume history, and protocol amendments;
- `benchmarks/v1/.work/final/`: temporary reconstructions, removed as points
  complete.

Do not wrap the driver in an additional timer. The C driver loads the PPM once,
then times only direct codec API calls. It rotates codec order within each
replicate, while the orchestrator rotates quality order by image. `taskset` is
applied automatically to one logical CPU for axis A and to one logical CPU per
physical core for parallel phases, avoiding SMT siblings.

## 6. Analyze the results

`analyze_final_benchmark.py` validates matrix completeness and emits PCHIP
BD-rate, paired Wilcoxon tests, category analysis, timing dispersion, metric
agreement, extreme cases, and deterministic SVG plots:

```bash
. .venv/bin/activate
python tools/analyze_final_benchmark.py
```

It writes `benchmarks/v1/analysis/summary.json`, sixteen focused CSV tables,
and the rate-distortion and thread-scaling plots under
`benchmarks/v1/analysis/plots/`. Existing files are replaced atomically.

## Other benchmark tools

### `benchmark_random_corpus.py`

This is the older streamed N.148i versus JPEG workflow. It downloads and
normalizes one image at a time, supports an A/B baseline binary, checkpoints
after every accepted image, and can replay a frozen manifest.

```bash
python tools/benchmark_random_corpus.py \
  --compare ./compare --output-dir /tmp/n148i-random-run \
  --count 100 --qualities 30 45 60 75 90 --reps 31 \
  --chroma 2 --threads 1 --cpu 0
```

It produces `results.csv`, `manifest.json`, `metadata.json`, `failures.json`,
`REPORT.md`, and optionally `baseline-results.csv` in the selected directory.

### `analyze_local_matrix.py`

This legacy analyzer consolidates the original fixed-path JPEG-only thread
runs (`benchmarks/v1/local-pinned-*`) into local scaling, per-quality,
per-image, repeatability, and verification files.

```bash
python tools/analyze_local_matrix.py
```

Those raw run directories are not part of the final committed dataset, so the
command is useful only when they have been restored at the paths declared near
the top of the script. The final benchmark should use
`analyze_final_benchmark.py` instead.

### `compare_benchmark_runs.py`

This tool performs a paired candidate-versus-baseline comparison, normalizes
N.148i timing against the JPEG control, and can bootstrap uncertainty:

```bash
python tools/compare_benchmark_runs.py \
  --candidate /tmp/candidate/results.csv \
  --baseline /tmp/baseline/results.csv \
  --manifest /tmp/candidate/manifest.json \
  --output /tmp/comparison.json \
  --output-csv /tmp/comparison.csv
```

It produces the requested JSON verdict and one detailed paired CSV.

### `recheck_entropy_outliers.py`

This targeted tool reruns candidate/baseline points flagged as timing or
entropy outliers against the same frozen PPMs:

```bash
python tools/recheck_entropy_outliers.py \
  --candidate-compare ./compare \
  --baseline-compare /tmp/baseline/compare \
  --manifest /tmp/candidate/manifest.json \
  --comparison-csv /tmp/comparison.csv \
  --candidate-results /tmp/candidate/results.csv \
  --output /tmp/outlier-rechecks.csv --reps 301
```

It writes one CSV containing both timings, normalized deltas, compressed
sizes, and the verified PPM hash for every selected outlier.

## Methodology checklist

Before publishing a new result, verify all of the following:

- the sentinel is 2,290 bytes and `./validate` reports zero failures;
- the exact corpus manifest and PPM hashes match;
- all codecs run in the same C process with input already in memory;
- single-thread efficiency and multi-thread throughput remain separate;
- CPU affinity excludes SMT siblings and the codec order is interleaved;
- warm-ups are discarded and medians, IQR/MAD, and residual noise are kept;
- a noisy series is refined up to its declared cap, never hidden;
- no transient `.n148i`, `.jpg`, `.jxl`, or reconstruction file is retained.

These controls are part of the benchmark, not optional presentation details.

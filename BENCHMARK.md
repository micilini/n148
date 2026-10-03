# N.148i V2 — reproducible comparison

## Scope

The comparison uses **all 130 development images**, fixed before this run.
These images participated in development; bootstrap intervals and a fresh
execution do not turn them into an unseen validation set. Another processor
and an independently selected image sample remain future validation work.

The [frozen protocol](benchmarks/release/protocol.json) records the matrix,
source hashes, confidence method and timing design. The [corpus manifest](benchmarks/release/corpus.json)
identifies every input by SHA-256, source, author, license and conversion.
Normalized PPM inputs are included under `images/`, with attribution in
[CREDITS.txt](images/CREDITS.txt) and [CREDITS-ADDITIONAL.md](images/CREDITS-ADDITIONAL.md).

## Implementations

| Name | Implementation | Configuration |
|---|---|---|
| N.148i V1 | First public library; [source archive](benchmarks/baselines/n148i-v1-source.tar.gz) | Optimized Huffman, 4:2:0, one thread, separately compiled with `-O3` |
| N.148i V2 | Current source | Detail `0xd8a6d7ff`, effort 3, 4:2:0, equal luma/chroma quality, one thread |
| JPEG Turbo | libjpeg-turbo 2.1.5 | Baseline JPEG, optimized Huffman, 4:2:0, other library defaults |
| WebP M4 / M6 | libwebp 1.6.0 | Lossy, method 4 / 6, `thread_level=0`, default RGB→YUV conversion, `use_sharp_yuv=0` |
| WebP M6 historical | libwebp 1.3.2, recorded Ubuntu environment | Same configuration as current M6 |
| JPEG XL E3 / E7 | libjxl 0.12.0 | Lossy RGB8/sRGB, effort 3 / 7, caller-only thread runner |

V1 uses its original library, not V2's compatibility encoder. All codecs use
the same [in-memory driver](src/benchmark_final_cli.c). The [build record](benchmarks/release/build.json)
contains compiler commands, binary hashes and linkage. Full [WebP configurations](benchmarks/release/webp-configurations.json)
specify what “M6” means here; method alone does not specify every option.
[Official WebP options](https://developers.google.com/speed/webp/docs/cwebp).

On the published five-point grid, the two WebP M6 versions produced identical
file sizes and decoded PPM hashes in all 650 cases. Their shared libraries
were resolved separately, as recorded in the build metadata. This explains
the historical reference's zero quality delta; it does not establish equal
speed or identical encoded bitstreams.
[Recorded comparison](benchmarks/release/webp-quality-equivalence.json).

Quality settings: 30, 45, 60, 75 and 90 for N.148i/JPEG/WebP; JPEG XL distances
8, 5, 3, 1.5 and 0.7. Equal numbers are **not** assumed to mean equal quality.

## Quality and uncertainty

Reconstructed RGB is compared with the original PPM outside timing. Exact
definitions are in [benchmark_metrics.py](tools/benchmark_metrics.py): PSNR Y
uses `.299 R + .587 G + .114 B`; PSNR RGB pools RGB squared errors; chroma
PSNR pools both chroma differences. SSIM uses RGB, Gaussian weights, sigma
1.5 and population covariance. MS-SSIM uses `sewar`, RGB and peak 255.
SSIMULACRA2 and Butteraugli use the recorded libjxl tools; Butteraugli uses
the first score, not the second norm statistic.

Aggregate rate is total bits / total pixels. Aggregate PSNR is calculated
from pooled pixel-weighted MSE, rather than averaging decibels. Other metrics
use pixel weighting. Lower Butteraugli scores are better.

BD-rate integrates PCHIP log-rate curves over shared quality ranges only.
There is no extrapolation. Equal-quality and dominated points are handled
explicitly; insufficient overlap is missing, never a win. Negative values
mean fewer bits for the candidate. Five-point interpolation introduces
approximation error, especially for tiny differences.
The report also omits each interior point in turn and recomputes BD-rate;
this sensitivity check is separate from the sampling confidence interval.
[BD methodology](https://arxiv.org/abs/2401.04039).

Confidence intervals use **whole images**, paired across all codecs and
quality settings, resampled within content categories. Each of 10,000 seeded
bootstrap replicates recomputes the aggregate curve and BD-rate. Reports
include 95% percentile intervals and Bonferroni intervals for a seven-metric
claim against each reference, per-image values, win/loss counts and missing
overlaps. An interval containing zero is inconclusive.

Per-image quality values are deterministic; images are the resampling units.
Pixels and quality points are not treated as independent observations.
Intervals describe variation within this development sample, not correction
of tuning bias or cross-hardware uncertainty.
[Bootstrap methodology](https://pmc.ncbi.nlm.nih.gov/articles/PMC7958418/).

## Timing and matching

Timing uses one logical CPU, one codec thread, two blocks, seven repetitions
per block, two warmups and calibrated batches targeting 20 ms. Calibration
uses a preliminary call and caps batches at 256 iterations; actual batch
durations can differ as caches and clock frequency change.
The second block reverses image, group and codec order. Allocation, color
conversion and complete library calls are included; disk I/O and quality
metrics are excluded. Raw samples and CPU/load observations are retained.
Observed external QEMU/compiler interference triggers waiting or a paired-group
repeat; other jobs are never interrupted. Each group includes all eight codecs
for one image, matching mode and level within a block. Noise repeats the entire
group; completed groups at other operating points are checkpointed. A CPU tick monitor also waits if an
external process uses at least half a CPU, or external processes collectively
use at least one CPU, over an observation window of at least 0.75 seconds.
This guard does not provide exclusive CPU ownership or detect every brief job.
The recovery unit was amended after observing repeated interference, before
any timing analysis. All initial timings were excluded and the official timing
matrix was restarted. Thresholds, repetitions, order and codec binaries were
preserved. [Procedure amendment](benchmarks/release/timing-procedure-amendment.json).

Low/medium/high **nominal** settings are separate from comparable points.
Matching targets 25%, 50% and 75% of each image's common measured range across
the eight profiles: log bytes for size, linear SSIM for quality. Search stays
inside measured parameter endpoints, with at most 16 extra probes per target.
The first tested point within tolerance is accepted; otherwise the closest
tested point is retained as unmatched. Integer quality steps and
nonmonotonic curves can prevent matches.

Tolerance is 2% in bytes or 0.0005 in SSIM. Comparisons require both codecs to
meet the target tolerance **and their actual pairwise difference** to meet
the same limit. Coverage, exclusions and residuals are reported. These are
approximate matches, not exact equality.

Timing intervals resample images within categories, then blocks and
repetitions using the same draw for both codecs. Point estimates compare sums
of per-image median times. Bonferroni intervals cover six tests per reference
and matching mode: three levels × encode/decode. Measurements cover this
host only; CPU frequency was not fixed.

The compact README speed table uses one common subset: all eight profiles
must meet the medium size target and be within 2% of the actual WebP M6
size on each image. Its coverage is shown explicitly. This conditional subset
does not replace the full per-pair reports at all three levels. The common
table also records Bonferroni intervals over 14 codec/operation comparisons.

## Reproduce independently

Reference environment: Ubuntu 24.04, GCC 13.3, libjpeg-turbo 2.1.5, historical
libwebp 1.3.2. Current WebP and JPEG XL are built in isolated local folders.
Exact distribution revisions are recorded in [distribution-packages.json](benchmarks/release/distribution-packages.json).
Other environments need the same historical development libraries or a
separate, explicitly identified experiment.

```sh
sudo apt-get install build-essential cmake git pkg-config python3-venv \
  libjpeg-turbo8-dev libwebp-dev libbrotli-dev
python3 -m venv .venv
.venv/bin/pip install -r tools/requirements-benchmark.txt
python3 tools/prepare_release_benchmark.py --jobs 2

# Preserve the published measurements by using a separate output.
export N148_BENCH_OUTPUT="$PWD/.build/reproduced"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
.venv/bin/python tools/release_benchmark.py freeze
.venv/bin/python tools/release_benchmark.py quality --workers 3
.venv/bin/python tools/match_release_points.py --workers 2

# Finish other compute/build jobs before timing.
.venv/bin/python tools/time_release_benchmark.py
.venv/bin/python tools/analyze_release_benchmark.py
.venv/bin/python tools/analyze_release_timing.py
.venv/bin/python tools/plot_release_benchmark.py
.venv/bin/python -m unittest discover -s tests -v
```

Commands checkpoint completed units and refuse changed inputs, sources or
binaries within a run. Never mix experiments in one output directory. Times
vary with hardware/load; compare repeated paired results and intervals.
Scripts, normalized inputs, V1 source, protocol, build identities and raw
tables permit reproduction without the removed development history.

To audit the published evidence without installing scientific packages:

```sh
python3 tools/verify_release_evidence.py
```

This checks digests, source/input identity, every quality point and the full
timing matrix. It verifies integrity; it does not rerun the codecs or compute
new confidence intervals. Matching probes and timing/load observations are
retained in compressed JSONL audit files. The raw CSV tables remain directly
readable. `compact_release_evidence.py` creates those archives only after a
complete run and checks the compressed records before removing checkpoints.

Temporary reconstructions are deleted as measurements finish. Raw result
tables and provenance remain as evidence. Informal visual preferences from
earlier sessions are not counted as a controlled human study in these tables.

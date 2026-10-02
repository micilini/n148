# N.148i V2 release tools

Copyright (c) 2026 Micilini Roll. MIT License.

The production library is C11. Python is used for measurement and release
checks. Start with the commands and experiment definitions in
[BENCHMARK.md](../BENCHMARK.md).

| Tool | Purpose |
|---|---|
| `prepare_release_benchmark.py` | Build original V1, current V2 and pinned external libraries in local folders |
| `release_benchmark.py` | Freeze and verify sources, binaries and inputs; measure the complete quality matrix |
| `match_release_points.py` | Find actual encoder settings at common size/SSIM targets; retain probes and unmatched cases |
| `time_release_benchmark.py` | Repeat nominal and matched points on one CPU; preserve raw timing observations |
| `analyze_release_benchmark.py` | Aggregate curves, per-image BD-rate, paired image bootstrap and interpolation sensitivity |
| `analyze_release_timing.py` | Paired timing intervals, matching coverage and common-subset comparison |
| `plot_release_benchmark.py` | Export measured rate-distortion curves as a standalone SVG |
| `render_release_readme.py` | Generate README tables from completed analyses without transcribing values |
| `benchmark_metrics.py`, `benchmark_math.py` | Shared metric definitions and PCHIP integration |
| `describe_webp.c` | Record every WebP configuration field used by the comparison |
| `validate_release.py` | Check builds, codec correctness, sanitizers, ABI, installation and public API consumers |
| `audit_naming.py` | Reject obsolete implementation-stage names in active code and packaging |
| `benchmark_host.py` | Observe external CPU/build activity without interrupting other jobs |
| `compact_release_evidence.py` | Archive completed checkpoints and checksum the published evidence |
| `verify_release_evidence.py` | Verify inputs, digests and complete raw matrices with Python's standard library |

Each experiment has its own output directory. Set `N148_BENCH_OUTPUT` before
starting a reproduction; do not overwrite the published `benchmarks/release`.
Completed measurements can be resumed only with the same frozen source,
binary and input identities. Run quality/matching and statistical analysis
separately from speed measurements.

The normalized PPM inputs and their attribution are included under `images/`.
No image download or dependency on removed development history is required.
The original V1 source is retained under `benchmarks/baselines/`.

Python package versions are in `requirements-benchmark.txt`. SSIMULACRA2 and
Butteraugli are built from the same pinned libjxl source as the JPEG XL
comparison. Statistical checks with known analytical answers are in `tests/`.

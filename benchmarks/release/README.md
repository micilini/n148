# N.148i V2 comparison evidence

This directory is the measurement record for the V2 release. Scope: all 130
development images, eight codec profiles, one Intel Core 7 150U host.
It is not an unseen-corpus or cross-processor validation.

## Read the results

| File | Contents |
|---|---|
| `quality.csv` | All 5,200 measured image/codec/quality points, file sizes, decoded hashes and quality metrics |
| `quality-analysis.json` | Aggregate BD-rate, paired bootstrap intervals, per-image counts and interpolation sensitivity |
| `per-image-bd-rate.csv` | Every per-image comparison, including missing overlaps |
| `aggregate-curves.csv`, `rate-distortion.svg` | All seven measured quality curves; a figure showing three of them |
| `timing-samples.csv` | Every accepted repetition at nominal and matched points |
| `timing-analysis.json`, `per-image-timing.csv` | Paired speed comparisons, intervals, coverage and exclusions |
| `common-size-timing.json` | Same eligible image subset for all eight rows of the compact README speed table |
| `matching-audit.jsonl.gz` | Encoder-parameter probes, targets, selected points and unmatched cases |
| `timing-audit.jsonl.gz` | Accepted and rejected blocks, raw repetitions, commands, loop counts and load/frequency observations |

## Identify the experiment

- `protocol.json`: codec/input freeze, source digests, settings and stopping rule.
- `analysis-specification.json`: detailed matching, uncertainty and load-guard rules.
- `timing-procedure-amendment.json`: operating-point recovery change and complete restart before timing analysis.
- `corpus.json`: normalized input hashes, source metadata and attribution.
- `build.json`, `linked-library-sha256.json`: actual binaries and linked dependencies.
- `webp-configurations.json`, `jxl-provenance.json`: reference-library configurations and source identity.
- `webp-quality-equivalence.json`: equal sizes and reconstructions for the two measured WebP M6 versions on the five-point grid.
- `environment.json`: hardware, operating system, compilers and Python package versions.
- `distribution-packages.json`: exact Ubuntu package revisions for system references and runtimes.
- `measurement-implementation.json`: tool identities and unchanged quality helper definitions.
- `*-status.json`: completion counts and raw-table digests.
- `SHA256SUMS`: digest inventory for this directory.

The published absolute paths describe the measuring host. Reproduction tools
resolve the current checkout and create new paths; they do not require that
host's username, temporary folders or old Git history.

## Library validation

`library-validation/` records the 29 passed Linux release gates.
`library-validation/installed-examples.json` additionally records both
standalone CMake examples passing against the installed shared/static package.
`windows-build.json` records successful MinGW compilation and installed
consumer linking; native Windows execution remains pending CI. Native macOS
validation also remains pending CI.
`promotion-equivalence.json` records 650 identical streams and reconstructions
when the measured detail profile became the V2 default.

Run `python3 tools/verify_release_evidence.py` from the repository root to
verify the published digests and complete matrices. Full independent
measurement and analysis commands are in [BENCHMARK.md](../../BENCHMARK.md).
The V1 baseline source and its per-file provenance are retained separately in
[`../baselines/`](../baselines/).

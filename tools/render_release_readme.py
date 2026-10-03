#!/usr/bin/env python3
"""Render README tables directly from the completed release analyses."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from release_benchmark import ROOT, REPORT, CODECS
from analyze_release_benchmark import METRICS

LABELS = {
    "n148i_v1": "N.148i V1", "n148i_v2": "**N.148i V2**",
    "jpeg_turbo": "JPEG Turbo 2.1.5", "webp_m4": "WebP 1.6.0 M4",
    "webp_m6": "WebP 1.6.0 M6", "webp_m6_historical": "WebP 1.3.2 M6",
    "jxl_e3": "JPEG XL 0.12.0 E3", "jxl_e7": "JPEG XL 0.12.0 E7",
}
METRIC_LABELS = {
    "psnr_y_db": "PSNR Y", "psnr_rgb_db": "PSNR RGB", "ssim": "SSIM",
    "ms_ssim": "MS-SSIM", "ssimulacra2": "SSIMULACRA2",
    "butteraugli": "Butteraugli", "psnr_chroma_db": "PSNR chroma",
}


def percentage(value):
    return "N/A" if value is None else f"{value:+.2f}%"


def confidence(values):
    return "N/A" if values is None else f"[{values[0]:+.2f}, {values[1]:+.2f}]%"


def load(name):
    return json.loads((REPORT / name).read_text())


def render():
    for phase in ("quality", "matching", "timing"):
        if load(f"{phase}-status.json")["status"] != "complete":
            raise RuntimeError(f"Incomplete {phase}; refusing to publish partial results")
    quality = load("quality-analysis.json")["comparisons"]
    timing = load("timing-analysis.json")["comparisons"]
    common = load("common-size-timing.json")
    curves = REPORT / "rate-distortion.svg"
    if not curves.exists():
        raise RuntimeError("Generate the rate-distortion figure first")
    lookup = {(x["candidate"], x["anchor"], x["metric"]): x for x in quality}
    v2 = [lookup["n148i_v2", "webp_m6", metric] for metric in METRICS]
    evolution = [lookup["n148i_v2", "n148i_v1", metric] for metric in METRICS]
    wins = sum(x["conclusion"] == "lower_rate" for x in v2)
    losses = sum(x["conclusion"] == "higher_rate" for x in v2)
    unresolved = len(v2) - wins - losses
    lines = ["### From the first public V1 to V2", ""]
    if all(item["conclusion"] == "lower_rate" for item in evolution):
        savings = [-item["bd_rate_pct"] for item in evolution]
        lines += [f"V2 achieves **{min(savings):.2f}%–{max(savings):.2f}% BD-rate savings** over the",
                  "original V1 across the seven quality measures on this corpus. All seven",
                  "family-adjusted intervals favor V2. These are direct V2/V1 comparisons",
                  "over shared quality ranges.", ""]
    lines += ["<details>", "<summary>Direct V2 versus V1 results</summary>", "",
              "| Metric | V2 BD-rate relative to V1 | Family 95% interval |",
              "|---|---:|---:|"]
    for item in evolution:
        lines.append(f"| {METRIC_LABELS[item['metric']]} | {percentage(item['bd_rate_pct'])} | "
                     f"{confidence(item['familywise95_pct'])} |")
    lines += ["", "</details>", "",
        "### Compression at equivalent measured quality", "",
        "**5,200 measured points · 130 images · 8 profiles · 7 metrics.**", "",
        "BD-rate relative to **WebP 1.6.0 M6**. Negative means fewer bits over",
        "the overlapping quality range; positive means more bits. These are",
        "aggregate curves, not a promise that every image improves.", "",
        "| Codec | PSNR Y | PSNR RGB | SSIM | MS-SSIM | SSIMULACRA2 | Butteraugli | Chroma |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for codec in CODECS:
        cells = ["0.00%" if codec == "webp_m6" else percentage(lookup[codec, "webp_m6", metric]["bd_rate_pct"])
                 for metric in METRICS]
        lines.append("| " + LABELS[codec] + " | " + " | ".join(cells) + " |")
    lines += ["", "![Measured rate-distortion curves](benchmarks/release/rate-distortion.svg)", "",
              "### How certain are V2's differences from WebP M6?", "",
              "10,000 paired image bootstrap replicates, stratified by content.",
              "The family intervals account for the seven quality comparisons.",
              "An interval crossing zero does not establish a gain or a loss.", "",
              "| Metric | V2 BD-rate | 95% interval | Family 95% interval | Interior-point omission range | Images won / tied / lost |",
              "|---|---:|---:|---:|---:|---:|"]
    for item in v2:
        sensitivity = [x["bd_rate_pct"] for x in item["interior_point_omission"] if x["bd_rate_pct"] is not None]
        span = confidence([min(sensitivity), max(sensitivity)]) if len(sensitivity) == 3 else "Incomplete overlap"
        lines.append(f"| {METRIC_LABELS[item['metric']]} | {percentage(item['bd_rate_pct'])} | "
                     f"{confidence(item['ci95_pct'])} | {confidence(item['familywise95_pct'])} | "
                     f"{span} | "
                     f"{item['images_won']} / {item['images_tied']} / {item['images_lost']} "
                     f"({item['images_valid']}/130 valid) |")
    lines += ["", f"For this development sample, the family intervals support **{wins}/7 lower-rate",
              f"results, {losses}/7 higher-rate results and {unresolved}/7 inconclusive results** against",
              "this WebP M6 configuration. Per-image wins use the point estimates;",
              "they are not separate significance tests. Sampling intervals do not",
              "include codec tuning bias or five-point interpolation error. The omission",
              "range recomputes BD-rate after dropping each interior point in turn; it",
              "is a sensitivity diagnostic, not a confidence interval. The",
              "[full quality analysis](benchmarks/release/quality-analysis.json) records",
              "missing overlaps and sensitivity to omitting each interior curve point.", "",
              "### Speed at comparable file sizes", "",
              "One logical CPU, one codec thread; 2 blocks × 7 measured repetitions.",
              f"The same **{common['count']}/130 images** appear in every row below: all eight",
              "profiles meet the medium size target and differ from WebP M6's actual",
              "file size by at most 2% per image. This is a conditional subset.", "",
              "Times are averages of per-image medians; negative time differences",
              "mean faster than WebP 1.6.0 M6. Brackets show 95% paired intervals.", "",
              "| Codec | Encode ms/image | Encode Δ [95% interval] | Decode ms/image | Decode Δ [95% interval] |",
              "|---|---:|---:|---:|---:|"]
    if common["count"]:
        for item in common["codecs"]:
            enc, dec = item["encode_ms"], item["decode_ms"]
            lines.append(f"| {LABELS[item['codec']]} | {enc['mean_ms']:.3f} | "
                         f"{percentage(enc['time_difference_pct'])} {confidence(enc['ci95_pct'])} | "
                         f"{dec['mean_ms']:.3f} | {percentage(dec['time_difference_pct'])} {confidence(dec['ci95_pct'])} |")
    else:
        lines.append("| No common matched subset | — | — | — | — |")
    lines += ["", "[Common-subset details](benchmarks/release/common-size-timing.json) include",
              "MPix/s, image identifiers, bytes and intervals adjusted for 14 comparisons.", "",
              "#### V2 versus WebP M6 across operating ranges", "",
              "Each row uses all eligible pairs for that target, with both target and",
              "actual pairwise tolerances enforced. Size tolerance is 2%; SSIM tolerance",
              "is 0.0005. These intervals account for six tests per matching mode.", "",
              "| Matched by | Level | Images | Encode Δ [family 95% interval] | Decode Δ [family 95% interval] |",
              "|---|---|---:|---:|---:|"]
    for mode in ("bytes", "ssim"):
        for level in ("low", "medium", "high"):
            entries = {x["operation"]: x for x in timing if x["anchor"] == "webp_m6" and
                       x["mode"] == mode and x["level"] == level}
            enc, dec = entries["encode_ms"], entries["decode_ms"]
            cells = []
            for entry in (enc, dec):
                cells.append(percentage(entry.get("time_difference_pct")) + " " + confidence(entry.get("familywise95_pct")))
            lines.append(f"| {'File size' if mode == 'bytes' else 'SSIM'} | {level.title()} | "
                         f"{enc['paired_images']}/130 | " + " | ".join(cells) + " |")
    lines += ["", "Nominal-quality timings, every external reference, exclusions and raw",
              "observations are retained in the [timing report](benchmarks/release/timing-analysis.json)",
              "and [raw samples](benchmarks/release/timing-samples.csv). Measurements cover",
              "this CPU only, with clock frequency managed by the operating system."]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--readme", type=Path, default=ROOT / "README.md")
    args = parser.parse_args()
    before, rest = args.readme.read_text().split("<!-- RELEASE_RESULTS_BEGIN -->", 1)
    _, after = rest.split("<!-- RELEASE_RESULTS_END -->", 1)
    args.readme.write_text(before + "<!-- RELEASE_RESULTS_BEGIN -->\n" + render() +
                           "<!-- RELEASE_RESULTS_END -->" + after)
    print(args.readme)


if __name__ == "__main__": main()

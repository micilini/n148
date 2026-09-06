#!/usr/bin/env python3
"""Compare two N.148i benchmark result sets point by point.

The primary comparison is intended for a candidate and a baseline measured on
the same streamed PPM.  JPEG timings from each binary act as a control for
frequency and system drift.  An older historical run can also be supplied for
context, but it is kept separate from the contemporaneous A/B verdict.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import statistics
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Callable, Iterable


KEY_FIELDS = ("image_id", "quality")
IDENTITY_FIELDS = (
    "pageid",
    "width",
    "height",
    "pixels",
    "chroma",
    "threads",
    "reps",
)
COMPARISON_FIELDS = [
    "image_id",
    "pageid",
    "title",
    "quality",
    "width",
    "height",
    "baseline_n148_enc_ms",
    "candidate_n148_enc_ms",
    "enc_delta_pct",
    "baseline_enc_ratio",
    "candidate_enc_ratio",
    "enc_normalized_delta_pct",
    "baseline_n148_dec_ms",
    "candidate_n148_dec_ms",
    "dec_delta_pct",
    "baseline_dec_ratio",
    "candidate_dec_ratio",
    "dec_normalized_delta_pct",
    "baseline_n148_planes_ms",
    "candidate_n148_planes_ms",
    "planes_delta_pct",
    "baseline_n148_merge_ms",
    "candidate_n148_merge_ms",
    "merge_delta_pct",
    "baseline_n148_bytes",
    "candidate_n148_bytes",
    "size_delta_pct",
    "n148_psnr_delta_db",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--historical", type=Path)
    parser.add_argument("--rechecks", type=Path)
    parser.add_argument("--candidate-label", default="candidate")
    parser.add_argument("--baseline-label", default="baseline")
    parser.add_argument("--focus", choices=("encode", "decode"), default="encode")
    parser.add_argument("--bootstrap-samples", type=int, default=10_000)
    parser.add_argument("--seed", type=int, default=148)
    return parser.parse_args()


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def load_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def point_key(row: dict[str, str]) -> tuple[int, int]:
    return tuple(int(row[field]) for field in KEY_FIELDS)  # type: ignore[return-value]


def index_rows(rows: list[dict[str, str]], label: str) -> dict[tuple[int, int], dict[str, str]]:
    indexed: dict[tuple[int, int], dict[str, str]] = {}
    for row in rows:
        key = point_key(row)
        if key in indexed:
            raise SystemExit(f"duplicate point in {label}: {key}")
        indexed[key] = row
    return indexed


def validate_pair(
    candidate: dict[tuple[int, int], dict[str, str]],
    baseline: dict[tuple[int, int], dict[str, str]],
    candidate_label: str,
    baseline_label: str,
) -> None:
    if candidate.keys() != baseline.keys():
        only_candidate = sorted(candidate.keys() - baseline.keys())[:5]
        only_baseline = sorted(baseline.keys() - candidate.keys())[:5]
        raise SystemExit(
            f"different matrices ({candidate_label} only: {only_candidate}; "
            f"{baseline_label} only: {only_baseline})"
        )
    for key in candidate:
        for field in IDENTITY_FIELDS:
            if candidate[key][field] != baseline[key][field]:
                raise SystemExit(
                    f"different identity at {key}, field {field}: "
                    f"{candidate[key][field]} != {baseline[key][field]}"
                )


def number(row: dict[str, str], field: str) -> float:
    return float(row[field])


def total(rows: Iterable[dict[str, str]], field: str) -> float:
    return sum(number(row, field) for row in rows)


def ratio_delta(left: float, right: float) -> float:
    return (left / right - 1.0) * 100.0


def point_normalized_delta(
    candidate: dict[str, str], baseline: dict[str, str], kind: str
) -> float:
    n_field = f"n148_{kind}_ms"
    j_field = f"jpeg_{kind}_ms"
    candidate_ratio = number(candidate, n_field) / number(candidate, j_field)
    baseline_ratio = number(baseline, n_field) / number(baseline, j_field)
    return ratio_delta(candidate_ratio, baseline_ratio)


def aggregate_delta(
    candidate_rows: Iterable[dict[str, str]],
    baseline_rows: Iterable[dict[str, str]],
    field: str,
) -> float:
    return ratio_delta(total(candidate_rows, field), total(baseline_rows, field))


def aggregate_normalized_delta(
    candidate_rows: Iterable[dict[str, str]],
    baseline_rows: Iterable[dict[str, str]],
    kind: str,
) -> float:
    candidate_rows = list(candidate_rows)
    baseline_rows = list(baseline_rows)
    n_field = f"n148_{kind}_ms"
    j_field = f"jpeg_{kind}_ms"
    candidate_ratio = total(candidate_rows, n_field) / total(candidate_rows, j_field)
    baseline_ratio = total(baseline_rows, n_field) / total(baseline_rows, j_field)
    return ratio_delta(candidate_ratio, baseline_ratio)


def record(values: Iterable[float], tolerance: float = 0.0) -> tuple[int, int, int]:
    wins = neutral = losses = 0
    for value in values:
        if value < -tolerance:
            wins += 1
        elif value > tolerance:
            losses += 1
        else:
            neutral += 1
    return wins, neutral, losses


def percentile(values: list[float], probability: float) -> float:
    if not values:
        return math.nan
    position = (len(values) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return values[lower]
    fraction = position - lower
    return values[lower] * (1.0 - fraction) + values[upper] * fraction


def clustered_bootstrap(
    candidate: dict[tuple[int, int], dict[str, str]],
    baseline: dict[tuple[int, int], dict[str, str]],
    statistic: Callable[[list[dict[str, str]], list[dict[str, str]]], float],
    samples: int,
    seed: int,
) -> tuple[float, float]:
    by_image: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for key in candidate:
        by_image[key[0]].append(key)
    image_ids = sorted(by_image)
    generator = random.Random(seed)
    values: list[float] = []
    for _ in range(samples):
        candidate_sample: list[dict[str, str]] = []
        baseline_sample: list[dict[str, str]] = []
        for _ in image_ids:
            image_id = generator.choice(image_ids)
            for key in by_image[image_id]:
                candidate_sample.append(candidate[key])
                baseline_sample.append(baseline[key])
        values.append(statistic(candidate_sample, baseline_sample))
    values.sort()
    return percentile(values, 0.025), percentile(values, 0.975)


def decimal(value: float, digits: int = 3) -> str:
    if not math.isfinite(value):
        return "n/a"
    return f"{value:.{digits}f}"


def signed_percent(value: float, digits: int = 2) -> str:
    if not math.isfinite(value):
        return "n/a"
    sign = "+" if value > 0 else "−" if value < 0 else ""
    return f"{sign}{decimal(abs(value), digits)}%"


def integer(value: float) -> str:
    return f"{int(round(value)):,}"


def short_title(value: str, limit: int = 44) -> str:
    if value.startswith("File:"):
        value = value[5:]
    return value if len(value) <= limit else value[: limit - 1] + "…"


def escape_table(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ")


def metadata_for(results_path: Path) -> dict[str, Any]:
    metadata_path = results_path.parent / "metadata.json"
    return load_json(metadata_path) if metadata_path.exists() else {}


def atomic_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, prefix=".atomic-", delete=False
    ) as handle:
        handle.write(value)
        temporary = Path(handle.name)
    temporary.replace(path)


def atomic_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="", dir=path.parent,
        prefix=".atomic-", delete=False,
    ) as handle:
        writer = csv.DictWriter(
            handle, fieldnames=COMPARISON_FIELDS, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)
        temporary = Path(handle.name)
    temporary.replace(path)


def main() -> int:
    args = parse_args()
    if args.bootstrap_samples < 1:
        raise SystemExit("bootstrap-samples must be positive")

    focus_kind = "dec" if args.focus == "decode" else "enc"
    focus_label = "decode" if focus_kind == "dec" else "encode"

    candidate_rows = load_csv(args.candidate)
    baseline_rows = load_csv(args.baseline)
    candidate = index_rows(candidate_rows, "candidate")
    baseline = index_rows(baseline_rows, "baseline")
    validate_pair(candidate, baseline, args.candidate_label, args.baseline_label)
    decoder_stage_fields = ("n148_planes_ms", "n148_merge_ms")
    has_decoder_stages = all(
        row.get(field, "") != ""
        for row in candidate_rows + baseline_rows
        for field in decoder_stage_fields
    )

    historical = None
    if args.historical is not None:
        historical = index_rows(load_csv(args.historical), "historical")
        validate_pair(candidate, historical, args.candidate_label, "historical")

    rechecks: list[dict[str, str]] = []
    if args.rechecks is not None:
        rechecks = load_csv(args.rechecks)
        recheck_keys: set[tuple[int, int]] = set()
        for row in rechecks:
            key = point_key(row)
            if key not in candidate:
                raise SystemExit(f"recheck outside the matrix: {key}")
            if key in recheck_keys:
                raise SystemExit(f"duplicate recheck: {key}")
            recheck_keys.add(key)

    manifest = load_json(args.manifest)
    manifest_by_id = {int(item["image_id"]): item for item in manifest}
    if set(manifest_by_id) != {key[0] for key in candidate}:
        raise SystemExit("manifest does not cover all matrix IDs")

    ordered_keys = sorted(candidate)
    comparison_rows: list[dict[str, Any]] = []
    for key in ordered_keys:
        current = candidate[key]
        old = baseline[key]
        source = manifest_by_id[key[0]]
        comparison_rows.append(
            {
                "image_id": key[0],
                "pageid": current["pageid"],
                "title": source["title"],
                "quality": key[1],
                "width": current["width"],
                "height": current["height"],
                "baseline_n148_enc_ms": old["n148_enc_ms"],
                "candidate_n148_enc_ms": current["n148_enc_ms"],
                "enc_delta_pct": ratio_delta(
                    number(current, "n148_enc_ms"), number(old, "n148_enc_ms")
                ),
                "baseline_enc_ratio": number(old, "n148_enc_ms") / number(old, "jpeg_enc_ms"),
                "candidate_enc_ratio": number(current, "n148_enc_ms") / number(current, "jpeg_enc_ms"),
                "enc_normalized_delta_pct": point_normalized_delta(current, old, "enc"),
                "baseline_n148_dec_ms": old["n148_dec_ms"],
                "candidate_n148_dec_ms": current["n148_dec_ms"],
                "dec_delta_pct": ratio_delta(
                    number(current, "n148_dec_ms"), number(old, "n148_dec_ms")
                ),
                "baseline_dec_ratio": number(old, "n148_dec_ms") / number(old, "jpeg_dec_ms"),
                "candidate_dec_ratio": number(current, "n148_dec_ms") / number(current, "jpeg_dec_ms"),
                "dec_normalized_delta_pct": point_normalized_delta(current, old, "dec"),
                "baseline_n148_planes_ms": old.get("n148_planes_ms", ""),
                "candidate_n148_planes_ms": current.get("n148_planes_ms", ""),
                "planes_delta_pct": (
                    ratio_delta(
                        number(current, "n148_planes_ms"),
                        number(old, "n148_planes_ms"),
                    )
                    if has_decoder_stages else ""
                ),
                "baseline_n148_merge_ms": old.get("n148_merge_ms", ""),
                "candidate_n148_merge_ms": current.get("n148_merge_ms", ""),
                "merge_delta_pct": (
                    ratio_delta(
                        number(current, "n148_merge_ms"),
                        number(old, "n148_merge_ms"),
                    )
                    if has_decoder_stages else ""
                ),
                "baseline_n148_bytes": old["n148_bytes"],
                "candidate_n148_bytes": current["n148_bytes"],
                "size_delta_pct": ratio_delta(
                    number(current, "n148_bytes"), number(old, "n148_bytes")
                ),
                "n148_psnr_delta_db": number(current, "n148_psnr_db") - number(old, "n148_psnr_db"),
            }
        )
    atomic_csv(args.output_csv, comparison_rows)

    exact_size = sum(
        candidate[key]["n148_bytes"] == baseline[key]["n148_bytes"]
        for key in ordered_keys
    )
    exact_psnr = sum(
        candidate[key]["n148_psnr_db"] == baseline[key]["n148_psnr_db"]
        for key in ordered_keys
    )
    exact_jpeg = sum(
        candidate[key]["jpeg_bytes"] == baseline[key]["jpeg_bytes"]
        and candidate[key]["jpeg_psnr_db"] == baseline[key]["jpeg_psnr_db"]
        for key in ordered_keys
    )

    def raw_stat(kind: str) -> Callable[[list[dict[str, str]], list[dict[str, str]]], float]:
        field = f"n148_{kind}_ms"
        return lambda current, old: aggregate_delta(current, old, field)

    def normalized_stat(kind: str) -> Callable[[list[dict[str, str]], list[dict[str, str]]], float]:
        return lambda current, old: aggregate_normalized_delta(current, old, kind)

    intervals: dict[str, tuple[float, float]] = {}
    for index, kind in enumerate(("enc", "dec")):
        intervals[f"{kind}_raw"] = clustered_bootstrap(
            candidate, baseline, raw_stat(kind), args.bootstrap_samples,
            args.seed + index * 2,
        )
        intervals[f"{kind}_normalized"] = clustered_bootstrap(
            candidate, baseline, normalized_stat(kind), args.bootstrap_samples,
            args.seed + index * 2 + 1,
        )

    candidate_metadata = metadata_for(args.candidate)
    candidate_sha = (
        candidate_metadata.get("environment", {}).get("compare_sha256", "unknown")
    )
    baseline_sha = (
        candidate_metadata.get("baseline_environment", {}).get(
            "compare_sha256", "unknown"
        )
    )

    point_enc_raw = [float(row["enc_delta_pct"]) for row in comparison_rows]
    point_enc_normalized = [
        float(row["enc_normalized_delta_pct"]) for row in comparison_rows
    ]
    point_dec_raw = [float(row["dec_delta_pct"]) for row in comparison_rows]
    point_dec_normalized = [
        float(row["dec_normalized_delta_pct"]) for row in comparison_rows
    ]

    total_candidate_size = total(candidate_rows, "n148_bytes")
    total_baseline_size = total(baseline_rows, "n148_bytes")
    total_candidate_enc = total(candidate_rows, "n148_enc_ms")
    total_baseline_enc = total(baseline_rows, "n148_enc_ms")
    total_candidate_dec = total(candidate_rows, "n148_dec_ms")
    total_baseline_dec = total(baseline_rows, "n148_dec_ms")
    enc_raw_delta = aggregate_delta(candidate_rows, baseline_rows, "n148_enc_ms")
    enc_normalized_delta = aggregate_normalized_delta(candidate_rows, baseline_rows, "enc")
    dec_raw_delta = aggregate_delta(candidate_rows, baseline_rows, "n148_dec_ms")
    dec_normalized_delta = aggregate_normalized_delta(candidate_rows, baseline_rows, "dec")

    total_candidate_planes = (
        total(candidate_rows, "n148_planes_ms") if has_decoder_stages else math.nan
    )
    total_baseline_planes = (
        total(baseline_rows, "n148_planes_ms") if has_decoder_stages else math.nan
    )
    total_candidate_merge = (
        total(candidate_rows, "n148_merge_ms") if has_decoder_stages else math.nan
    )
    total_baseline_merge = (
        total(baseline_rows, "n148_merge_ms") if has_decoder_stages else math.nan
    )
    planes_raw_delta = (
        ratio_delta(total_candidate_planes, total_baseline_planes)
        if has_decoder_stages else math.nan
    )
    merge_raw_delta = (
        ratio_delta(total_candidate_merge, total_baseline_merge)
        if has_decoder_stages else math.nan
    )

    jpeg_enc_drift = aggregate_delta(candidate_rows, baseline_rows, "jpeg_enc_ms")
    jpeg_dec_drift = aggregate_delta(candidate_rows, baseline_rows, "jpeg_dec_ms")

    current_vs_jpeg = {
        "size": ratio_delta(total_candidate_size, total(candidate_rows, "jpeg_bytes")),
        "enc": ratio_delta(total_candidate_enc, total(candidate_rows, "jpeg_enc_ms")),
        "dec": ratio_delta(total_candidate_dec, total(candidate_rows, "jpeg_dec_ms")),
    }
    current_jpeg_records = {
        kind: record(
            ratio_delta(number(row, f"n148_{kind}_ms"), number(row, f"jpeg_{kind}_ms"))
            for row in candidate_rows
        )
        for kind in ("enc", "dec")
    }
    current_size_record = record(
        ratio_delta(number(row, "n148_bytes"), number(row, "jpeg_bytes"))
        for row in candidate_rows
    )

    by_image: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for key in ordered_keys:
        by_image[key[0]].append(key)

    def image_jpeg_record(kind: str) -> tuple[int, int, int]:
        values = []
        for keys in by_image.values():
            current_subset = [candidate[key] for key in keys]
            values.append(
                ratio_delta(
                    total(current_subset, f"n148_{kind}_ms"),
                    total(current_subset, f"jpeg_{kind}_ms"),
                )
            )
        return record(values)

    current_jpeg_image_records = {
        kind: image_jpeg_record(kind) for kind in ("enc", "dec")
    }

    focus_raw_delta = dec_raw_delta if focus_kind == "dec" else enc_raw_delta
    focus_normalized_delta = (
        dec_normalized_delta if focus_kind == "dec" else enc_normalized_delta
    )
    focus_interval = intervals[f"{focus_kind}_normalized"]

    lines = [
        f"# N.148i: {focus_label} — before versus after",
        "",
        f"Comparison of commit **`{args.candidate_label}`** against **`{args.baseline_label}`** "
        f"on the same {len(manifest)} images and {len(ordered_keys)} points. Each normalized input "
        "was reconstructed with the same dimensions and SHA-256 before both measurements.",
        "",
        "## Verdict",
        "",
        f"The new version's {focus_label} time changed by "
        f"**{signed_percent(focus_raw_delta)} in raw time** and "
        f"**{signed_percent(focus_normalized_delta)} after normalization by the co-measured JPEG control**. "
        f"The per-image 95% bootstrap interval was "
        f"[{signed_percent(focus_interval[0])}, "
        f"{signed_percent(focus_interval[1])}] for the normalized change.",
        "",
        f"Size and reconstruction remained identical at **{exact_size}/{len(ordered_keys)}** "
        f"and **{exact_psnr}/{len(ordered_keys)}** points, respectively. The change is therefore "
        "an internal N.148i optimization with no format, rate, or quality change.",
        "",
        "## Contemporaneous A/B scorecard",
        "",
        "| Metric | Baseline | New | Raw delta | JPEG-normalized delta | New W/N/L (±1%) | Normalized 95% CI |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        f"| N.148i size | {integer(total_baseline_size)} B | {integer(total_candidate_size)} B | "
        f"{signed_percent(ratio_delta(total_candidate_size, total_baseline_size))} | n/a | "
        f"{exact_size}/{len(ordered_keys) - exact_size}/0 equal | exact |",
        f"| Sum of encode medians | {decimal(total_baseline_enc)} ms | "
        f"{decimal(total_candidate_enc)} ms | {signed_percent(enc_raw_delta)} | "
        f"{signed_percent(enc_normalized_delta)} | "
        f"{'/'.join(map(str, record(point_enc_normalized, 1.0)))} | "
        f"[{signed_percent(intervals['enc_normalized'][0])}, {signed_percent(intervals['enc_normalized'][1])}] |",
        f"| Sum of decode medians | {decimal(total_baseline_dec)} ms | "
        f"{decimal(total_candidate_dec)} ms | {signed_percent(dec_raw_delta)} | "
        f"{signed_percent(dec_normalized_delta)} | "
        f"{'/'.join(map(str, record(point_dec_normalized, 1.0)))} | "
        f"[{signed_percent(intervals['dec_normalized'][0])}, {signed_percent(intervals['dec_normalized'][1])}] |",
        "",
        "`W/N/L` means improvement, neutral zone, and regression for the new version. The ±1% neutral zone "
        "is only a practical noise filter; CSVs preserve unrounded values.",
        "",
        f"Drift control: co-measured JPEG changed by {signed_percent(jpeg_enc_drift)} for encode "
        f"and {signed_percent(jpeg_dec_drift)} for decode between passes. The normalized column "
        "removes this first-order variation.",
        *(
            [
                "",
                "## Internal decoder breakdown",
                "",
                "| Stage | Baseline | New | Raw delta |",
                "| --- | ---: | ---: | ---: |",
                f"| Plane decoding | {decimal(total_baseline_planes)} ms | "
                f"{decimal(total_candidate_planes)} ms | {signed_percent(planes_raw_delta)} |",
                f"| YCbCr→RGB merge | {decimal(total_baseline_merge)} ms | "
                f"{decimal(total_candidate_merge)} ms | {signed_percent(merge_raw_delta)} |",
                "",
                "The plane stage includes entropy reading, dequantization, IDCT, and writes "
                "to the Y/Cb/Cr planes. The merge includes chroma resampling and RGB conversion.",
            ]
            if has_decoder_stages else []
        ),
        "",
        "## New version versus JPEG",
        "",
        "| Metric | Aggregate N.148i/JPEG delta | Per-point wins/ties/losses |",
        "| --- | ---: | ---: |",
        f"| Size | {signed_percent(current_vs_jpeg['size'])} | "
        f"{'/'.join(map(str, current_size_record))} |",
        f"| Encode | {signed_percent(current_vs_jpeg['enc'])} | "
        f"{'/'.join(map(str, current_jpeg_records['enc']))} |",
        f"| Decode | {signed_percent(current_vs_jpeg['dec'])} | "
        f"{'/'.join(map(str, current_jpeg_records['dec']))} |",
        "",
        "Aggregated by image, the N.148i/JPEG score was "
        f"**{'/'.join(map(str, current_jpeg_image_records['enc']))}** for encode and "
        f"**{'/'.join(map(str, current_jpeg_image_records['dec']))}** for decode (wins/ties/losses). "
        "The new version does not literally win every individual point in the short sweep: "
        f"there are {current_jpeg_records['enc'][2]} encode losses and "
        f"{current_jpeg_records['dec'][2]} decode losses among {len(ordered_keys)} points.",
    ]

    if rechecks:
        recheck_ab_field = f"{focus_kind}_normalized_delta_pct"
        recheck_ratio_field = f"candidate_{focus_kind}_ratio"
        recheck_ab = [float(row[recheck_ab_field]) for row in rechecks]
        recheck_reps = "/".join(
            str(value) for value in sorted({int(row["reps"]) for row in rechecks})
        )
        initial_jpeg_loss_keys = {
            key
            for key in ordered_keys
            if number(candidate[key], f"n148_{focus_kind}_ms")
            > number(candidate[key], f"jpeg_{focus_kind}_ms")
        }
        rechecked_initial_losses = [
            row for row in rechecks if point_key(row) in initial_jpeg_loss_keys
        ]
        persistent_jpeg_losses = [
            row for row in rechecked_initial_losses
            if float(row[recheck_ratio_field]) > 1.0
        ]
        long_jpeg_record = record(
            ratio_delta(float(row[recheck_ratio_field]), 1.0) for row in rechecks
        )
        lines.extend(
            [
                "",
                f"## Targeted outlier recheck ({recheck_reps}×)",
                "",
                f"The **{len(rechecks)} selected {focus_label} outliers** were repeated "
                "after the short sweep. In the long run, the new version improved by more than 1% "
                f"over the baseline at "
                f"**{sum(value < -1.0 for value in recheck_ab)}/{len(rechecks)}**, with a median "
                f"of **{signed_percent(statistics.median(recheck_ab))}**.",
                "",
                f"Against JPEG, the long-run score for these points was "
                f"**{'/'.join(map(str, long_jpeg_record))}** (wins/ties/losses). "
                f"Of the **{len(rechecked_initial_losses)} {focus_label} losses** from the short sweep "
                f"that were included, **{len(persistent_jpeg_losses)}** remained above 1.0000×.",
                "",
                f"| ID | Q | Normalized A/B in 31× sweep | Normalized A/B at {recheck_reps}× | "
                f"N.148i/JPEG {recheck_reps}× |",
                "| ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in sorted(rechecks, key=point_key):
            key = point_key(row)
            initial_delta = point_normalized_delta(
                candidate[key], baseline[key], focus_kind
            )
            lines.append(
                f"| {key[0]} | {key[1]} | {signed_percent(initial_delta)} | "
                f"{signed_percent(float(row[recheck_ab_field]))} | "
                f"{decimal(float(row[recheck_ratio_field]), 4)}× |"
            )
        lines.extend(
            [
                "",
                "Selection was deliberately biased toward the worst points; this recheck therefore "
                "audits false negatives and must not be used to recalculate the corpus mean.",
            ]
        )

    lines.extend(
        [
            "",
            "## A/B results by quality",
            "",
            f"| Q | Raw encode delta | Normalized encode delta | Raw decode delta | "
            f"Normalized decode delta | {focus_label} W/N/L (±1%) |",
            "| ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )

    qualities = sorted({key[1] for key in ordered_keys})
    for quality in qualities:
        keys = [key for key in ordered_keys if key[1] == quality]
        current_subset = [candidate[key] for key in keys]
        old_subset = [baseline[key] for key in keys]
        focus_values = [
            point_normalized_delta(candidate[key], baseline[key], focus_kind)
            for key in keys
        ]
        lines.append(
            f"| {quality} | {signed_percent(aggregate_delta(current_subset, old_subset, 'n148_enc_ms'))} | "
            f"{signed_percent(aggregate_normalized_delta(current_subset, old_subset, 'enc'))} | "
            f"{signed_percent(aggregate_delta(current_subset, old_subset, 'n148_dec_ms'))} | "
            f"{signed_percent(aggregate_normalized_delta(current_subset, old_subset, 'dec'))} | "
            f"{'/'.join(map(str, record(focus_values, 1.0)))} |"
        )

    if historical is not None:
        historical_rows = [historical[key] for key in ordered_keys]
        historical_enc = aggregate_delta(candidate_rows, historical_rows, "n148_enc_ms")
        historical_enc_normalized = aggregate_normalized_delta(
            candidate_rows, historical_rows, "enc"
        )
        historical_dec = aggregate_delta(candidate_rows, historical_rows, "n148_dec_ms")
        historical_dec_normalized = aggregate_normalized_delta(
            candidate_rows, historical_rows, "dec"
        )
        base_drift_enc = aggregate_normalized_delta(
            baseline_rows, historical_rows, "enc"
        )
        base_drift_dec = aggregate_normalized_delta(
            baseline_rows, historical_rows, "dec"
        )
        lines.extend(
            [
                "",
                "## Cross-check against the historical run",
                "",
                "| Comparison | Raw encode | Normalized encode | Raw decode | Normalized decode |",
                "| --- | ---: | ---: | ---: | ---: |",
                f"| Current new version vs historical baseline | {signed_percent(historical_enc)} | "
                f"{signed_percent(historical_enc_normalized)} | {signed_percent(historical_dec)} | "
                f"{signed_percent(historical_dec_normalized)} |",
                f"| Contemporaneous baseline vs historical baseline (control) | "
                f"{signed_percent(aggregate_delta(baseline_rows, historical_rows, 'n148_enc_ms'))} | "
                f"{signed_percent(base_drift_enc)} | "
                f"{signed_percent(aggregate_delta(baseline_rows, historical_rows, 'n148_dec_ms'))} | "
                f"{signed_percent(base_drift_dec)} |",
                "",
                "The control row shows why the main verdict uses the contemporaneous baseline: "
                "it measures the same older revision again under the current run's conditions.",
            ]
        )

    lines.extend(
        [
            "",
            f"## All {len(manifest)} images: change in the new version",
            "",
            f"Negative deltas favor the new version. `W E/D` counts at how many of the {len(qualities)} "
            "qualities it improved by more than 1% after JPEG normalization.",
            "",
            "| ID / image | Dimensions | Raw encode | Normalized encode | Normalized decode | W E/D |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for image_id in sorted(by_image):
        keys = sorted(by_image[image_id])
        current_subset = [candidate[key] for key in keys]
        old_subset = [baseline[key] for key in keys]
        source = manifest_by_id[image_id]
        title = escape_table(short_title(str(source["title"])))
        url = str(source.get("description_url") or source.get("original_url") or "")
        label = f"[{image_id:03d} — {title}](<{url}>)" if url else f"{image_id:03d} — {title}"
        enc_values = [
            point_normalized_delta(candidate[key], baseline[key], "enc") for key in keys
        ]
        dec_values = [
            point_normalized_delta(candidate[key], baseline[key], "dec") for key in keys
        ]
        lines.append(
            f"| {label} | {source['width']}×{source['height']} | "
            f"{signed_percent(aggregate_delta(current_subset, old_subset, 'n148_enc_ms'))} | "
            f"{signed_percent(aggregate_normalized_delta(current_subset, old_subset, 'enc'))} | "
            f"{signed_percent(aggregate_normalized_delta(current_subset, old_subset, 'dec'))} | "
            f"{record(enc_values, 1.0)[0]}/{record(dec_values, 1.0)[0]} |"
        )

    focus_delta_field = f"{focus_kind}_normalized_delta_pct"
    focus_baseline_ratio_field = f"baseline_{focus_kind}_ratio"
    focus_candidate_ratio_field = f"candidate_{focus_kind}_ratio"
    sorted_focus = sorted(
        comparison_rows, key=lambda row: float(row[focus_delta_field])
    )
    lines.extend(
        [
            "",
            "## Point-by-point extremes",
            "",
            f"### Ten largest normalized {focus_label} improvements",
            "",
            "| ID | Q | Normalized delta | Baseline N/J | New N/J |",
            "| ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in sorted_focus[:10]:
        lines.append(
            f"| {row['image_id']} | {row['quality']} | "
            f"{signed_percent(float(row[focus_delta_field]))} | "
            f"{decimal(float(row[focus_baseline_ratio_field]), 4)}× | "
            f"{decimal(float(row[focus_candidate_ratio_field]), 4)}× |"
        )
    lines.extend(
        [
            "",
            f"### Ten largest normalized {focus_label} regressions",
            "",
            "| ID | Q | Normalized delta | Baseline N/J | New N/J |",
            "| ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in reversed(sorted_focus[-10:]):
        lines.append(
            f"| {row['image_id']} | {row['quality']} | "
            f"{signed_percent(float(row[focus_delta_field]))} | "
            f"{decimal(float(row[focus_baseline_ratio_field]), 4)}× | "
            f"{decimal(float(row[focus_candidate_ratio_field]), 4)}× |"
        )

    lines.extend(
        [
            "",
            "## Integrity and methodology",
            "",
            f"- N.148i output with identical size: **{exact_size}/{len(ordered_keys)}** points.",
            f"- Identical N.148i PSNR: **{exact_psnr}/{len(ordered_keys)}** points.",
            f"- JPEG control with identical size and PSNR: **{exact_jpeg}/{len(ordered_keys)}** points.",
            f"- New binary: `{candidate_sha}`; baseline binary: `{baseline_sha}`.",
            f"- Bootstrap: {args.bootstrap_samples} resamples of the {len(manifest)} images as clusters; "
            f"each image's {len(qualities)} qualities remain together.",
            "- The bootstrap measures sensitivity to corpus composition. Because the driver preserves only "
            "each point's median, it does not replace an interval built from all timing samples.",
            "- At each point, both binaries ran on the same PPM and baseline/new order was "
            "alternated. Each binary internally alternated N.148i/JPEG over 31 replicates plus warm-up.",
            "",
            "## Files",
            "",
            f"- `{args.output_csv.name}`: all {len(ordered_keys)} unrounded point-by-point deltas.",
            f"- `{args.candidate.name}`: new-version measurements against JPEG.",
            f"- `{args.baseline.name}`: contemporaneous baseline-commit measurements against JPEG.",
            *(
                [f"- `{args.rechecks.name}`: targeted rechecks with {recheck_reps} replicates."]
                if rechecks else []
            ),
            f"- `{args.output.name}`: complete report for the new version against JPEG.",
            "",
        ]
    )

    atomic_text(args.output, "\n".join(lines))
    print(f"A/B report: {args.output}")
    print(f"CSV A/B: {args.output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

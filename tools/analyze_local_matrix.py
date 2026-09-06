#!/usr/bin/env python3
"""Validate and aggregate the local single/multi-thread benchmark matrix.

Copyright (c) Micilini Roll. Licensed under the MIT License.

The benchmark runner intentionally keeps one raw CSV per run.  This tool
cross-checks their format-bearing fields and emits lossless consolidated CSVs
without replacing those source measurements.
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
import statistics
from collections import defaultdict
from decimal import Decimal, getcontext
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[1]
RUNS = {
    1: ROOT / "benchmarks/v1/local-pinned-single-cpu0-100",
    2: ROOT / "benchmarks/v1/local-pinned-threads-2",
    4: ROOT / "benchmarks/v1/local-pinned-threads-4",
    8: ROOT / "benchmarks/v1/local-pinned-threads-8",
    10: ROOT / "benchmarks/v1/local-pinned-threads-10",
}
REPEATS = {
    1: ROOT / "benchmarks/v1/local-pinned-single-cpu0-repeat-100",
    10: ROOT / "benchmarks/v1/local-pinned-threads-10-repeat",
}
OUT_DIR = ROOT / "benchmarks" / "v1"
KEY_FIELDS = ("image_id", "quality")
INVARIANT_FIELDS = (
    "pageid",
    "width",
    "height",
    "pixels",
    "quality",
    "chroma",
    "reps",
    "n148_bytes",
    "n148_psnr_db",
    "jpeg_bytes",
    "jpeg_psnr_db",
)
TIME_FIELDS = (
    "n148_enc_ms",
    "n148_dec_ms",
    "n148_planes_ms",
    "n148_merge_ms",
    "n148_other_ms",
    "jpeg_enc_ms",
    "jpeg_dec_ms",
)

getcontext().prec = 50


def load_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def key(row: dict[str, str]) -> tuple[int, int]:
    return tuple(int(row[field]) for field in KEY_FIELDS)  # type: ignore[return-value]


def dsum(rows: Iterable[dict[str, str]], field: str) -> Decimal:
    return sum((Decimal(row[field]) for row in rows), Decimal(0))


def ratio(left: Decimal, right: Decimal) -> Decimal:
    return left / right


def record(
    rows: Iterable[dict[str, str]], left_field: str, right_field: str
) -> tuple[int, int, int]:
    wins = ties = losses = 0
    for row in rows:
        left = Decimal(row[left_field])
        right = Decimal(row[right_field])
        if left < right:
            wins += 1
        elif left == right:
            ties += 1
        else:
            losses += 1
    return wins, ties, losses


def solve_linear(matrix: list[list[float]], vector: list[float]) -> list[float]:
    augmented = [row[:] + [value] for row, value in zip(matrix, vector)]
    size = len(vector)
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1e-14:
            raise ValueError("singular curve")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        divisor = augmented[column][column]
        augmented[column] = [value / divisor for value in augmented[column]]
        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [
                left - factor * right
                for left, right in zip(augmented[row], augmented[column])
            ]
    return [augmented[row][-1] for row in range(size)]


def bd_rate(
    candidate: list[tuple[float, float]], anchor: list[tuple[float, float]]
) -> float | None:
    candidate = sorted(candidate)
    anchor = sorted(anchor)
    if len(candidate) != 4 or len(anchor) != 4:
        return None
    low = max(candidate[0][0], anchor[0][0])
    high = min(candidate[-1][0], anchor[-1][0])
    if not math.isfinite(low + high) or high - low < 1e-6:
        return None
    center = (low + high) / 2.0
    scale = (high - low) / 2.0

    def integral(points: list[tuple[float, float]]) -> float:
        matrix: list[list[float]] = []
        values: list[float] = []
        for psnr, rate_value in points:
            if rate_value <= 0 or not math.isfinite(psnr + rate_value):
                raise ValueError("invalid curve point")
            x = (psnr - center) / scale
            matrix.append([1.0, x, x * x, x * x * x])
            values.append(math.log(rate_value))
        coefficients = solve_linear(matrix, values)
        return scale * sum(
            coefficient
            * (((1.0 ** (power + 1)) - ((-1.0) ** (power + 1))) / (power + 1))
            for power, coefficient in enumerate(coefficients)
        )

    try:
        difference = (integral(candidate) - integral(anchor)) / (high - low)
        return (math.exp(difference) - 1.0) * 100.0
    except (OverflowError, ValueError, ZeroDivisionError):
        return None


def pooled_psnr(rows: Iterable[dict[str, str]], prefix: str) -> float:
    total_pixels = 0.0
    squared_error = 0.0
    for row in rows:
        pixels = float(row["pixels"])
        psnr = float(row[f"{prefix}_psnr_db"])
        total_pixels += pixels
        squared_error += pixels * 65025.0 / (10.0 ** (psnr / 10.0))
    mse = squared_error / total_pixels
    return 10.0 * math.log10(65025.0 / mse)


def load_run(threads: int, path: Path) -> dict[str, Any]:
    metadata = load_json(path / "metadata.json")
    rows = load_csv(path / "results.csv")
    manifest = load_json(path / "manifest.json")
    failures = load_json(path / "failures.json")
    if metadata.get("status") != "complete":
        raise RuntimeError(f"{path}: run is not complete")
    if int(metadata.get("completed_images", -1)) != 100:
        raise RuntimeError(f"{path}: expected 100 completed images")
    if int(metadata.get("completed_points", -1)) != 400 or len(rows) != 400:
        raise RuntimeError(f"{path}: expected 400 completed points")
    if failures or int(metadata.get("rejected_candidates", -1)) != 0:
        raise RuntimeError(f"{path}: run contains failures or rejections")
    if int(metadata["config"]["threads"]) != threads:
        raise RuntimeError(f"{path}: thread count does not match directory role")
    indexed = {key(row): row for row in rows}
    if len(indexed) != len(rows):
        raise RuntimeError(f"{path}: duplicate image/quality key")
    return {
        "path": path,
        "metadata": metadata,
        "rows": rows,
        "indexed": indexed,
        "manifest": manifest,
        "manifest_by_id": {int(item["image_id"]): item for item in manifest},
        "manifest_sha256": sha256(path / "manifest.json"),
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        raise RuntimeError(f"refusing to write empty CSV: {path}")
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    runs = {threads: load_run(threads, path) for threads, path in RUNS.items()}
    baseline = runs[1]
    baseline_keys = set(baseline["indexed"])
    mismatch_examples: list[dict[str, Any]] = []
    manifest_mismatches = 0

    for threads, run in runs.items():
        if set(run["indexed"]) != baseline_keys:
            raise RuntimeError(f"T{threads}: point identities differ from T1")
        for point_key, expected in baseline["indexed"].items():
            actual = run["indexed"][point_key]
            for field in INVARIANT_FIELDS:
                if actual[field] != expected[field]:
                    mismatch_examples.append(
                        {
                            "threads": threads,
                            "image_id": point_key[0],
                            "quality": point_key[1],
                            "field": field,
                            "expected": expected[field],
                            "actual": actual[field],
                        }
                    )
        for image_id, expected in baseline["manifest_by_id"].items():
            actual = run["manifest_by_id"].get(image_id)
            if not actual or actual.get("ppm_sha256") != expected.get("ppm_sha256"):
                manifest_mismatches += 1

    if mismatch_examples or manifest_mismatches:
        raise RuntimeError(
            f"matrix invariance failed: {len(mismatch_examples)} field mismatches, "
            f"{manifest_mismatches} manifest mismatches; examples={mismatch_examples[:5]}"
        )

    baseline_totals = {
        field: dsum(baseline["rows"], field)
        for field in TIME_FIELDS + ("n148_bytes", "jpeg_bytes")
    }
    scaling_rows: list[dict[str, Any]] = []
    quality_rows: list[dict[str, Any]] = []

    for threads, run in runs.items():
        rows = run["rows"]
        totals = {field: dsum(rows, field) for field in TIME_FIELDS}
        enc_speedup = ratio(baseline_totals["n148_enc_ms"], totals["n148_enc_ms"])
        dec_speedup = ratio(baseline_totals["n148_dec_ms"], totals["n148_dec_ms"])
        enc_record = record(rows, "n148_enc_ms", "jpeg_enc_ms")
        dec_record = record(rows, "n148_dec_ms", "jpeg_dec_ms")
        scaling_rows.append(
            {
                "threads": threads,
                "cpu_set": run["metadata"]["config"]["cpu_set"],
                "jpeg_cpu": run["metadata"]["config"]["jpeg_cpu"],
                **{field: totals[field] for field in TIME_FIELDS},
                "n148_encode_speedup_vs_t1": enc_speedup,
                "n148_decode_speedup_vs_t1": dec_speedup,
                "n148_encode_parallel_efficiency": enc_speedup / Decimal(threads),
                "n148_decode_parallel_efficiency": dec_speedup / Decimal(threads),
                "encode_delta_vs_same_run_jpeg": ratio(
                    totals["n148_enc_ms"], totals["jpeg_enc_ms"]
                ) - 1,
                "decode_delta_vs_same_run_jpeg": ratio(
                    totals["n148_dec_ms"], totals["jpeg_dec_ms"]
                ) - 1,
                "jpeg_encode_control_drift_vs_t1": ratio(
                    totals["jpeg_enc_ms"], baseline_totals["jpeg_enc_ms"]
                ) - 1,
                "jpeg_decode_control_drift_vs_t1": ratio(
                    totals["jpeg_dec_ms"], baseline_totals["jpeg_dec_ms"]
                ) - 1,
                "n148_encode_wins": enc_record[0],
                "n148_encode_ties": enc_record[1],
                "n148_encode_losses": enc_record[2],
                "n148_decode_wins": dec_record[0],
                "n148_decode_ties": dec_record[1],
                "n148_decode_losses": dec_record[2],
            }
        )

        rows_by_quality: dict[int, list[dict[str, str]]] = defaultdict(list)
        for row in rows:
            rows_by_quality[int(row["quality"])].append(row)
        for quality in sorted(rows_by_quality):
            subset = rows_by_quality[quality]
            baseline_subset = [
                row for row in baseline["rows"] if int(row["quality"]) == quality
            ]
            values = {
                field: dsum(subset, field)
                for field in (
                    "n148_bytes",
                    "jpeg_bytes",
                    "n148_enc_ms",
                    "jpeg_enc_ms",
                    "n148_dec_ms",
                    "jpeg_dec_ms",
                )
            }
            base_n_enc = dsum(baseline_subset, "n148_enc_ms")
            base_n_dec = dsum(baseline_subset, "n148_dec_ms")
            size_record = record(subset, "n148_bytes", "jpeg_bytes")
            enc_record_q = record(subset, "n148_enc_ms", "jpeg_enc_ms")
            dec_record_q = record(subset, "n148_dec_ms", "jpeg_dec_ms")
            quality_rows.append(
                {
                    "threads": threads,
                    "quality": quality,
                    "points": len(subset),
                    **values,
                    "size_ratio": ratio(values["n148_bytes"], values["jpeg_bytes"]),
                    "encode_ratio": ratio(values["n148_enc_ms"], values["jpeg_enc_ms"]),
                    "decode_ratio": ratio(values["n148_dec_ms"], values["jpeg_dec_ms"]),
                    "n148_encode_speedup_vs_t1": ratio(base_n_enc, values["n148_enc_ms"]),
                    "n148_decode_speedup_vs_t1": ratio(base_n_dec, values["n148_dec_ms"]),
                    "n148_pooled_psnr_db": repr(pooled_psnr(subset, "n148")),
                    "jpeg_pooled_psnr_db": repr(pooled_psnr(subset, "jpeg")),
                    "size_wins": size_record[0],
                    "size_ties": size_record[1],
                    "size_losses": size_record[2],
                    "encode_wins": enc_record_q[0],
                    "encode_ties": enc_record_q[1],
                    "encode_losses": enc_record_q[2],
                    "decode_wins": dec_record_q[0],
                    "decode_ties": dec_record_q[1],
                    "decode_losses": dec_record_q[2],
                }
            )

    rows_by_image: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in baseline["rows"]:
        rows_by_image[int(row["image_id"])].append(row)
    image_rows: list[dict[str, Any]] = []
    image_bd_values: list[float] = []
    for image_id in sorted(rows_by_image):
        subset = sorted(rows_by_image[image_id], key=lambda row: int(row["quality"]))
        source = baseline["manifest_by_id"][image_id]
        n_curve = [
            (float(row["n148_psnr_db"]), float(row["n148_bytes"]) * 8 / float(row["pixels"]))
            for row in subset
        ]
        j_curve = [
            (float(row["jpeg_psnr_db"]), float(row["jpeg_bytes"]) * 8 / float(row["pixels"]))
            for row in subset
        ]
        image_bd = bd_rate(n_curve, j_curve)
        if image_bd is not None and math.isfinite(image_bd):
            image_bd_values.append(image_bd)
        item: dict[str, Any] = {
            "image_id": image_id,
            "pageid": source["pageid"],
            "title": source["title"],
            "width": source["width"],
            "height": source["height"],
            "ppm_sha256": source["ppm_sha256"],
            "n148_bytes": dsum(subset, "n148_bytes"),
            "jpeg_bytes": dsum(subset, "jpeg_bytes"),
            "size_ratio": ratio(dsum(subset, "n148_bytes"), dsum(subset, "jpeg_bytes")),
            "bd_rate_percent": "" if image_bd is None else repr(image_bd),
        }
        for threads, run in runs.items():
            run_subset = [
                run["indexed"][(image_id, int(row["quality"]))] for row in subset
            ]
            n_enc = dsum(run_subset, "n148_enc_ms")
            n_dec = dsum(run_subset, "n148_dec_ms")
            j_enc = dsum(run_subset, "jpeg_enc_ms")
            j_dec = dsum(run_subset, "jpeg_dec_ms")
            item[f"t{threads}_n148_enc_ms"] = n_enc
            item[f"t{threads}_jpeg_enc_ms"] = j_enc
            item[f"t{threads}_encode_ratio"] = ratio(n_enc, j_enc)
            item[f"t{threads}_n148_dec_ms"] = n_dec
            item[f"t{threads}_jpeg_dec_ms"] = j_dec
            item[f"t{threads}_decode_ratio"] = ratio(n_dec, j_dec)
            if threads != 1:
                item[f"t{threads}_encode_speedup_vs_t1"] = ratio(
                    Decimal(item["t1_n148_enc_ms"]), n_enc
                )
                item[f"t{threads}_decode_speedup_vs_t1"] = ratio(
                    Decimal(item["t1_n148_dec_ms"]), n_dec
                )
        image_rows.append(item)

    quality_base = [row for row in quality_rows if row["threads"] == 1]
    corpus_n_curve = [
        (
            float(row["n148_pooled_psnr_db"]),
            float(row["n148_bytes"])
            * 8
            / sum(float(raw["pixels"]) for raw in baseline["rows"] if int(raw["quality"]) == row["quality"]),
        )
        for row in quality_base
    ]
    corpus_j_curve = [
        (
            float(row["jpeg_pooled_psnr_db"]),
            float(row["jpeg_bytes"])
            * 8
            / sum(float(raw["pixels"]) for raw in baseline["rows"] if int(raw["quality"]) == row["quality"]),
        )
        for row in quality_base
    ]
    corpus_bd = bd_rate(corpus_n_curve, corpus_j_curve)

    write_csv(OUT_DIR / "local-thread-scaling.csv", scaling_rows)
    write_csv(OUT_DIR / "local-by-quality.csv", quality_rows)
    write_csv(OUT_DIR / "local-by-image.csv", image_rows)

    repeatability_rows: list[dict[str, Any]] = []
    for threads, repeat_path in REPEATS.items():
        primary = runs[threads]
        repeated = load_run(threads, repeat_path)
        repeat_mismatches = 0
        for point_key, expected in primary["indexed"].items():
            actual = repeated["indexed"].get(point_key)
            if actual is None:
                repeat_mismatches += 1
                continue
            repeat_mismatches += sum(
                actual[field] != expected[field] for field in INVARIANT_FIELDS
            )
        for image_id, expected in primary["manifest_by_id"].items():
            actual = repeated["manifest_by_id"].get(image_id)
            if not actual or actual.get("ppm_sha256") != expected.get("ppm_sha256"):
                repeat_mismatches += 1
        if repeat_mismatches:
            raise RuntimeError(
                f"T{threads} repeat invariance failed with {repeat_mismatches} mismatches"
            )

        primary_totals = {
            field: dsum(primary["rows"], field) for field in TIME_FIELDS
        }
        repeated_totals = {
            field: dsum(repeated["rows"], field) for field in TIME_FIELDS
        }
        primary_enc_ratio = ratio(
            primary_totals["n148_enc_ms"], primary_totals["jpeg_enc_ms"]
        )
        repeated_enc_ratio = ratio(
            repeated_totals["n148_enc_ms"], repeated_totals["jpeg_enc_ms"]
        )
        primary_dec_ratio = ratio(
            primary_totals["n148_dec_ms"], primary_totals["jpeg_dec_ms"]
        )
        repeated_dec_ratio = ratio(
            repeated_totals["n148_dec_ms"], repeated_totals["jpeg_dec_ms"]
        )
        repeatability_rows.append(
            {
                "threads": threads,
                "primary_path": primary["path"].relative_to(ROOT),
                "repeat_path": repeated["path"].relative_to(ROOT),
                "invariant_field_mismatches": repeat_mismatches,
                "primary_n148_enc_ms": primary_totals["n148_enc_ms"],
                "repeat_n148_enc_ms": repeated_totals["n148_enc_ms"],
                "n148_encode_repeat_delta": ratio(
                    repeated_totals["n148_enc_ms"], primary_totals["n148_enc_ms"]
                ) - 1,
                "primary_n148_dec_ms": primary_totals["n148_dec_ms"],
                "repeat_n148_dec_ms": repeated_totals["n148_dec_ms"],
                "n148_decode_repeat_delta": ratio(
                    repeated_totals["n148_dec_ms"], primary_totals["n148_dec_ms"]
                ) - 1,
                "primary_jpeg_enc_ms": primary_totals["jpeg_enc_ms"],
                "repeat_jpeg_enc_ms": repeated_totals["jpeg_enc_ms"],
                "jpeg_encode_repeat_delta": ratio(
                    repeated_totals["jpeg_enc_ms"], primary_totals["jpeg_enc_ms"]
                ) - 1,
                "primary_jpeg_dec_ms": primary_totals["jpeg_dec_ms"],
                "repeat_jpeg_dec_ms": repeated_totals["jpeg_dec_ms"],
                "jpeg_decode_repeat_delta": ratio(
                    repeated_totals["jpeg_dec_ms"], primary_totals["jpeg_dec_ms"]
                ) - 1,
                "primary_n148_vs_jpeg_encode_ratio": primary_enc_ratio,
                "repeat_n148_vs_jpeg_encode_ratio": repeated_enc_ratio,
                "encode_ratio_repeat_change": repeated_enc_ratio - primary_enc_ratio,
                "primary_n148_vs_jpeg_decode_ratio": primary_dec_ratio,
                "repeat_n148_vs_jpeg_decode_ratio": repeated_dec_ratio,
                "decode_ratio_repeat_change": repeated_dec_ratio - primary_dec_ratio,
            }
        )
    write_csv(OUT_DIR / "local-repeatability.csv", repeatability_rows)
    baseline_size_record = record(baseline["rows"], "n148_bytes", "jpeg_bytes")
    baseline_enc_record = record(baseline["rows"], "n148_enc_ms", "jpeg_enc_ms")
    baseline_dec_record = record(baseline["rows"], "n148_dec_ms", "jpeg_dec_ms")
    summary = {
        "status": "PASS",
        "runs": {
            str(threads): {
                "path": str(run["path"].relative_to(ROOT)),
                "status": run["metadata"]["status"],
                "completed_images": run["metadata"]["completed_images"],
                "completed_points": run["metadata"]["completed_points"],
                "rejected_candidates": run["metadata"]["rejected_candidates"],
                "manifest_sha256": run["manifest_sha256"],
                "cpu_set": run["metadata"]["config"]["cpu_set"],
                "jpeg_cpu": run["metadata"]["config"]["jpeg_cpu"],
            }
            for threads, run in runs.items()
        },
        "invariance": {
            "points_compared_per_run": len(baseline_keys),
            "fields_compared": list(INVARIANT_FIELDS),
            "field_mismatches": 0,
            "manifest_ppm_hash_mismatches": 0,
        },
        "repeatability": {
            "runs_repeated": sorted(REPEATS),
            "invariant_field_mismatches": 0,
            "max_absolute_n148_timing_delta": str(
                max(
                    abs(Decimal(str(row[field])))
                    for row in repeatability_rows
                    for field in (
                        "n148_encode_repeat_delta",
                        "n148_decode_repeat_delta",
                    )
                )
            ),
        },
        "baseline": {
            "n148_bytes": str(baseline_totals["n148_bytes"]),
            "jpeg_bytes": str(baseline_totals["jpeg_bytes"]),
            "size_ratio": str(ratio(baseline_totals["n148_bytes"], baseline_totals["jpeg_bytes"])),
            "n148_encode_ms": str(baseline_totals["n148_enc_ms"]),
            "jpeg_encode_ms": str(baseline_totals["jpeg_enc_ms"]),
            "encode_ratio": str(ratio(baseline_totals["n148_enc_ms"], baseline_totals["jpeg_enc_ms"])),
            "n148_decode_ms": str(baseline_totals["n148_dec_ms"]),
            "jpeg_decode_ms": str(baseline_totals["jpeg_dec_ms"]),
            "decode_ratio": str(ratio(baseline_totals["n148_dec_ms"], baseline_totals["jpeg_dec_ms"])),
            "size_record": baseline_size_record,
            "encode_record": baseline_enc_record,
            "decode_record": baseline_dec_record,
            "bd_rate_percent": corpus_bd,
            "image_bd_rate_median_percent": statistics.median(image_bd_values),
            "image_bd_rate_mean_percent": statistics.mean(image_bd_values),
        },
        "outputs": [
            "benchmarks/v1/local-thread-scaling.csv",
            "benchmarks/v1/local-by-quality.csv",
            "benchmarks/v1/local-by-image.csv",
            "benchmarks/v1/local-repeatability.csv",
            "benchmarks/v1/local-matrix-verification.json",
        ],
    }
    verification_path = OUT_DIR / "local-matrix-verification.json"
    with verification_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

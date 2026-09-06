#!/usr/bin/env python3
"""Analyze the final N.148i benchmark matrix and emit publication data.

The script is the JPEG XL/multi-metric extension of analyze_local_matrix.py.
It validates matrix completeness, computes PCHIP Bjontegaard delta-rate,
paired Wilcoxon tests, category effects, timing ratios, thread scaling and
rate-distortion plots.

Copyright (c) Micilini Roll. Licensed under the MIT License.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import statistics
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

try:
    import matplotlib
    matplotlib.use("Agg")
    matplotlib.rcParams["svg.hashsalt"] = "n148i-final-benchmark"
    import matplotlib.pyplot as plt
    import numpy as np
    from scipy.interpolate import PchipInterpolator
    from scipy.stats import kruskal, wilcoxon
except ImportError as error:  # pragma: no cover
    raise SystemExit("numpy, scipy and matplotlib are required; see BENCHMARK.md") from error


METRICS: dict[str, dict[str, Any]] = {
    "psnr_rgb_db": {"label": "PSNR RGB (dB)", "higher": True, "pooled": True},
    "psnr_y_db": {"label": "PSNR Y (dB)", "higher": True, "pooled": True},
    "ssim": {"label": "SSIM", "higher": True, "pooled": False},
    "ms_ssim": {"label": "MS-SSIM", "higher": True, "pooled": False},
    "ssimulacra2": {"label": "SSIMULACRA2", "higher": True, "pooled": False},
    "butteraugli": {"label": "Butteraugli", "higher": False, "pooled": False},
}
CODECS = ("n148i", "jpeg", "jxl_e3", "jxl_e7")
COMPETITORS = ("jpeg", "jxl_e3", "jxl_e7")


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def atomic_csv(path: Path, fields: list[str], rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="", dir=path.parent,
        prefix=".atomic-", delete=False
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
        handle.flush()
        os.fsync(handle.fileno())
        temporary = Path(handle.name)
    os.replace(temporary, path)


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, prefix=".atomic-", delete=False
    ) as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2, allow_nan=False)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
        temporary = Path(handle.name)
    os.replace(temporary, path)


def save_svg(fig: Any, path: Path) -> None:
    """Write a deterministic SVG without Matplotlib's trailing spaces."""
    fig.savefig(path, metadata={"Date": None})
    rendered = path.read_text(encoding="utf-8")
    normalized = "\n".join(line.rstrip() for line in rendered.splitlines()) + "\n"
    path.write_text(normalized, encoding="utf-8")


def f(row: dict[str, str], field: str) -> float:
    return float(row[field])


def quantile(values: Iterable[float], probability: float) -> float:
    return float(np.quantile(np.asarray(list(values), dtype=np.float64), probability))


def geometric_mean(values: Iterable[float]) -> float:
    array = np.asarray(list(values), dtype=np.float64)
    if len(array) == 0 or np.any(array <= 0):
        return math.nan
    return float(np.exp(np.mean(np.log(array))))


def record(values: Iterable[float], tolerance: float = 1.0) -> tuple[int, int, int]:
    wins = ties = losses = 0
    for value in values:
        if value < -tolerance:
            wins += 1
        elif value > tolerance:
            losses += 1
        else:
            ties += 1
    return wins, ties, losses


def pareto_points(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Return increasing-quality, increasing-rate non-dominated points."""
    finite = sorted(
        (quality, rate) for quality, rate in points
        if math.isfinite(quality) and math.isfinite(rate) and rate > 0
    )
    unique: list[tuple[float, float]] = []
    for quality, rate in finite:
        if unique and abs(quality - unique[-1][0]) <= 1e-12:
            if rate < unique[-1][1]:
                unique[-1] = (quality, rate)
        else:
            unique.append((quality, rate))
    # A point is dominated if equal/better quality exists at a lower rate.
    kept_reversed: list[tuple[float, float]] = []
    best_rate = math.inf
    for quality, rate in reversed(unique):
        if rate < best_rate:
            kept_reversed.append((quality, rate))
            best_rate = rate
    kept = list(reversed(kept_reversed))
    # PCHIP also expects rate to grow with quality for a physical RD curve.
    monotone: list[tuple[float, float]] = []
    largest = -math.inf
    for point in kept:
        if point[1] > largest:
            monotone.append(point)
            largest = point[1]
    return monotone


def bd_rate(
    candidate: list[tuple[float, float]], anchor: list[tuple[float, float]]
) -> tuple[float, float, float] | None:
    """PCHIP Bjontegaard delta-rate; negative means candidate uses fewer bits."""
    candidate = pareto_points(candidate)
    anchor = pareto_points(anchor)
    if len(candidate) < 3 or len(anchor) < 3:
        return None
    low = max(candidate[0][0], anchor[0][0])
    high = min(candidate[-1][0], anchor[-1][0])
    if not math.isfinite(low + high) or high - low <= 1e-9:
        return None
    candidate_curve = PchipInterpolator(
        [point[0] for point in candidate],
        [math.log(point[1]) for point in candidate], extrapolate=False,
    )
    anchor_curve = PchipInterpolator(
        [point[0] for point in anchor],
        [math.log(point[1]) for point in anchor], extrapolate=False,
    )
    difference = (
        candidate_curve.integrate(low, high) - anchor_curve.integrate(low, high)
    ) / (high - low)
    return (math.exp(float(difference)) - 1.0) * 100.0, low, high


def metric_quality(row: dict[str, str], metric: str) -> float:
    value = f(row, metric)
    return value if METRICS[metric]["higher"] else -value


def pooled_psnr(rows: list[dict[str, str]], metric: str) -> float:
    total_pixels = sum(f(row, "pixels") for row in rows)
    mse = sum(
        f(row, "pixels") * 65025.0 / (10.0 ** (f(row, metric) / 10.0))
        for row in rows
    ) / total_pixels
    return 10.0 * math.log10(65025.0 / mse)


def pixel_weighted_mean(rows: list[dict[str, str]], metric: str) -> float:
    total_pixels = sum(f(row, "pixels") for row in rows)
    return sum(
        f(row, "pixels") * f(row, metric) for row in rows
    ) / total_pixels


def paired_p(values: list[float]) -> float:
    nonzero = [value for value in values if math.isfinite(value) and value != 0.0]
    if not nonzero:
        return 1.0
    return float(wilcoxon(nonzero, alternative="two-sided", method="auto").pvalue)


def validate_matrix(
    rows: list[dict[str, str]], manifest: list[dict[str, Any]],
    samples: list[dict[str, str]],
) -> dict[str, Any]:
    image_count = len(manifest)
    axis_a = [row for row in rows if row["axis"] == "A_single_thread"]
    axis_b = [row for row in rows if row["axis"] == "B_throughput"]
    sweep = [row for row in rows if row["axis"] == "N148_thread_sweep"]
    expected_main = image_count * 5 * 4
    if len(axis_a) != expected_main or len(axis_b) != expected_main:
        raise ValueError(
            f"incomplete main axes: A={len(axis_a)}, B={len(axis_b)}, expected={expected_main}"
        )
    sweep_threads = sorted({int(row["threads"]) for row in sweep})
    if len(sweep) != image_count * len(sweep_threads):
        raise ValueError("incomplete N.148i thread sweep")
    image_ids = {int(item["image_id"]) for item in manifest}
    expected_main_keys = {
        (image_id, point, codec)
        for image_id in image_ids for point in range(1, 6) for codec in CODECS
    }
    for name, axis_rows in (("A", axis_a), ("B", axis_b)):
        observed = {
            (int(row["image_id"]), int(row["point"]), row["codec"])
            for row in axis_rows
        }
        if observed != expected_main_keys:
            raise ValueError(f"{name} axis key matrix differs from expectation")
    if any(int(row["threads"]) != 1 for row in axis_a):
        raise ValueError("axis A contains a non-single-thread row")
    throughput_threads = {
        int(row["threads"]) for row in axis_b if row["codec"] != "jpeg"
    }
    if len(throughput_threads) != 1:
        raise ValueError("axis B has inconsistent parallel thread counts")
    throughput_thread_count = next(iter(throughput_threads))
    if any(int(row["threads"]) != 1 for row in axis_b if row["codec"] == "jpeg"):
        raise ValueError("axis B JPEG must remain single-threaded")
    expected_sweep_threads = sorted(
        thread for thread in {1, 2, 4, 8, throughput_thread_count}
        if thread <= throughput_thread_count
    )
    if sweep_threads != expected_sweep_threads:
        raise ValueError(
            f"thread sweep {sweep_threads} differs from {expected_sweep_threads}"
        )
    expected_sweep_keys = {
        (image_id, thread) for image_id in image_ids
        for thread in expected_sweep_threads
    }
    observed_sweep_keys = {
        (int(row["image_id"]), int(row["threads"])) for row in sweep
        if row["codec"] == "n148i" and int(row["point"]) == 3
    }
    if observed_sweep_keys != expected_sweep_keys or any(
        row["codec"] != "n148i" or int(row["point"]) != 3 for row in sweep
    ):
        raise ValueError("thread sweep key matrix differs from expectation")
    keys: set[tuple[str, int, int, str, int]] = set()
    for row in rows:
        key = (
            row["axis"], int(row["image_id"]), int(row["point"]),
            row["codec"], int(row["threads"]),
        )
        if key in keys:
            raise ValueError(f"duplicate summary key: {key}")
        keys.add(key)
        if f(row, "encode_median_ms") <= 0 or f(row, "decode_median_ms") <= 0:
            raise ValueError(f"non-positive timing: {key}")
        if int(row["bytes"]) <= 0:
            raise ValueError(f"non-positive size: {key}")
    for row in axis_a:
        for metric in METRICS:
            if row[metric] == "" or not math.isfinite(f(row, metric)):
                raise ValueError(f"missing metric {metric}: {row['filename']} {row['codec']}")
    sample_indices: dict[tuple[str, int, int, str, int], list[int]] = defaultdict(list)
    for sample in samples:
        key = (
            sample["axis"], int(sample["image_id"]), int(sample["point"]),
            sample["codec"], int(sample["threads"]),
        )
        sample_indices[key].append(int(sample["sample"]))
    for row in rows:
        key = (
            row["axis"], int(row["image_id"]), int(row["point"]),
            row["codec"], int(row["threads"]),
        )
        expected_indices = list(range(int(row["reps"])))
        if sorted(sample_indices[key]) != expected_indices:
            raise ValueError(f"sample count differs for {key}")

    # Sizes must not depend on worker count. The same point in A and B is an
    # independent determinism check for every codec and corpus image.
    size_index = {
        (int(row["image_id"]), int(row["point"]), row["codec"]): int(row["bytes"])
        for row in axis_a
    }
    size_mismatches: list[str] = []
    for row in axis_b:
        key = (int(row["image_id"]), int(row["point"]), row["codec"])
        if int(row["bytes"]) != size_index[key]:
            size_mismatches.append(f"{key}: {size_index[key]} vs {row['bytes']}")
    if size_mismatches:
        raise ValueError("thread-dependent bitstream sizes: " + "; ".join(size_mismatches[:5]))
    for row in sweep:
        key = (int(row["image_id"]), int(row["point"]), "n148i")
        if int(row["bytes"]) != size_index[key]:
            raise ValueError(f"thread-sweep bitstream size differs for {key}")
    return {
        "image_count": image_count,
        "summary_rows": len(rows),
        "timing_samples": len(samples),
        "axis_a_rows": len(axis_a),
        "axis_b_rows": len(axis_b),
        "sweep_rows": len(sweep),
        "sweep_threads": sweep_threads,
        "throughput_threads": throughput_thread_count,
        "thread_size_mismatches": 0,
    }


def aggregate_curves(axis_a: list[dict[str, str]]) -> list[dict[str, Any]]:
    groups: dict[tuple[str, int], list[dict[str, str]]] = defaultdict(list)
    for row in axis_a:
        groups[(row["codec"], int(row["point"]))].append(row)
    curves: list[dict[str, Any]] = []
    for codec in CODECS:
        for point in range(1, 6):
            group = groups[(codec, point)]
            total_pixels = sum(int(row["pixels"]) for row in group)
            total_bytes = sum(int(row["bytes"]) for row in group)
            result: dict[str, Any] = {
                "codec": codec,
                "point": point,
                "quality": group[0]["quality"],
                "jxl_distance": group[0]["jxl_distance"],
                "jxl_effort": group[0]["jxl_effort"],
                "images": len(group),
                "total_pixels": total_pixels,
                "total_bytes": total_bytes,
                "bits_per_pixel": total_bytes * 8.0 / total_pixels,
            }
            for metric, config in METRICS.items():
                result[metric] = (
                    pooled_psnr(group, metric) if config["pooled"]
                    else pixel_weighted_mean(group, metric)
                )
            curves.append(result)
    return curves


def compute_bd_rates(
    axis_a: list[dict[str, str]], curves: list[dict[str, Any]],
    manifest_index: dict[int, dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    by_image_codec: dict[tuple[int, str], list[dict[str, str]]] = defaultdict(list)
    for row in axis_a:
        by_image_codec[(int(row["image_id"]), row["codec"])].append(row)
    aggregate_index: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in curves:
        aggregate_index[row["codec"]].append(row)

    per_image: list[dict[str, Any]] = []
    summaries: list[dict[str, Any]] = []
    for competitor in COMPETITORS:
        comparison = f"n148i_vs_{competitor}"
        for metric in METRICS:
            aggregate_candidate = [
                ((float(row[metric]) if METRICS[metric]["higher"] else -float(row[metric])),
                 float(row["bits_per_pixel"]))
                for row in aggregate_index["n148i"]
            ]
            aggregate_anchor = [
                ((float(row[metric]) if METRICS[metric]["higher"] else -float(row[metric])),
                 float(row["bits_per_pixel"]))
                for row in aggregate_index[competitor]
            ]
            aggregate = bd_rate(aggregate_candidate, aggregate_anchor)
            values: list[float] = []
            for image_id, image in manifest_index.items():
                candidate_rows = by_image_codec[(image_id, "n148i")]
                anchor_rows = by_image_codec[(image_id, competitor)]
                candidate = [
                    (metric_quality(row, metric), f(row, "bits_per_pixel"))
                    for row in candidate_rows
                ]
                anchor = [
                    (metric_quality(row, metric), f(row, "bits_per_pixel"))
                    for row in anchor_rows
                ]
                result = bd_rate(candidate, anchor)
                if result is None:
                    continue
                value, low, high = result
                values.append(value)
                per_image.append({
                    "comparison": comparison,
                    "metric": metric,
                    "image_id": image_id,
                    "filename": image["filename"],
                    "category": image["category"],
                    "bd_rate_pct": value,
                    "overlap_quality_low": low,
                    "overlap_quality_high": high,
                })
            wins, ties, losses = record(values)
            summaries.append({
                "comparison": comparison,
                "metric": metric,
                "aggregate_bd_rate_pct": aggregate[0] if aggregate else "",
                "aggregate_overlap_low": aggregate[1] if aggregate else "",
                "aggregate_overlap_high": aggregate[2] if aggregate else "",
                "per_image_valid": len(values),
                "per_image_missing": len(manifest_index) - len(values),
                "per_image_median_pct": statistics.median(values) if values else "",
                "per_image_q1_pct": quantile(values, 0.25) if values else "",
                "per_image_q3_pct": quantile(values, 0.75) if values else "",
                "wins": wins,
                "ties": ties,
                "losses": losses,
                "tie_threshold_pct": 1.0,
                "wilcoxon_p": paired_p(values),
            })
    return per_image, summaries


def timing_analysis(rows: list[dict[str, str]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    detail: list[dict[str, Any]] = []
    summaries: list[dict[str, Any]] = []
    for axis in ("A_single_thread", "B_throughput"):
        axis_rows = [row for row in rows if row["axis"] == axis]
        index = {
            (int(row["image_id"]), int(row["point"]), row["codec"]): row
            for row in axis_rows
        }
        image_ids = sorted({int(row["image_id"]) for row in axis_rows})
        for competitor in COMPETITORS:
            for operation in ("encode", "decode"):
                image_ratios: list[float] = []
                all_ratios: list[float] = []
                for image_id in image_ids:
                    ratios = []
                    for point in range(1, 6):
                        candidate = index[(image_id, point, "n148i")]
                        anchor = index[(image_id, point, competitor)]
                        ratio = (
                            f(candidate, f"{operation}_median_ms") /
                            f(anchor, f"{operation}_median_ms")
                        )
                        ratios.append(ratio)
                        all_ratios.append(ratio)
                    image_ratio = geometric_mean(ratios)
                    image_ratios.append(image_ratio)
                    detail.append({
                        "axis": axis,
                        "competitor": competitor,
                        "operation": operation,
                        "image_id": image_id,
                        "ratio_n148_over_competitor": image_ratio,
                        "n148_delta_pct": (image_ratio - 1.0) * 100.0,
                    })
                deltas = [(ratio - 1.0) * 100.0 for ratio in image_ratios]
                wins, ties, losses = record(deltas)
                summaries.append({
                    "axis": axis,
                    "competitor": competitor,
                    "operation": operation,
                    "geomean_ratio_n148_over_competitor": geometric_mean(all_ratios),
                    "median_image_ratio": statistics.median(image_ratios),
                    "image_ratio_q1": quantile(image_ratios, 0.25),
                    "image_ratio_q3": quantile(image_ratios, 0.75),
                    "n148_delta_pct": (geometric_mean(all_ratios) - 1.0) * 100.0,
                    "wins": wins,
                    "ties": ties,
                    "losses": losses,
                    "tie_threshold_pct": 1.0,
                    "wilcoxon_p": paired_p([math.log(ratio) for ratio in image_ratios]),
                })
    return detail, summaries


def absolute_timing_analysis(
    rows: list[dict[str, str]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Aggregate per-image medians into corpus latency and throughput."""
    by_point: dict[tuple[str, str, int], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["axis"] in ("A_single_thread", "B_throughput"):
            by_point[(row["axis"], row["codec"], int(row["point"]))].append(row)

    points: list[dict[str, Any]] = []
    for (axis, codec, point), group in sorted(by_point.items()):
        total_pixels = sum(int(row["pixels"]) for row in group)
        encode_ms = sum(f(row, "encode_median_ms") for row in group)
        decode_ms = sum(f(row, "decode_median_ms") for row in group)
        threads = {int(row["threads"]) for row in group}
        if len(threads) != 1:
            raise ValueError(f"inconsistent thread count for {axis}/{codec}/p{point}")
        points.append({
            "axis": axis,
            "codec": codec,
            "point": point,
            "quality": group[0]["quality"],
            "jxl_distance": group[0]["jxl_distance"],
            "jxl_effort": group[0]["jxl_effort"],
            "threads": next(iter(threads)),
            "images": len(group),
            "total_pixels": total_pixels,
            "encode_corpus_ms": encode_ms,
            "decode_corpus_ms": decode_ms,
            "encode_megapixels_s": total_pixels / 1_000.0 / encode_ms,
            "decode_megapixels_s": total_pixels / 1_000.0 / decode_ms,
        })

    summaries: list[dict[str, Any]] = []
    groups: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in points:
        groups[(str(row["axis"]), str(row["codec"]))].append(row)
    for (axis, codec), group in sorted(groups.items()):
        work_pixels = sum(int(row["total_pixels"]) for row in group)
        encode_ms = sum(float(row["encode_corpus_ms"]) for row in group)
        decode_ms = sum(float(row["decode_corpus_ms"]) for row in group)
        summaries.append({
            "axis": axis,
            "codec": codec,
            "threads": int(group[0]["threads"]),
            "quality_points": len(group),
            "images_per_point": int(group[0]["images"]),
            "total_work_pixels": work_pixels,
            "encode_total_ms": encode_ms,
            "decode_total_ms": decode_ms,
            "encode_megapixels_s": work_pixels / 1_000.0 / encode_ms,
            "decode_megapixels_s": work_pixels / 1_000.0 / decode_ms,
        })
    return points, summaries


def noise_by_axis(rows: list[dict[str, str]], target: float = 1.0) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for axis in ("A_single_thread", "B_throughput", "N148_thread_sweep"):
        group = [row for row in rows if row["axis"] == axis]
        encode = [f(row, "encode_noise_pct") for row in group]
        decode = [f(row, "decode_noise_pct") for row in group]
        worst = [max(enc, dec) for enc, dec in zip(encode, decode)]
        output.append({
            "axis": axis,
            "rows": len(group),
            "rows_at_or_below_target": sum(value <= target for value in worst),
            "rows_above_target": sum(value > target for value in worst),
            "target_pct": target,
            "median_worst_noise_pct": statistics.median(worst),
            "p95_worst_noise_pct": quantile(worst, 0.95),
            "maximum_worst_noise_pct": max(worst),
            "maximum_encode_noise_pct": max(encode),
            "maximum_decode_noise_pct": max(decode),
            "minimum_reps": min(int(row["reps"]) for row in group),
            "median_reps": statistics.median(int(row["reps"]) for row in group),
            "maximum_reps": max(int(row["reps"]) for row in group),
        })
    return output


def timing_category_analysis(
    timing_detail: list[dict[str, Any]],
    manifest_index: dict[int, dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    groups: dict[tuple[str, str, str, str], list[float]] = defaultdict(list)
    for row in timing_detail:
        category = str(manifest_index[int(row["image_id"])]["category"])
        key = (
            str(row["axis"]), str(row["competitor"]),
            str(row["operation"]), category,
        )
        groups[key].append(float(row["ratio_n148_over_competitor"]))

    summaries: list[dict[str, Any]] = []
    tests: list[dict[str, Any]] = []
    combinations = sorted({key[:3] for key in groups})
    categories = sorted({key[3] for key in groups})
    for axis, competitor, operation in combinations:
        samples: list[list[float]] = []
        for category in categories:
            ratios = groups[(axis, competitor, operation, category)]
            if not ratios:
                continue
            deltas = [(ratio - 1.0) * 100.0 for ratio in ratios]
            wins, ties, losses = record(deltas)
            summaries.append({
                "axis": axis,
                "competitor": competitor,
                "operation": operation,
                "category": category,
                "count": len(ratios),
                "geomean_ratio_n148_over_competitor": geometric_mean(ratios),
                "median_ratio": statistics.median(ratios),
                "ratio_q1": quantile(ratios, 0.25),
                "ratio_q3": quantile(ratios, 0.75),
                "n148_delta_pct": (geometric_mean(ratios) - 1.0) * 100.0,
                "wins": wins,
                "ties": ties,
                "losses": losses,
                "tie_threshold_pct": 1.0,
                "wilcoxon_p": paired_p([math.log(ratio) for ratio in ratios]),
            })
            if len(ratios) >= 2:
                samples.append([math.log(ratio) for ratio in ratios])
        statistic: float | None = None
        pvalue: float | None = None
        if len(samples) >= 2:
            test = kruskal(*samples)
            statistic, pvalue = float(test.statistic), float(test.pvalue)
        tests.append({
            "axis": axis,
            "competitor": competitor,
            "operation": operation,
            "kruskal_h": statistic,
            "kruskal_p": pvalue,
            "categories_tested": len(samples),
        })
    return summaries, tests


def thread_scaling(rows: list[dict[str, str]]) -> list[dict[str, Any]]:
    sweep = [row for row in rows if row["axis"] == "N148_thread_sweep"]
    grouped: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in sweep:
        grouped[int(row["threads"])].append(row)
    baseline = grouped[min(grouped)]
    baseline_enc = sum(f(row, "encode_median_ms") for row in baseline)
    baseline_dec = sum(f(row, "decode_median_ms") for row in baseline)
    output: list[dict[str, Any]] = []
    for threads in sorted(grouped):
        enc = sum(f(row, "encode_median_ms") for row in grouped[threads])
        dec = sum(f(row, "decode_median_ms") for row in grouped[threads])
        for operation, elapsed, base in (
            ("encode", enc, baseline_enc), ("decode", dec, baseline_dec)
        ):
            speedup = base / elapsed
            output.append({
                "operation": operation,
                "threads": threads,
                "corpus_time_ms": elapsed,
                "speedup": speedup,
                "parallel_efficiency_pct": speedup / threads * 100.0,
                "throughput_megapixels_s": (
                    sum(int(row["pixels"]) for row in grouped[threads]) /
                    1_000_000.0 / (elapsed / 1000.0)
                ),
            })
    return output


def category_analysis(
    per_image_bd: list[dict[str, Any]], manifest: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    groups: dict[tuple[str, str, str], list[float]] = defaultdict(list)
    for row in per_image_bd:
        groups[(row["comparison"], row["metric"], row["category"])].append(
            float(row["bd_rate_pct"])
        )
    summary: list[dict[str, Any]] = []
    tests: list[dict[str, Any]] = []
    comparisons = sorted({row["comparison"] for row in per_image_bd})
    categories = sorted({item["category"] for item in manifest})
    for comparison in comparisons:
        for metric in METRICS:
            samples = []
            for category in categories:
                values = groups[(comparison, metric, category)]
                if values:
                    wins, ties, losses = record(values)
                    summary.append({
                        "comparison": comparison,
                        "metric": metric,
                        "category": category,
                        "count": len(values),
                        "median_bd_rate_pct": statistics.median(values),
                        "q1_bd_rate_pct": quantile(values, 0.25),
                        "q3_bd_rate_pct": quantile(values, 0.75),
                        "wins": wins,
                        "ties": ties,
                        "losses": losses,
                    })
                    if len(values) >= 2:
                        samples.append(values)
            statistic: float | None = None
            pvalue: float | None = None
            if len(samples) >= 2:
                test = kruskal(*samples)
                statistic, pvalue = float(test.statistic), float(test.pvalue)
            tests.append({
                "comparison": comparison,
                "metric": metric,
                "kruskal_h": statistic,
                "kruskal_p": pvalue,
                "categories_tested": len(samples),
            })
    return summary, tests


def agreement_analysis(per_image_bd: list[dict[str, Any]]) -> list[dict[str, Any]]:
    index: dict[tuple[str, int], dict[str, float]] = defaultdict(dict)
    for row in per_image_bd:
        index[(row["comparison"], int(row["image_id"]))][row["metric"]] = float(
            row["bd_rate_pct"]
        )
    output: list[dict[str, Any]] = []
    for comparison in sorted({key[0] for key in index}):
        unanimous_win = unanimous_loss = mixed = incomplete = 0
        for (current, _), metrics in index.items():
            if current != comparison:
                continue
            if set(metrics) != set(METRICS):
                incomplete += 1
                continue
            signs = {-1 if value < -1.0 else 1 if value > 1.0 else 0 for value in metrics.values()}
            if signs <= {-1, 0} and -1 in signs:
                unanimous_win += 1
            elif signs <= {0, 1} and 1 in signs:
                unanimous_loss += 1
            else:
                mixed += 1
        output.append({
            "comparison": comparison,
            "unanimous_n148_win": unanimous_win,
            "unanimous_n148_loss": unanimous_loss,
            "metric_disagreement": mixed,
            "incomplete": incomplete,
        })
    return output


def extreme_cases(per_image_bd: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    groups: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in per_image_bd:
        groups[(row["comparison"], row["metric"])].append(row)
    for (comparison, metric), values in groups.items():
        ordered = sorted(values, key=lambda row: float(row["bd_rate_pct"]))
        for side, selected in (("best", ordered[:5]), ("worst", reversed(ordered[-5:]))):
            for rank, row in enumerate(selected, 1):
                output.append({
                    "comparison": comparison,
                    "metric": metric,
                    "side": side,
                    "rank": rank,
                    "image_id": row["image_id"],
                    "filename": row["filename"],
                    "category": row["category"],
                    "bd_rate_pct": row["bd_rate_pct"],
                })
    return output


def corpus_summary(manifest: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    by_category: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for item in manifest:
        by_category[item["category"]].append(item)
    for category, items in sorted(by_category.items()):
        pixels = [int(item["pixels"]) for item in items]
        output.append({
            "kind": "category",
            "name": category,
            "count": len(items),
            "minimum_pixels": min(pixels),
            "median_pixels": statistics.median(pixels),
            "maximum_pixels": max(pixels),
        })
    bins = ((0, 400_000), (400_000, 700_000), (700_000, 1_000_000), (1_000_000, math.inf))
    for low, high in bins:
        items = [item for item in manifest if low <= int(item["pixels"]) < high]
        output.append({
            "kind": "resolution_bin",
            "name": f"{low:g}-{high:g}",
            "count": len(items),
            "minimum_pixels": min((int(item["pixels"]) for item in items), default=""),
            "median_pixels": statistics.median([int(item["pixels"]) for item in items]) if items else "",
            "maximum_pixels": max((int(item["pixels"]) for item in items), default=""),
        })
    return output


def plots(curves: list[dict[str, Any]], scaling: list[dict[str, Any]], directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    colors = {"n148i": "#c43c39", "jpeg": "#2f6fba", "jxl_e3": "#3a9d5d", "jxl_e7": "#7953a9"}
    labels = {"n148i": "N.148i", "jpeg": "JPEG turbo", "jxl_e3": "JPEG XL e3", "jxl_e7": "JPEG XL e7"}
    for metric, config in METRICS.items():
        fig, axis = plt.subplots(figsize=(7.2, 4.6))
        for codec in CODECS:
            points = sorted((row for row in curves if row["codec"] == codec), key=lambda row: row["bits_per_pixel"])
            axis.plot(
                [row["bits_per_pixel"] for row in points],
                [row[metric] for row in points], marker="o", linewidth=1.8,
                color=colors[codec], label=labels[codec],
            )
        axis.set_xlabel("bits per pixel")
        axis.set_ylabel(config["label"] + (" (lower is better)" if not config["higher"] else ""))
        axis.grid(True, alpha=0.25)
        axis.legend()
        fig.tight_layout()
        save_svg(fig, directory / f"rate-distortion-{metric}.svg")
        plt.close(fig)

    fig, axis = plt.subplots(figsize=(7.2, 4.6))
    for operation, color in (("encode", "#c43c39"), ("decode", "#2f6fba")):
        points = sorted((row for row in scaling if row["operation"] == operation), key=lambda row: row["threads"])
        axis.plot([row["threads"] for row in points], [row["speedup"] for row in points], marker="o", color=color, label=operation)
    maximum = max(int(row["threads"]) for row in scaling)
    axis.plot([1, maximum], [1, maximum], linestyle="--", color="#777777", label="ideal")
    axis.set_xlabel("threads")
    axis.set_ylabel("speedup")
    axis.set_xticks(sorted({int(row["threads"]) for row in scaling}))
    axis.grid(True, alpha=0.25)
    axis.legend()
    fig.tight_layout()
    save_svg(fig, directory / "n148-thread-scaling.svg")
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=root / "benchmarks/v1/final-results.csv")
    parser.add_argument("--samples", type=Path, default=root / "benchmarks/v1/timing-samples.csv")
    parser.add_argument("--manifest", type=Path, default=root / "benchmarks/v1/corpus-manifest.json")
    parser.add_argument("--output-dir", type=Path, default=root / "benchmarks/v1/analysis")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = load_csv(args.results.resolve())
    samples = load_csv(args.samples.resolve())
    with args.manifest.resolve().open(encoding="utf-8") as handle:
        document = json.load(handle)
    manifest = document["images"] if isinstance(document, dict) else document
    validation = validate_matrix(rows, manifest, samples)
    manifest_index = {int(item["image_id"]): item for item in manifest}
    axis_a = [row for row in rows if row["axis"] == "A_single_thread"]
    curves = aggregate_curves(axis_a)
    per_image_bd, bd_summaries = compute_bd_rates(axis_a, curves, manifest_index)
    timing_detail, timing_summaries = timing_analysis(rows)
    timing_points, timing_absolute_summaries = absolute_timing_analysis(rows)
    noise_axes = noise_by_axis(rows)
    timing_category_summaries, timing_category_tests = timing_category_analysis(
        timing_detail, manifest_index
    )
    scaling = thread_scaling(rows)
    category_summaries, category_tests = category_analysis(per_image_bd, manifest)
    agreement = agreement_analysis(per_image_bd)
    extremes = extreme_cases(per_image_bd)
    corpus = corpus_summary(manifest)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)

    tables: list[tuple[str, list[dict[str, Any]]]] = [
        ("rate-distortion.csv", curves),
        ("bd-rate-per-image.csv", per_image_bd),
        ("bd-rate-summary.csv", bd_summaries),
        ("timing-per-image.csv", timing_detail),
        ("timing-summary.csv", timing_summaries),
        ("timing-by-point.csv", timing_points),
        ("timing-absolute-summary.csv", timing_absolute_summaries),
        ("noise-by-axis.csv", noise_axes),
        ("timing-category-summary.csv", timing_category_summaries),
        ("timing-category-tests.csv", timing_category_tests),
        ("thread-scaling.csv", scaling),
        ("category-summary.csv", category_summaries),
        ("category-tests.csv", category_tests),
        ("metric-agreement.csv", agreement),
        ("extremes.csv", extremes),
        ("corpus-summary.csv", corpus),
    ]
    for filename, table in tables:
        if not table:
            raise ValueError(f"analysis table is empty: {filename}")
        atomic_csv(output / filename, list(table[0]), table)
    plots(curves, scaling, output / "plots")

    noise_failures = [row for row in rows if row["noise_target_met"].lower() != "true"]
    summary = {
        "validation": validation,
        "noise": {
            "target_pct": 1.0,
            "rows_above_target": len(noise_failures),
            "maximum_encode_noise_pct": max(f(row, "encode_noise_pct") for row in rows),
            "maximum_decode_noise_pct": max(f(row, "decode_noise_pct") for row in rows),
            "median_encode_cv_pct": statistics.median(f(row, "encode_cv_pct") for row in rows),
            "median_decode_cv_pct": statistics.median(f(row, "decode_cv_pct") for row in rows),
        },
        "bd_rate": bd_summaries,
        "timing": timing_summaries,
        "timing_absolute": timing_absolute_summaries,
        "noise_by_axis": noise_axes,
        "timing_category_tests": timing_category_tests,
        "thread_scaling": scaling,
        "metric_agreement": agreement,
        "category_tests": category_tests,
    }
    atomic_json(output / "summary.json", summary)
    print(json.dumps(summary["validation"], ensure_ascii=False, indent=2))
    print(json.dumps(summary["noise"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

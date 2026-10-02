#!/usr/bin/env python3
"""Paired, stratified image bootstrap for the frozen release comparison."""
from __future__ import annotations

import argparse
import csv
import json
import math
import hashlib
from pathlib import Path

import numpy as np

from benchmark_math import bd_rate
from release_benchmark import REPORT, CODECS, save

METRICS = {
    "psnr_y_db": "mse_y", "psnr_rgb_db": "mse_rgb", "ssim": None,
    "ms_ssim": None, "ssimulacra2": None, "butteraugli": None,
    "psnr_chroma_db": "mse_chroma",
}


def bootstrap_counts(categories, repetitions, seed):
    """Resample whole images within each stratum; reuse weights for all codecs."""
    rng = np.random.default_rng(seed)
    categories = np.asarray(categories)
    counts = np.zeros((repetitions, len(categories)), dtype=np.int32)
    for category in sorted(set(categories)):
        indices = np.flatnonzero(categories == category)
        draws = rng.choice(indices, size=(repetitions, len(indices)), replace=True)
        for rep in range(repetitions):
            np.add.at(counts[rep], draws[rep], 1)
    return counts


def interval(values, alpha=.05):
    finite = np.asarray([x for x in values if x is not None and math.isfinite(x)], dtype=float)
    if len(finite) == 0:
        return None
    return [float(x) for x in np.quantile(finite, [alpha / 2, 1 - alpha / 2])]


def curve(weights, pixels, sizes, values, mse=False, lower=False):
    """Aggregate with pixel weighting, converting pooled MSE to PSNR last."""
    weighted_pixels = weights * pixels
    denominator = weighted_pixels.sum()
    rates = 8 * (weights @ sizes) / denominator
    quality = (weighted_pixels @ values) / denominator
    if mse:
        with np.errstate(divide="ignore"):
            quality = 10 * np.log10(65025 / quality)
    elif lower:
        quality = -quality
    return list(zip(quality, rates))


def csv_write(path, rows):
    if not rows:
        raise ValueError("Cannot write empty result table")
    with Path(path).open("w") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def analyze_quality():
    protocol = json.loads((REPORT / "protocol.json").read_text())
    status = json.loads((REPORT / "quality-status.json").read_text())
    rows = list(csv.DictReader((REPORT / "quality.csv").open()))
    if hashlib.sha256((REPORT / "quality.csv").read_bytes()).hexdigest() != status["sha256"]:
        raise ValueError("Quality CSV digest differs from completion record")
    images = json.loads((REPORT / "corpus.json").read_text())["images"]
    n = len(images)
    keys = {(int(row["image_id"]), row["codec"], int(row["point"])): row for row in rows}
    expected = {(i["image_id"], codec, point) for i in images for codec in CODECS for point in range(1, 6)}
    if status["status"] != "complete" or set(keys) != expected or len(rows) != len(expected):
        raise ValueError("Quality matrix is incomplete or contains duplicate rows")
    pixels = np.asarray([i["width"] * i["height"] for i in images], dtype=float)
    settings = protocol["uncertainty"]
    weights = bootstrap_counts([i["category"] for i in images], settings["replicates"], settings["seed"])
    all_weights = np.vstack([np.ones(n), weights])
    denominators = all_weights @ pixels
    sizes = {codec: np.asarray([[float(keys[i["image_id"], codec, p]["bytes"]) for p in range(1, 6)] for i in images]) for codec in CODECS}
    rates = {codec: 8 * (all_weights @ values) / denominators[:, None] for codec, values in sizes.items()}
    pairs = [(codec, "webp_m6") for codec in CODECS if codec != "webp_m6"]
    pairs += [("n148i_v2", codec) for codec in CODECS if codec not in ("n148i_v2", "webp_m6")]
    summaries, per_image, aggregate = [], [], []
    for metric, mse_field in METRICS.items():
        values = {codec: np.asarray([[float(keys[i["image_id"], codec, p][mse_field or metric]) for p in range(1, 6)] for i in images]) for codec in CODECS}
        qualities = {}
        for codec in CODECS:
            qualities[codec] = (all_weights @ (values[codec] * pixels[:, None])) / denominators[:, None]
            if mse_field:
                with np.errstate(divide="ignore"):
                    qualities[codec] = 10 * np.log10(65025 / qualities[codec])
            elif metric == "butteraugli":
                qualities[codec] = -qualities[codec]
            for point in range(5):
                aggregate.append({"codec": codec, "metric": metric, "point": point + 1,
                                  "bpp": rates[codec][0, point], "quality_axis": qualities[codec][0, point]})
        for candidate, anchor in pairs:
            estimates = []
            for rep in range(len(all_weights)):
                result = bd_rate(list(zip(qualities[candidate][rep], rates[candidate][rep])),
                                 list(zip(qualities[anchor][rep], rates[anchor][rep])))
                estimates.append(result)
            point = estimates[0]
            boot = [result[0] if result else None for result in estimates[1:]]
            valid_bootstrap = sum(x is not None for x in boot)
            ci = interval(boot)
            simultaneous = interval(boot, alpha=.05 / len(METRICS))
            sensitivity = []
            for omitted in (1, 2, 3):
                selected = [p for p in range(5) if p != omitted]
                reduced = bd_rate(list(zip(qualities[candidate][0, selected], rates[candidate][0, selected])),
                                  list(zip(qualities[anchor][0, selected], rates[anchor][0, selected])))
                sensitivity.append({"omitted_point": omitted + 1, "bd_rate_pct": reduced[0] if reduced else None})
            image_values = []
            for index, item in enumerate(images):
                def image_curve(codec):
                    quality = values[codec][index]
                    if mse_field:
                        with np.errstate(divide="ignore"):
                            quality = 10 * np.log10(65025 / quality)
                    elif metric == "butteraugli": quality = -quality
                    return list(zip(quality, 8 * sizes[codec][index] / pixels[index]))
                result = bd_rate(image_curve(candidate), image_curve(anchor))
                per_image.append({"image_id": item["image_id"], "category": item["category"],
                                  "candidate": candidate, "anchor": anchor, "metric": metric,
                                  "bd_rate_pct": result[0] if result else "", "valid": result is not None})
                if result: image_values.append(result[0])
            summaries.append({"candidate": candidate, "anchor": anchor, "metric": metric,
                "bd_rate_pct": point[0] if point else None,
                "quality_overlap": list(point[1:]) if point else None,
                "ci95_pct": ci, "familywise95_pct": simultaneous,
                "bootstrap_valid": valid_bootstrap, "bootstrap_total": len(boot),
                "interior_point_omission": sensitivity,
                "images_valid": len(image_values), "images_missing": n - len(image_values),
                "images_won": sum(x < -1e-9 for x in image_values),
                "images_tied": sum(abs(x) <= 1e-9 for x in image_values),
                "images_lost": sum(x > 1e-9 for x in image_values),
                "median_image_bd_rate_pct": float(np.median(image_values)) if image_values else None,
                "conclusion": ("insufficient_bootstrap_overlap" if valid_bootstrap != len(boot) else
                               "lower_rate" if simultaneous and simultaneous[1] < 0 else
                               "higher_rate" if simultaneous and simultaneous[0] > 0 else "inconclusive")})
            print(f"{metric}: {candidate} / {anchor}: {point[0] if point else None}; CI {ci}", flush=True)
    save(REPORT / "quality-analysis.json", {"scope": protocol["scope"], "method": settings, "comparisons": summaries})
    csv_write(REPORT / "per-image-bd-rate.csv", per_image)
    csv_write(REPORT / "aggregate-curves.csv", aggregate)
    markdown = ["| Codec | Y | RGB | SSIM | MS-SSIM | SSIMULACRA2 | Butteraugli | Chroma |",
                "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for codec in CODECS:
        cells = []
        for metric in METRICS:
            if codec == "webp_m6": cells.append("0 (reference)"); continue
            entry = next(x for x in summaries if x["candidate"] == codec and x["anchor"] == "webp_m6" and x["metric"] == metric)
            value = entry["bd_rate_pct"]
            cells.append(f"{value:+.2f}%" if value is not None else "N/A")
        markdown.append("| " + codec + " | " + " | ".join(cells) + " |")
    (REPORT / "quality-table.md").write_text("\n".join(markdown) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    analyze_quality()


if __name__ == "__main__": main()

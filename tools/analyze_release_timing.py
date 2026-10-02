#!/usr/bin/env python3
"""Paired image/block/sample bootstrap of measured operating-point timings."""
from __future__ import annotations
import csv
import json
from pathlib import Path
import numpy as np
from analyze_release_benchmark import interval, csv_write
from release_benchmark import REPORT, CODECS, save, sha, load_matching


def timing_bootstrap(candidate, anchor, categories, count, seed):
    """Resample images in strata, then blocks/repetitions paired across codecs."""
    rng = np.random.default_rng(seed)
    categories = np.asarray(categories)
    strata = [np.flatnonzero(categories == cat) for cat in sorted(set(categories))]
    n, blocks, repetitions = candidate.shape
    results = []
    for offset in range(0, count, 128):
        batch = min(128, count - offset)
        indices = np.concatenate([rng.choice(s, size=(batch, len(s))) for s in strata], axis=1)
        selected_blocks = rng.integers(blocks, size=(batch, n, blocks, 1))
        selected_repetitions = rng.integers(repetitions, size=(batch, n, blocks, repetitions))
        a = candidate[indices[:, :, None, None], selected_blocks, selected_repetitions]
        b = anchor[indices[:, :, None, None], selected_blocks, selected_repetitions]
        # Each resampled image contributes one median time. Reusing exactly
        # the same indices on both sides preserves the experimental pairing.
        total_a = np.median(a, axis=(2, 3)).sum(axis=1)
        total_b = np.median(b, axis=(2, 3)).sum(axis=1)
        results.extend((100 * (total_a / total_b - 1)).tolist())
    return results


def main():
    protocol = json.loads((REPORT / "protocol.json").read_text())
    status = json.loads((REPORT / "timing-status.json").read_text())
    if status["status"] != "complete": raise RuntimeError("Timing matrix is incomplete")
    if status["sha256"] != sha(REPORT / "timing-samples.csv"):
        raise RuntimeError("Timing CSV digest differs from completion record")
    rows = list(csv.DictReader((REPORT / "timing-samples.csv").open()))
    groups = {}
    for row in rows:
        key = (int(row["image_id"]), row["mode"], row["level"], row["codec"])
        groups.setdefault(key, []).append(row)
    images = json.loads((REPORT / "corpus.json").read_text())["images"]
    matching = load_matching(images)
    expected_groups = {(image["image_id"], "nominal", level, codec) for image in images
                       for level in ("low", "medium", "high") for codec in CODECS}
    for image in images:
        expected_groups.update((image["image_id"], row["mode"], row["level"], row["codec"])
                               for row in matching[image["image_id"]]["selections"] if "codec" in row)
    if set(groups) != expected_groups:
        raise RuntimeError("Timing matrix has missing or unexpected operating points")
    blocks, repetitions = protocol["timing"]["blocks"], protocol["timing"]["repetitions_per_block"]
    for key, samples in groups.items():
        actual = {(int(r["block"]), int(r["index"])) for r in samples}
        expected = {(b, r) for b in range(blocks) for r in range(repetitions)}
        if actual != expected or len(samples) != len(expected):
            raise RuntimeError(f"Missing/duplicate samples: {key}")
        samples.sort(key=lambda r: (int(r["block"]), int(r["index"])))
    comparisons, per_image = [], []
    for mode in ("nominal", "bytes", "ssim"):
        for level in ("low", "medium", "high"):
            for reference in CODECS:
                if reference == "n148i_v2": continue
                pairs = []
                rejected = {"missing_common_range": 0, "unmatched_to_target": 0, "pairwise_tolerance": 0}
                for image in images:
                    a = groups.get((image["image_id"], mode, level, "n148i_v2"))
                    b = groups.get((image["image_id"], mode, level, reference))
                    if not a or not b:
                        rejected["missing_common_range"] += 1; continue
                    if mode != "nominal" and (a[0]["matched"] != "True" or b[0]["matched"] != "True"):
                        rejected["unmatched_to_target"] += 1; continue
                    rate_difference = float(a[0]["bytes"]) / float(b[0]["bytes"]) - 1
                    score_difference = float(a[0]["ssim"]) - float(b[0]["ssim"])
                    if ((mode == "bytes" and abs(rate_difference) > protocol["timing"]["size_tolerance_relative"]) or
                        (mode == "ssim" and abs(score_difference) > protocol["timing"]["ssim_tolerance_absolute"])):
                        rejected["pairwise_tolerance"] += 1; continue
                    pairs.append((image, a, b, rate_difference, score_difference))
                for operation in ("encode_ms", "decode_ms"):
                    if not pairs:
                        comparisons.append({"mode": mode, "level": level, "candidate": "n148i_v2", "anchor": reference,
                                            "operation": operation, "paired_images": 0, "excluded": rejected, "conclusion": "no_matched_pairs"})
                        continue
                    a = np.asarray([[float(s[operation]) for s in pair[1]] for pair in pairs]).reshape(-1, blocks, repetitions)
                    b = np.asarray([[float(s[operation]) for s in pair[2]] for pair in pairs]).reshape(-1, blocks, repetitions)
                    median_a, median_b = np.median(a, axis=(1, 2)), np.median(b, axis=(1, 2))
                    boot = timing_bootstrap(a, b, [p[0]["category"] for p in pairs],
                                            protocol["uncertainty"]["replicates"], protocol["uncertainty"]["seed"])
                    ci, joint = interval(boot), interval(boot, .05 / 6)
                    pixels = sum(p[0]["width"] * p[0]["height"] for p in pairs)
                    entry = {"mode": mode, "level": level, "candidate": "n148i_v2", "anchor": reference,
                        "operation": operation, "paired_images": len(pairs), "total_images": len(images), "excluded": rejected,
                        "time_difference_pct": float(100 * (median_a.sum() / median_b.sum() - 1)),
                        "ci95_pct": ci, "familywise95_pct": joint,
                        "candidate_mean_ms": float(median_a.mean()), "anchor_mean_ms": float(median_b.mean()),
                        "candidate_mpix_s": float(pixels / (1000 * median_a.sum())),
                        "anchor_mpix_s": float(pixels / (1000 * median_b.sum())),
                        "geometric_time_ratio": float(np.exp(np.log(median_a / median_b).mean())),
                        "size_difference_pct_range": [100 * min(p[3] for p in pairs), 100 * max(p[3] for p in pairs)],
                        "ssim_difference_range": [min(p[4] for p in pairs), max(p[4] for p in pairs)],
                        "conclusion": "faster" if joint[1] < 0 else "slower" if joint[0] > 0 else "inconclusive"}
                    comparisons.append(entry)
                    for index, pair in enumerate(pairs):
                        per_image.append({"image_id": pair[0]["image_id"], "mode": mode, "level": level,
                            "anchor": reference, "operation": operation, "candidate_median_ms": median_a[index],
                            "anchor_median_ms": median_b[index], "time_difference_pct": 100 * (median_a[index] / median_b[index] - 1),
                            "size_difference_pct": 100 * pair[3], "ssim_difference": pair[4]})
                    print(f"{mode}/{level}/{reference}/{operation}: {entry['time_difference_pct']:+.3f}% CI {ci}; n={len(pairs)}", flush=True)
    save(REPORT / "timing-analysis.json", {"method": "Paired stratified image bootstrap, nested paired resampling of two blocks and seven repetitions; 10000 replicates. Bonferroni six tests per reference/matching mode (three levels, two operations). Strict pairwise matching tolerance applied in addition to target tolerance.", "comparisons": comparisons})
    csv_write(REPORT / "per-image-timing.csv", per_image)
    common = []
    for item in images:
        entries = {codec: groups.get((item["image_id"], "bytes", "medium", codec)) for codec in CODECS}
        if not all(entries.values()): continue
        baseline_size = float(entries["webp_m6"][0]["bytes"])
        if all(samples[0]["matched"] == "True" and abs(float(samples[0]["bytes"]) / baseline_size - 1) <= protocol["timing"]["size_tolerance_relative"]
               for samples in entries.values()):
            common.append((item, entries))
    table = []
    if common:
        for codec in CODECS:
            entry = {"codec": codec, "images": len(common),
                     "total_bytes": sum(int(p[1][codec][0]["bytes"]) for p in common)}
            for operation in ("encode_ms", "decode_ms"):
                a = np.asarray([[float(r[operation]) for r in p[1][codec]] for p in common]).reshape(-1, blocks, repetitions)
                b = np.asarray([[float(r[operation]) for r in p[1]["webp_m6"]] for p in common]).reshape(-1, blocks, repetitions)
                med_a, med_b = np.median(a, axis=(1, 2)), np.median(b, axis=(1, 2))
                bootstrap = timing_bootstrap(a, b, [p[0]["category"] for p in common], protocol["uncertainty"]["replicates"], protocol["uncertainty"]["seed"])
                total_pixels = sum(p[0]["width"] * p[0]["height"] for p in common)
                entry[operation] = {"mean_ms": float(med_a.mean()), "mpix_s": float(total_pixels / (1000 * med_a.sum())),
                                    "time_difference_pct": float(100 * (med_a.sum() / med_b.sum() - 1)),
                                    "ci95_pct": interval(bootstrap), "familywise95_pct": interval(bootstrap, .05 / 14)}
            table.append(entry)
    save(REPORT / "common-size-timing.json", {"mode": "bytes", "level": "medium", "reference": "webp_m6",
        "scope": "Same subset for all eight codecs; every codec matches target and WebP M6 size within 2% per image. Conditional subset, not the full corpus.",
        "image_ids": [p[0]["image_id"] for p in common], "count": len(common), "total": len(images), "codecs": table})


if __name__ == "__main__": main()

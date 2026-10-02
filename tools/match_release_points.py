#!/usr/bin/env python3
"""Select actual comparable operating points before measuring their speed.

All probes are untimed. Preserve search traces and unmatched points. This
selects encoder parameters only; it never adjusts the frozen codec.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ProcessPoolExecutor, wait, FIRST_COMPLETED
import csv
import json
import math
from pathlib import Path
import subprocess
import tempfile
import time

import numpy as np
from PIL import Image
from skimage.metrics import structural_similarity

from benchmark_metrics import parse_driver
from release_benchmark import ROOT, REPORT, CODECS, command, save, verify


def parameter_key(value):
    return f"{value:.6f}"


def match_image(task):
    build, protocol, item, rows = task
    image_id = item["image_id"]
    output = REPORT / "matching" / f"{image_id:03d}.json"
    if output.exists():
        return json.loads(output.read_text())
    source = ROOT / "images" / item["filename"]
    with Image.open(source) as image:
        original = np.asarray(image.convert("RGB"), dtype=np.float64)
    cache = {codec: {} for codec in CODECS}
    for row in rows:
        parameter = float(row["parameter"])
        cache[row["codec"]][parameter_key(parameter)] = {
            "parameter": parameter, "bytes": int(row["bytes"]),
            "ssim": float(row["ssim"]), "origin": "quality_curve"}

    def probe(codec, value):
        value = round(value, 6)
        if codec.startswith("n148i") or codec == "jpeg_turbo":
            value = float(round(value))
        key = parameter_key(value)
        if key in cache[codec]:
            return cache[codec][key]
        temporary_parent = "/dev/shm" if Path("/dev/shm").is_dir() else None
        with tempfile.TemporaryDirectory(prefix="n148-match-", dir=temporary_parent) as directory:
            prefix = Path(directory) / "decoded"
            argv = command(build, codec, source, value, prefix=prefix)
            result = subprocess.run(argv, capture_output=True, text=True, timeout=600, check=True)
            parsed = parse_driver(result.stdout)
            native = CODECS[codec][1]
            with Image.open(str(prefix) + "-" + native + ".ppm") as image:
                reconstructed = np.asarray(image.convert("RGB"), dtype=np.float64)
            score = float(structural_similarity(original, reconstructed, channel_axis=2,
                          data_range=255., gaussian_weights=True, sigma=1.5,
                          use_sample_covariance=False))
            point = {"parameter": value, "bytes": parsed["sizes"][native], "ssim": score,
                     "origin": "search", "argv": argv[:-2]}
        cache[codec][key] = point
        return point

    selections = []
    for mode in ("bytes", "ssim"):
        # Compute ranges from the original five-point curves, never from
        # adaptively selected probes or from speed results.
        ranges = []
        for codec in CODECS:
            values = [float(row[mode]) for row in rows if row["codec"] == codec]
            ranges.append((min(values), max(values)))
        low = max(x[0] for x in ranges)
        high = min(x[1] for x in ranges)
        for level, fraction in (("low", .25), ("medium", .5), ("high", .75)):
            if high <= low:
                selections.append({"mode": mode, "level": level, "status": "no_common_range", "range": [low, high]})
                continue
            target = math.exp(math.log(low) + fraction * math.log(high / low)) if mode == "bytes" else low + fraction * (high - low)
            for codec in CODECS:
                tolerance = target * protocol["timing"]["size_tolerance_relative"] if mode == "bytes" else protocol["timing"]["ssim_tolerance_absolute"]
                integer = codec.startswith("n148i") or codec == "jpeg_turbo"
                direction = -1 if codec.startswith("jxl") else 1
                original_points = [p for p in cache[codec].values() if p["origin"] == "quality_curve"]
                min_control = min(direction * p["parameter"] for p in original_points)
                max_control = max(direction * p["parameter"] for p in original_points)
                tried = []
                for iteration in range(16):
                    points = sorted(cache[codec].values(), key=lambda p: direction * p["parameter"])
                    best = min(points, key=lambda p: (abs(p[mode] - target), p["parameter"]))
                    if abs(best[mode] - target) <= tolerance:
                        break
                    brackets = [(a, b) for a, b in zip(points, points[1:])
                                if a[mode] <= target <= b[mode]]
                    if not brackets:
                        break
                    a, b = min(brackets, key=lambda pair: abs(pair[0][mode] - target) + abs(pair[1][mode] - target))
                    left, right = direction * a["parameter"], direction * b["parameter"]
                    # Secant estimate, bounded away from an endpoint so every
                    # iteration also contracts the search interval.
                    ratio = (target - a[mode]) / (b[mode] - a[mode]) if b[mode] != a[mode] else .5
                    control = left + max(.2, min(.8, ratio)) * (right - left)
                    control = max(min_control, min(max_control, control))
                    if integer:
                        control = round(control)
                        if control <= left: control = left + 1
                        if control >= right: break
                    parameter = round(direction * control, 6)
                    if parameter_key(parameter) in cache[codec]: break
                    tried.append(parameter)
                    probe(codec, parameter)
                best = min(cache[codec].values(), key=lambda p: (abs(p[mode] - target), p["parameter"]))
                residual = best[mode] - target
                selections.append({"mode": mode, "level": level, "codec": codec,
                    "target": target, "range": [low, high], "parameter": best["parameter"],
                    "bytes": best["bytes"], "ssim": best["ssim"], "residual": residual,
                    "relative_size_residual": (best["bytes"] / target - 1) if mode == "bytes" else None,
                    "tolerance": tolerance, "matched": abs(residual) <= tolerance,
                    "status": "matched" if abs(residual) <= tolerance else "unmatched",
                    "probes": tried})
    result = {"image_id": image_id, "filename": item["filename"], "category": item["category"],
              "selections": selections, "probes": cache}
    save(output, result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--follow-quality", action="store_true",
                        help="Start each image only after all its frozen quality curves exist")
    args = parser.parse_args()
    protocol, build, images = verify()
    if args.follow_quality:
        completed, pending, submitted = set(), {}, set()
        with ProcessPoolExecutor(max_workers=args.workers) as pool:
            while len(completed) != len(images):
                for item in images:
                    image_id = item["image_id"]
                    if image_id in submitted or len(pending) >= args.workers: continue
                    paths = [REPORT / "quality" / f"{image_id:03d}-{codec}-{point}.json"
                             for codec in CODECS for point in range(1, 6)]
                    if not all(path.exists() for path in paths): continue
                    rows = [json.loads(path.read_text()) for path in paths]
                    future = pool.submit(match_image, (build, protocol, item, rows))
                    pending[future] = image_id
                    submitted.add(image_id)
                if not pending:
                    time.sleep(3)
                    continue
                done, _ = wait(pending, timeout=3, return_when=FIRST_COMPLETED)
                for future in done:
                    result = future.result()
                    completed.add(pending.pop(future))
                    successes = sum(p.get("matched", False) for p in result["selections"])
                    print(f"Matching {len(completed)}/{len(images)}: image {result['image_id']}, {successes}/48 within tolerance", flush=True)
        save(REPORT / "matching-status.json", {"status": "complete", "images": len(images)})
        return
    status = json.loads((REPORT / "quality-status.json").read_text())
    if status["status"] != "complete": raise RuntimeError("Complete quality curves first")
    rows = list(csv.DictReader((REPORT / "quality.csv").open()))
    tasks = [(build, protocol, item, [r for r in rows if int(r["image_id"]) == item["image_id"]]) for item in images]
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        for index, result in enumerate(pool.map(match_image, tasks), 1):
            successes = sum(p.get("matched", False) for p in result["selections"])
            print(f"Matching {index}/{len(images)}: {successes}/48 points within tolerance", flush=True)
    save(REPORT / "matching-status.json", {"status": "complete", "images": len(images)})


if __name__ == "__main__": main()

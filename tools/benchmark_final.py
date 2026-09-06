#!/usr/bin/env python3
"""Run the final N.148i/JPEG/JPEG XL benchmark matrix.

The C driver performs all timed work through direct in-process APIs. This
orchestrator preserves every timing sample, computes quality metrics outside
the timed region, attempts a robust <1% timing-noise target up to declared
per-axis sampling ceilings, and checkpoints atomically after each
image/quality point.

This extends the repository's benchmark_random_corpus.py workflow and keeps
its one-normalized-input-at-a-time, resumable and hash-checked design.

Copyright (c) Micilini Roll. Licensed under the MIT License.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

try:
    import numpy as np
    from PIL import Image as PillowImage
    from sewar.full_ref import msssim
    from skimage.metrics import structural_similarity
except ImportError as error:  # pragma: no cover - environment diagnostic
    raise SystemExit(
        "numpy, Pillow, scikit-image and sewar are required; see BENCHMARK.md setup"
    ) from error

from benchmark_random_corpus import now_iso, sha256_file


CODECS = ("n148i", "jpeg", "jxl_e3", "jxl_e7")
QUALITIES = (30, 45, 60, 75, 90)
JXL_DISTANCES = (8.0, 5.0, 3.0, 1.5, 0.7)
SUMMARY_FIELDS = [
    "axis", "image_id", "filename", "category", "width", "height", "pixels",
    "point", "codec", "quality", "jxl_distance", "jxl_effort", "chroma",
    "threads", "jxl_runner_workers", "cpu_set", "reps", "warmups",
    "sample_target_ms", "encode_inner_loops", "decode_inner_loops",
    "encode_median_ms", "encode_mean_ms", "encode_stdev_ms", "encode_iqr_ms",
    "encode_mad_ms", "encode_noise_pct", "encode_cv_pct",
    "decode_median_ms", "decode_mean_ms", "decode_stdev_ms", "decode_iqr_ms",
    "decode_mad_ms", "decode_noise_pct", "decode_cv_pct", "noise_target_met",
    "bytes", "bits_per_pixel", "psnr_rgb_db", "psnr_y_db", "ssim", "ms_ssim",
    "ssimulacra2", "butteraugli", "metrics_available", "driver_sha256",
]
SAMPLE_FIELDS = [
    "axis", "image_id", "filename", "point", "codec", "threads", "cpu_set",
    "sample", "encode_ms", "decode_ms", "encode_inner_loops",
    "decode_inner_loops",
]


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
        json.dump(value, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
        temporary = Path(handle.name)
    os.replace(temporary, path)


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def command_output(command: list[str]) -> str:
    try:
        result = subprocess.run(
            command, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=30
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    return result.stdout.strip()


def load_manifest(path: Path) -> list[dict[str, Any]]:
    with path.open(encoding="utf-8") as handle:
        document = json.load(handle)
    images = document.get("images") if isinstance(document, dict) else document
    if not isinstance(images, list) or not images:
        raise ValueError("empty or invalid corpus manifest")
    for expected, item in enumerate(images, 1):
        if int(item.get("image_id", -1)) != expected:
            raise ValueError(f"manifest order fails at image {expected}")
    return images


def physical_cpu_ids() -> list[int]:
    try:
        allowed = sorted(os.sched_getaffinity(0))
    except AttributeError:
        allowed = list(range(os.cpu_count() or 1))
    selected: list[int] = []
    seen: set[tuple[str, str]] = set()
    for cpu in allowed:
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        try:
            package = (topology / "physical_package_id").read_text().strip()
            core = (topology / "core_id").read_text().strip()
            identity = (package, core)
        except OSError:
            identity = ("logical", str(cpu))
        if identity not in seen:
            seen.add(identity)
            selected.append(cpu)
    return selected


def percentile(values: Iterable[float], probability: float) -> float:
    return float(np.percentile(np.asarray(list(values), dtype=np.float64), probability))


def timing_summary(values: list[float]) -> dict[str, float]:
    array = np.asarray(values, dtype=np.float64)
    median = float(np.median(array))
    mean = float(np.mean(array))
    stdev = float(np.std(array, ddof=1)) if len(array) > 1 else 0.0
    q1, q3 = np.percentile(array, (25.0, 75.0))
    mad = float(np.median(np.abs(array - median)))
    # Robust relative standard error of the median. 1.4826 makes MAD a
    # Gaussian-consistent scale estimate and 1.253314 is the asymptotic
    # Gaussian standard-error factor for a sample median.
    noise = (
        100.0 * 1.253314 * 1.4826 * mad /
        (median * math.sqrt(len(array)))
    )
    cv = 100.0 * stdev / mean if mean else math.inf
    return {
        "median": median,
        "mean": mean,
        "stdev": stdev,
        "iqr": float(q3 - q1),
        "mad": mad,
        "noise": noise,
        "cv": cv,
    }


def next_repetition_count(current: int, maximum: int) -> int:
    """Return the next 2**k-1 sampling tier, capped by the configured maximum."""
    target = current * 2 + 1
    exponent = math.ceil(math.log2(target + 1))
    tier = (1 << exponent) - 1
    return min(maximum, tier)


def parse_driver(stdout: str) -> dict[str, Any]:
    samples: dict[str, list[tuple[int, float, float]]] = defaultdict(list)
    calibration: dict[str, tuple[int, int]] = {}
    sizes: dict[str, int] = {}
    meta: list[str] | None = None
    for raw_line in stdout.splitlines():
        parts = raw_line.strip().split(",")
        if not parts or not parts[0]:
            continue
        if parts[0] == "meta" and len(parts) == 12:
            meta = parts[1:]
        elif parts[0] == "calibration" and len(parts) == 4:
            calibration[parts[1]] = (int(parts[2]), int(parts[3]))
        elif parts[0] == "sample" and len(parts) == 5:
            samples[parts[1]].append((int(parts[2]), float(parts[3]), float(parts[4])))
        elif parts[0] == "result" and len(parts) == 4:
            sizes[parts[1]] = int(parts[2])
    if meta is None or not samples or set(samples) != set(calibration) or set(samples) != set(sizes):
        raise RuntimeError(f"unrecognized/incomplete driver output: {stdout[:1000]!r}")
    for codec, codec_samples in samples.items():
        expected = list(range(len(codec_samples)))
        if [sample[0] for sample in codec_samples] != expected:
            raise RuntimeError(f"non-contiguous samples for {codec}")
    return {"meta": meta, "calibration": calibration, "samples": samples, "sizes": sizes}


def run_driver(
    driver: Path, image: Path, quality: int, distance: float, reps: int,
    warmups: int, sample_ms: float, n148_threads: int, jxl_workers: int,
    cpu_set: list[int], jpeg_cpu: int, codecs: str, output_prefix: Path | None,
) -> tuple[dict[str, Any], list[str]]:
    command = [
        "taskset", "-c", ",".join(map(str, cpu_set)), str(driver),
        "--input", str(image), "--n148-quality", str(quality),
        "--jpeg-quality", str(quality), "--jxl-distance", str(distance),
        "--reps", str(reps), "--warmups", str(warmups),
        "--sample-ms", str(sample_ms), "--chroma", "2",
        "--n148-threads", str(n148_threads), "--jxl-threads", str(jxl_workers),
        "--jpeg-cpu", str(jpeg_cpu), "--codecs", codecs,
    ]
    if output_prefix is not None:
        command += ["--output-prefix", str(output_prefix)]
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=3600, env=environment
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().replace("\n", " ")
        raise RuntimeError(f"driver exit {result.returncode}: {detail[:1500]}")
    return parse_driver(result.stdout), command


def psnr(mse: float) -> float:
    return math.inf if mse == 0.0 else 10.0 * math.log10(65025.0 / mse)


def external_metric(binary: Path, reference: Path, distorted: Path) -> float:
    result = subprocess.run(
        [str(binary), str(reference), str(distorted)], check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().replace("\n", " ")
        raise RuntimeError(f"{binary.name} failed: {detail[:600]}")
    first_line = result.stdout.strip().splitlines()[0]
    return float(first_line.strip())


def quality_metrics(
    reference_path: Path, distorted_path: Path, ssimulacra2: Path,
    butteraugli: Path,
) -> dict[str, float]:
    with PillowImage.open(reference_path) as image:
        reference = np.asarray(image.convert("RGB"), dtype=np.float64)
    with PillowImage.open(distorted_path) as image:
        distorted = np.asarray(image.convert("RGB"), dtype=np.float64)
    if reference.shape != distorted.shape:
        raise RuntimeError(
            f"metric dimensions differ: {reference.shape} vs {distorted.shape}"
        )
    difference = reference - distorted
    mse_rgb = float(np.mean(difference * difference))
    reference_y = (
        0.299 * reference[:, :, 0] + 0.587 * reference[:, :, 1] +
        0.114 * reference[:, :, 2]
    )
    distorted_y = (
        0.299 * distorted[:, :, 0] + 0.587 * distorted[:, :, 1] +
        0.114 * distorted[:, :, 2]
    )
    mse_y = float(np.mean((reference_y - distorted_y) ** 2))
    ssim = float(
        structural_similarity(
            reference, distorted, channel_axis=2, data_range=255.0,
            gaussian_weights=True, sigma=1.5, use_sample_covariance=False,
        )
    )
    ms_ssim = float(np.real(msssim(reference, distorted, MAX=255.0)))
    return {
        "psnr_rgb_db": psnr(mse_rgb),
        "psnr_y_db": psnr(mse_y),
        "ssim": ssim,
        "ms_ssim": ms_ssim,
        "ssimulacra2": external_metric(ssimulacra2, reference_path, distorted_path),
        "butteraugli": external_metric(butteraugli, reference_path, distorted_path),
    }


def result_key(row: dict[str, Any]) -> tuple[str, int, int, str, int]:
    return (
        str(row["axis"]), int(row["image_id"]), int(row["point"]),
        str(row["codec"]), int(row["threads"]),
    )


def codec_thread_count(phase: dict[str, Any], codec: str) -> int:
    """Return the reported thread count for one codec in a benchmark phase."""
    if codec == "jpeg":
        return 1
    if codec.startswith("jxl"):
        workers = int(phase["jxl_workers"])
        return workers if workers > 0 else 1
    return int(phase["threads"])


def make_phases(names: list[str], physical: list[int]) -> list[dict[str, Any]]:
    core_count = len(physical)
    phases: list[dict[str, Any]] = []
    if "axis_a" in names:
        phases.append({
            "axis": "A_single_thread", "threads": 1, "jxl_workers": 0,
            "cpu_set": physical[:1], "codecs": "all", "points": list(range(5)),
            "metrics": True,
        })
    if "axis_b" in names:
        phases.append({
            "axis": "B_throughput", "threads": core_count,
            "jxl_workers": core_count, "cpu_set": physical, "codecs": "all",
            "points": list(range(5)), "metrics": False,
        })
    if "sweep" in names:
        thread_counts = sorted({1, 2, 4, 8, core_count})
        for threads in thread_counts:
            if threads > core_count:
                continue
            phases.append({
                "axis": "N148_thread_sweep", "threads": threads,
                "jxl_workers": 0, "cpu_set": physical[:threads],
                "codecs": "n148i", "points": [2], "metrics": False,
            })
    return phases


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=root / "benchmarks/v1/corpus-manifest.json")
    parser.add_argument("--images-dir", type=Path, default=root / "images")
    parser.add_argument("--driver", type=Path, default=root / "benchmark-final")
    parser.add_argument("--output-dir", type=Path, default=root / "benchmarks" / "v1")
    parser.add_argument(
        "--ssimulacra2", type=Path,
        default=Path("/tmp/n148-libjxl-0.12.0/build/tools/ssimulacra2"),
    )
    parser.add_argument(
        "--butteraugli", type=Path,
        default=Path("/tmp/n148-libjxl-0.12.0/build/tools/butteraugli_main"),
    )
    parser.add_argument(
        "--phases", nargs="+", choices=("axis_a", "axis_b", "sweep"),
        default=["axis_a", "axis_b", "sweep"],
    )
    parser.add_argument("--reps", type=int, default=15)
    parser.add_argument("--max-reps", type=int, default=255)
    parser.add_argument(
        "--parallel-max-reps", type=int, default=31,
        help=(
            "sampling ceiling for throughput and thread-sweep axes; their "
            "scheduler variance can remain above the target even at 255 reps"
        ),
    )
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--sample-ms", type=float, default=20.0)
    parser.add_argument("--noise-target-pct", type=float, default=1.0)
    parser.add_argument("--limit-images", type=int)
    parser.add_argument(
        "--image-ids", type=int, nargs="+",
        help="diagnostic subset of manifested image IDs (incompatible with --limit-images)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if (
        args.reps < 3 or args.max_reps < args.reps or
        args.parallel_max_reps < args.reps or args.warmups < 0
    ):
        raise SystemExit("invalid repetition configuration")
    if args.limit_images is not None and args.image_ids:
        raise SystemExit("--limit-images and --image-ids are mutually exclusive")
    manifest_path = args.manifest.resolve()
    images_dir = args.images_dir.resolve()
    driver = args.driver.resolve()
    output_dir = args.output_dir.resolve()
    ssimulacra2 = args.ssimulacra2.resolve()
    butteraugli = args.butteraugli.resolve()
    for required in (driver, ssimulacra2, butteraugli):
        if not required.is_file() or not os.access(required, os.X_OK):
            raise SystemExit(f"missing executable: {required}")
    manifest = load_manifest(manifest_path)
    if args.limit_images is not None:
        manifest = manifest[: args.limit_images]
    elif args.image_ids:
        selected = set(args.image_ids)
        known = {int(item["image_id"]) for item in manifest}
        missing = sorted(selected - known)
        if missing:
            raise SystemExit(f"unknown --image-ids: {missing}")
        manifest = [item for item in manifest if int(item["image_id"]) in selected]
    for item in manifest:
        path = images_dir / item["filename"]
        if not path.is_file() or sha256_file(path) != item["ppm_sha256"]:
            raise SystemExit(f"missing or changed corpus file: {path}")

    physical = physical_cpu_ids()
    if not physical:
        raise SystemExit("no usable logical CPUs")
    phases = make_phases(args.phases, physical)
    run_config = {
        "manifest_sha256": sha256_file(manifest_path),
        "image_count": len(manifest),
        "image_ids": [int(item["image_id"]) for item in manifest],
        "phases": list(args.phases),
        "reps_initial": args.reps,
        "reps_maximum": args.max_reps,
        "parallel_reps_maximum": args.parallel_max_reps,
        "warmups": args.warmups,
        "sample_target_ms": args.sample_ms,
        "noise_target_pct": args.noise_target_pct,
        "physical_cpu_set": physical,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    work_dir = output_dir / ".work" / "final"
    work_dir.mkdir(parents=True, exist_ok=True)
    summary_path = output_dir / "final-results.csv"
    samples_path = output_dir / "timing-samples.csv"
    metadata_path = output_dir / "benchmark-metadata.json"
    summaries: list[dict[str, Any]] = load_csv(summary_path)
    samples: list[dict[str, Any]] = load_csv(samples_path)
    original_summary_count = len(summaries)
    original_sample_count = len(samples)
    # A pre-release resume-key bug represented JPEG as if it used the phase's
    # full thread count. If an interrupted run contains those duplicate rows,
    # retain the higher-repetition summary and the first (original) copy of
    # each numbered sample. This is deterministic and never selects by time.
    summary_by_key: dict[tuple[str, int, int, str, int], dict[str, Any]] = {}
    for row in summaries:
        key = result_key(row)
        previous = summary_by_key.get(key)
        if previous is None or int(row["reps"]) > int(previous["reps"]):
            summary_by_key[key] = row
    summaries = sorted(summary_by_key.values(), key=result_key)
    sample_by_key: dict[tuple[str, int, int, str, int, int], dict[str, Any]] = {}
    for row in samples:
        key = result_key(row) + (int(row["sample"]),)
        sample_by_key.setdefault(key, row)
    samples = sorted(
        sample_by_key.values(),
        key=lambda row: result_key(row) + (int(row["sample"]),),
    )
    removed_summary_duplicates = original_summary_count - len(summaries)
    removed_sample_duplicates = original_sample_count - len(samples)
    if removed_summary_duplicates:
        atomic_csv(summary_path, SUMMARY_FIELDS, summaries)
    if removed_sample_duplicates:
        atomic_csv(samples_path, SAMPLE_FIELDS, samples)
    completed = {result_key(row) for row in summaries}
    stable_completed = {
        result_key(row) for row in summaries
        if (
            str(row.get("noise_target_met", "")).lower() == "true" or
            int(row.get("reps", 0)) >= (
                args.max_reps
                if str(row.get("axis")) == "A_single_thread"
                else args.parallel_max_reps
            )
        )
    }
    driver_hash = sha256_file(driver)

    metadata: dict[str, Any]
    if metadata_path.exists():
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if metadata.get("driver_sha256") != driver_hash:
            raise SystemExit("driver changed since this run began")
        metadata_changed = False
        for key, value in run_config.items():
            if metadata.get(key) == value:
                continue
            if key == "parallel_reps_maximum" and key not in metadata:
                metadata[key] = int(value)
                metadata.setdefault("protocol_amendments", []).append({
                    "at": now_iso(),
                    "change": "parallel sampling ceiling set to 31 repetitions",
                    "reason": (
                        "pilot throughput points remained above 1% robust "
                        "median uncertainty after 255 repetitions; preserve "
                        "the observed residual rather than run an unbounded "
                        "non-converging refinement"
                    ),
                    "completed_point_ordinal": 613,
                })
                metadata_changed = True
                continue
            if (
                key == "reps_maximum" and
                int(value) > int(metadata.get(key, 0))
            ):
                history = metadata.setdefault(
                    "reps_maximum_history", [int(metadata.get(key, 0))]
                )
                if int(value) not in history:
                    history.append(int(value))
                metadata[key] = int(value)
                metadata["noise_refinement_started_at"] = now_iso()
                metadata_changed = True
                continue
            raise SystemExit(f"run configuration changed at {key}")
        if (
            (removed_summary_duplicates or removed_sample_duplicates) and
            not metadata.get("resume_key_repair")
        ):
            metadata["resume_key_repair"] = {
                "at": now_iso(),
                "summary_duplicates_removed": removed_summary_duplicates,
                "sample_duplicates_removed": removed_sample_duplicates,
                "selection_rule": (
                    "highest repetition summary; first original numbered sample; "
                    "never selected by measured duration"
                ),
                "affected_points": 7,
            }
            metadata_changed = True
        if metadata_changed:
            atomic_json(metadata_path, metadata)
    else:
        metadata = {
            "status": "running",
            "started_at": now_iso(),
            "manifest": str(manifest_path),
            **run_config,
            "qualities": list(QUALITIES),
            "jxl_distances": list(JXL_DISTANCES),
            "chroma": "4:2:0",
            "noise_definition": (
                "100 * 1.253314 * 1.4826 * MAD / (median * sqrt(n))"
            ),
            "driver": str(driver),
            "driver_sha256": driver_hash,
            "ssimulacra2": str(ssimulacra2),
            "butteraugli": str(butteraugli),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "numpy": np.__version__,
            "pillow": getattr(sys.modules.get("PIL"), "__version__", "unknown"),
            "scikit_image": command_output([
                sys.executable, "-c", "import skimage; print(skimage.__version__)"
            ]),
            "sewar": command_output([
                sys.executable, "-c",
                "import importlib.metadata; print(importlib.metadata.version('sewar'))",
            ]),
            "commands": [],
            "rerun_points": 0,
        }
        atomic_json(metadata_path, metadata)

    total_points = sum(len(phase["points"]) * len(manifest) for phase in phases)
    ordinal = 0
    for phase in phases:
        # Rotate quality order by image to distribute frequency/thermal drift.
        for image_index, item in enumerate(manifest):
            point_order = list(phase["points"])
            if len(point_order) > 1:
                rotation = image_index % len(point_order)
                point_order = point_order[rotation:] + point_order[:rotation]
            for point_index in point_order:
                ordinal += 1
                quality = QUALITIES[point_index]
                distance = JXL_DISTANCES[point_index]
                expected_codecs = CODECS if phase["codecs"] == "all" else ("n148i",)
                expected_keys = {
                    (phase["axis"], int(item["image_id"]), point_index + 1,
                     codec, codec_thread_count(phase, codec))
                    for codec in expected_codecs
                }
                # A partially noisy point is rerun as a whole, preserving the
                # within-process codec interleaving and paired design.
                if expected_keys <= stable_completed:
                    continue
                image_path = images_dir / item["filename"]
                prefix = work_dir / (
                    f"{phase['axis']}-{int(item['image_id']):04d}-p{point_index + 1}"
                )
                for stale in work_dir.glob(prefix.name + "-*.ppm"):
                    stale.unlink()
                previous_reps = [
                    int(row["reps"]) for row in summaries
                    if result_key(row) in expected_keys
                ]
                phase_max_reps = (
                    args.max_reps if phase["axis"] == "A_single_thread"
                    else args.parallel_max_reps
                )
                reps = args.reps
                if previous_reps:
                    # Refinement never replaces a noisy long run with a
                    # shorter run that happened to pass by chance.
                    reps = min(
                        phase_max_reps,
                        max(
                            args.reps,
                            next_repetition_count(
                                max(previous_reps), phase_max_reps
                            ),
                        ),
                    )
                parsed: dict[str, Any]
                command: list[str]
                point_commands: list[str] = []
                while True:
                    parsed, command = run_driver(
                        driver, image_path, quality, distance, reps,
                        args.warmups, args.sample_ms, int(phase["threads"]),
                        int(phase["jxl_workers"]), list(phase["cpu_set"]),
                        physical[0], str(phase["codecs"]),
                        prefix if phase["metrics"] else None,
                    )
                    point_commands.append(" ".join(command))
                    worst_noise = 0.0
                    for codec_samples in parsed["samples"].values():
                        worst_noise = max(
                            worst_noise,
                            timing_summary([sample[1] for sample in codec_samples])["noise"],
                            timing_summary([sample[2] for sample in codec_samples])["noise"],
                        )
                    if worst_noise <= args.noise_target_pct or reps >= phase_max_reps:
                        break
                    reps = next_repetition_count(reps, phase_max_reps)
                    metadata["rerun_points"] = int(metadata.get("rerun_points", 0)) + 1
                    print(
                        f"retry noise {worst_noise:.3f}%: {phase['axis']} "
                        f"{item['filename']} p{point_index + 1} with {reps} reps",
                        flush=True,
                    )
                metadata["commands"].extend(point_commands)

                new_summary: list[dict[str, Any]] = []
                new_samples: list[dict[str, Any]] = []
                for codec in expected_codecs:
                    observed = parsed["samples"][codec]
                    encode = timing_summary([sample[1] for sample in observed])
                    decode = timing_summary([sample[2] for sample in observed])
                    enc_inner, dec_inner = parsed["calibration"][codec]
                    metrics: dict[str, float | str] = {
                        "psnr_rgb_db": "", "psnr_y_db": "", "ssim": "",
                        "ms_ssim": "",
                        "ssimulacra2": "", "butteraugli": "",
                    }
                    metrics_available = False
                    reconstruction = Path(f"{prefix}-{codec}.ppm")
                    if phase["metrics"]:
                        if not reconstruction.is_file():
                            raise RuntimeError(f"missing reconstruction {reconstruction}")
                        metrics.update(
                            quality_metrics(
                                image_path, reconstruction, ssimulacra2, butteraugli
                            )
                        )
                        metrics_available = True
                    threads = codec_thread_count(phase, codec)
                    size = int(parsed["sizes"][codec])
                    row = {
                        "axis": phase["axis"],
                        "image_id": int(item["image_id"]),
                        "filename": item["filename"],
                        "category": item["category"],
                        "width": int(item["width"]),
                        "height": int(item["height"]),
                        "pixels": int(item["pixels"]),
                        "point": point_index + 1,
                        "codec": codec,
                        "quality": quality if codec in ("n148i", "jpeg") else "",
                        "jxl_distance": distance if codec.startswith("jxl") else "",
                        "jxl_effort": 3 if codec == "jxl_e3" else 7 if codec == "jxl_e7" else "",
                        "chroma": 2 if codec in ("n148i", "jpeg") else "native",
                        "threads": threads,
                        "jxl_runner_workers": int(phase["jxl_workers"]) if codec.startswith("jxl") else "",
                        "cpu_set": ",".join(map(str, phase["cpu_set"])),
                        "reps": len(observed),
                        "warmups": args.warmups,
                        "sample_target_ms": args.sample_ms,
                        "encode_inner_loops": enc_inner,
                        "decode_inner_loops": dec_inner,
                        "encode_median_ms": encode["median"],
                        "encode_mean_ms": encode["mean"],
                        "encode_stdev_ms": encode["stdev"],
                        "encode_iqr_ms": encode["iqr"],
                        "encode_mad_ms": encode["mad"],
                        "encode_noise_pct": encode["noise"],
                        "encode_cv_pct": encode["cv"],
                        "decode_median_ms": decode["median"],
                        "decode_mean_ms": decode["mean"],
                        "decode_stdev_ms": decode["stdev"],
                        "decode_iqr_ms": decode["iqr"],
                        "decode_mad_ms": decode["mad"],
                        "decode_noise_pct": decode["noise"],
                        "decode_cv_pct": decode["cv"],
                        "noise_target_met": (
                            encode["noise"] <= args.noise_target_pct and
                            decode["noise"] <= args.noise_target_pct
                        ),
                        "bytes": size,
                        "bits_per_pixel": size * 8.0 / int(item["pixels"]),
                        **metrics,
                        "metrics_available": metrics_available,
                        "driver_sha256": driver_hash,
                    }
                    new_summary.append(row)
                    for sample_index, enc_ms, dec_ms in observed:
                        new_samples.append({
                            "axis": phase["axis"],
                            "image_id": int(item["image_id"]),
                            "filename": item["filename"],
                            "point": point_index + 1,
                            "codec": codec,
                            "threads": threads,
                            "cpu_set": ",".join(map(str, phase["cpu_set"])),
                            "sample": sample_index,
                            "encode_ms": enc_ms,
                            "decode_ms": dec_ms,
                            "encode_inner_loops": enc_inner,
                            "decode_inner_loops": dec_inner,
                        })

                summaries = [row for row in summaries if result_key(row) not in expected_keys]
                samples = [
                    row for row in samples
                    if (str(row["axis"]), int(row["image_id"]), int(row["point"]),
                        str(row["codec"]), int(row["threads"])) not in expected_keys
                ]
                summaries.extend(new_summary)
                samples.extend(new_samples)
                summaries.sort(key=result_key)
                samples.sort(key=lambda row: result_key(row) + (int(row["sample"]),))
                atomic_csv(summary_path, SUMMARY_FIELDS, summaries)
                atomic_csv(samples_path, SAMPLE_FIELDS, samples)
                completed |= expected_keys
                stable_completed -= expected_keys
                stable_completed |= {
                    result_key(row) for row in new_summary
                    if (
                        bool(row["noise_target_met"]) or
                        int(row["reps"]) >= phase_max_reps
                    )
                }
                metadata["completed_summary_rows"] = len(summaries)
                metadata["completed_sample_rows"] = len(samples)
                metadata["last_completed_at"] = now_iso()
                atomic_json(metadata_path, metadata)
                for reconstruction in work_dir.glob(prefix.name + "-*.ppm"):
                    reconstruction.unlink()
                print(
                    f"[{ordinal}/{total_points}] {phase['axis']} "
                    f"{item['filename']} p{point_index + 1} reps={reps}",
                    flush=True,
                )

    metadata["status"] = "complete"
    metadata["completed_at"] = now_iso()
    metadata["completed_summary_rows"] = len(summaries)
    metadata["completed_sample_rows"] = len(samples)
    atomic_json(metadata_path, metadata)
    print(f"complete: {len(summaries)} summary rows, {len(samples)} samples")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

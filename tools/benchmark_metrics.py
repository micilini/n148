#!/usr/bin/env python3
"""Shared, fixed quality definitions and benchmark-output parsing.
Copyright (c) 2026 Micilini Roll. MIT License.
"""
from __future__ import annotations
from collections import defaultdict
import math
from pathlib import Path
import subprocess
from typing import Any
import numpy as np
from PIL import Image as PillowImage
from sewar.full_ref import msssim
from skimage.metrics import structural_similarity

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
    reference_cb = (
        -0.168736 * reference[:, :, 0] - 0.331264 * reference[:, :, 1] +
        0.5 * reference[:, :, 2]
    )
    distorted_cb = (
        -0.168736 * distorted[:, :, 0] - 0.331264 * distorted[:, :, 1] +
        0.5 * distorted[:, :, 2]
    )
    reference_cr = (
        0.5 * reference[:, :, 0] - 0.418688 * reference[:, :, 1] -
        0.081312 * reference[:, :, 2]
    )
    distorted_cr = (
        0.5 * distorted[:, :, 0] - 0.418688 * distorted[:, :, 1] -
        0.081312 * distorted[:, :, 2]
    )
    mse_y = float(np.mean((reference_y - distorted_y) ** 2))
    mse_cb = float(np.mean((reference_cb - distorted_cb) ** 2))
    mse_cr = float(np.mean((reference_cr - distorted_cr) ** 2))
    mse_chroma = 0.5 * (mse_cb + mse_cr)
    ssim = float(
        structural_similarity(
            reference, distorted, channel_axis=2, data_range=255.0,
            gaussian_weights=True, sigma=1.5, use_sample_covariance=False,
        )
    )
    ms_ssim = float(np.real(msssim(reference, distorted, MAX=255.0)))
    return {
        "mse_rgb": mse_rgb,
        "mse_y": mse_y,
        "mse_cb": mse_cb,
        "mse_cr": mse_cr,
        "mse_chroma": mse_chroma,
        "psnr_rgb_db": psnr(mse_rgb),
        "psnr_y_db": psnr(mse_y),
        "psnr_cb_db": psnr(mse_cb),
        "psnr_cr_db": psnr(mse_cr),
        "psnr_chroma_db": psnr(mse_chroma),
        "ssim": ssim,
        "ms_ssim": ms_ssim,
        "ssimulacra2": external_metric(ssimulacra2, reference_path, distorted_path),
        "butteraugli": external_metric(butteraugli, reference_path, distorted_path),
    }

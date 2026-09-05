#!/usr/bin/env python3
"""Recheck candidate/baseline codec outliers from a frozen corpus."""

from __future__ import annotations

import argparse
import csv
import os
import shutil
import tempfile
from pathlib import Path
from typing import Any

import benchmark_random_corpus as corpus


FIELDS = [
    "image_id",
    "pageid",
    "title",
    "quality",
    "width",
    "height",
    "reps",
    "candidate_n148_enc_ms",
    "candidate_jpeg_enc_ms",
    "candidate_enc_ratio",
    "baseline_n148_enc_ms",
    "baseline_jpeg_enc_ms",
    "baseline_enc_ratio",
    "enc_delta_pct",
    "enc_normalized_delta_pct",
    "candidate_n148_dec_ms",
    "candidate_n148_planes_ms",
    "candidate_n148_merge_ms",
    "candidate_n148_other_ms",
    "candidate_jpeg_dec_ms",
    "candidate_dec_ratio",
    "baseline_n148_dec_ms",
    "baseline_n148_planes_ms",
    "baseline_n148_merge_ms",
    "baseline_n148_other_ms",
    "baseline_jpeg_dec_ms",
    "baseline_dec_ratio",
    "dec_delta_pct",
    "dec_normalized_delta_pct",
    "n148_bytes",
    "jpeg_bytes",
    "ppm_sha256",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-compare", type=Path, required=True)
    parser.add_argument("--baseline-compare", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--comparison-csv", type=Path, required=True)
    parser.add_argument("--candidate-results", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reps", type=int, default=301)
    parser.add_argument("--ab-regression-threshold", type=float, default=1.0)
    parser.add_argument("--ab-decode-regression-threshold", type=float)
    parser.add_argument("--include-jpeg-encode-losses", action="store_true")
    parser.add_argument("--include-jpeg-decode-losses", action="store_true")
    parser.add_argument("--chroma", type=int, default=2)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--cpu", type=int, default=2)
    parser.add_argument("--max-dimension", type=int, default=1600)
    parser.add_argument("--max-download-mib", type=int, default=32)
    parser.add_argument("--min-free-gib", type=float, default=2.0)
    parser.add_argument("--download-attempts", type=int, default=8)
    parser.add_argument("--retry-base-seconds", type=float, default=5.0)
    return parser.parse_args()


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def atomic_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="", dir=path.parent,
        prefix=".atomic-", delete=False,
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
        temporary = Path(handle.name)
    os.replace(temporary, path)
    path.chmod(0o664)


def delta(candidate: float, baseline: float) -> float:
    return (candidate / baseline - 1.0) * 100.0


def main() -> int:
    args = parse_args()
    candidate_compare = args.candidate_compare.resolve()
    baseline_compare = args.baseline_compare.resolve()
    for label, binary in (
        ("candidate", candidate_compare),
        ("baseline", baseline_compare),
    ):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise SystemExit(f"binário {label} ausente ou não executável: {binary}")
    if args.reps < 1:
        raise SystemExit("reps deve ser positivo")
    same_binary = (
        corpus.sha256_file(candidate_compare)
        == corpus.sha256_file(baseline_compare)
    )
    if same_binary:
        print("candidate e baseline são binários idênticos; executando uma vez por ponto")

    manifest = corpus.load_json(args.manifest.resolve(), [])
    manifest_by_id = {int(item["image_id"]): item for item in manifest}
    comparison = {
        (int(row["image_id"]), int(row["quality"])): row
        for row in load_csv(args.comparison_csv.resolve())
    }
    candidate_results = {
        (int(row["image_id"]), int(row["quality"])): row
        for row in load_csv(args.candidate_results.resolve())
    }
    if comparison.keys() != candidate_results.keys():
        raise SystemExit("comparison.csv e results.csv têm matrizes diferentes")

    selected: set[tuple[int, int]] = {
        key
        for key, row in comparison.items()
        if float(row["enc_normalized_delta_pct"]) > args.ab_regression_threshold
    }
    if args.include_jpeg_encode_losses:
        selected.update(
            key
            for key, row in candidate_results.items()
            if float(row["n148_enc_ms"]) > float(row["jpeg_enc_ms"])
        )
    if args.ab_decode_regression_threshold is not None:
        selected.update(
            key
            for key, row in comparison.items()
            if float(row["dec_normalized_delta_pct"])
            > args.ab_decode_regression_threshold
        )
    if args.include_jpeg_decode_losses:
        selected.update(
            key
            for key, row in candidate_results.items()
            if float(row["n148_dec_ms"]) > float(row["jpeg_dec_ms"])
        )
    if not selected:
        raise SystemExit("nenhum ponto selecionado para rechecagem")

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = load_csv(output) if output.exists() else []
    completed = {(int(row["image_id"]), int(row["quality"])) for row in rows}
    unexpected = completed - selected
    if unexpected:
        raise SystemExit(f"saída contém pontos fora da seleção: {sorted(unexpected)[:5]}")

    work_dir = output.parent / ".recheck-work"
    work_dir.mkdir(exist_ok=True)
    source_path = work_dir / "source.download"
    ppm_path = work_dir / "input.ppm"
    source_path.unlink(missing_ok=True)
    ppm_path.unlink(missing_ok=True)
    maximum_download = args.max_download_mib * 1024 * 1024
    minimum_free = int(args.min_free_gib * 1024**3)

    by_image: dict[int, list[int]] = {}
    for image_id, quality in sorted(selected - completed):
        by_image.setdefault(image_id, []).append(quality)

    try:
        for image_number, image_id in enumerate(sorted(by_image), 1):
            item = manifest_by_id.get(image_id)
            if item is None:
                raise RuntimeError(f"imagem {image_id} ausente do manifesto")
            qualities = sorted(by_image[image_id])
            print(
                f"[{image_number:02d}/{len(by_image):02d}] ID {image_id:03d} "
                f"Q={','.join(map(str, qualities))} — {corpus.short_title(str(item['title']), 58)}",
                flush=True,
            )
            if shutil.disk_usage(output.parent).free < minimum_free:
                raise RuntimeError("espaço livre abaixo do limite antes do download")
            try:
                corpus.download(
                    str(item["download_url"]),
                    source_path,
                    maximum_download,
                    args.download_attempts,
                    args.retry_base_seconds,
                )
                width, height = corpus.convert_to_ppm(
                    source_path, ppm_path, args.max_dimension
                )
                ppm_sha256 = corpus.sha256_file(ppm_path)
                expected = (
                    int(item["width"]),
                    int(item["height"]),
                    str(item["ppm_sha256"]),
                )
                actual = (width, height, ppm_sha256)
                if actual != expected:
                    raise RuntimeError(
                        f"PPM congelado divergiu para ID {image_id}: {actual} != {expected}"
                    )

                for quality_index, quality in enumerate(qualities):
                    arguments = (
                        ppm_path,
                        quality,
                        args.reps,
                        args.chroma,
                        args.threads,
                        args.cpu,
                    )
                    if same_binary:
                        candidate_metrics = corpus.benchmark_point(
                            candidate_compare, *arguments
                        )
                        baseline_metrics = candidate_metrics
                    else:
                        baseline_metrics = None
                        if (image_id + quality_index) % 2 == 0:
                            baseline_metrics = corpus.benchmark_point(
                                baseline_compare, *arguments
                            )
                        candidate_metrics = corpus.benchmark_point(
                            candidate_compare, *arguments
                        )
                        if baseline_metrics is None:
                            baseline_metrics = corpus.benchmark_point(
                                baseline_compare, *arguments
                            )

                    candidate_enc_ratio = float(candidate_metrics["enc_ratio"])
                    baseline_enc_ratio = float(baseline_metrics["enc_ratio"])
                    candidate_dec_ratio = float(candidate_metrics["dec_ratio"])
                    baseline_dec_ratio = float(baseline_metrics["dec_ratio"])
                    if (
                        candidate_metrics["n148_bytes"] != baseline_metrics["n148_bytes"]
                        or candidate_metrics["n148_psnr_db"]
                        != baseline_metrics["n148_psnr_db"]
                    ):
                        raise RuntimeError(
                            f"saída N.148i divergiu na rechecagem ID {image_id} Q{quality}"
                        )
                    rows.append(
                        {
                            "image_id": image_id,
                            "pageid": item["pageid"],
                            "title": item["title"],
                            "quality": quality,
                            "width": width,
                            "height": height,
                            "reps": args.reps,
                            "candidate_n148_enc_ms": candidate_metrics["n148_enc_ms"],
                            "candidate_jpeg_enc_ms": candidate_metrics["jpeg_enc_ms"],
                            "candidate_enc_ratio": candidate_enc_ratio,
                            "baseline_n148_enc_ms": baseline_metrics["n148_enc_ms"],
                            "baseline_jpeg_enc_ms": baseline_metrics["jpeg_enc_ms"],
                            "baseline_enc_ratio": baseline_enc_ratio,
                            "enc_delta_pct": delta(
                                float(candidate_metrics["n148_enc_ms"]),
                                float(baseline_metrics["n148_enc_ms"]),
                            ),
                            "enc_normalized_delta_pct": delta(
                                candidate_enc_ratio, baseline_enc_ratio
                            ),
                            "candidate_n148_dec_ms": candidate_metrics["n148_dec_ms"],
                            "candidate_n148_planes_ms": candidate_metrics["n148_planes_ms"],
                            "candidate_n148_merge_ms": candidate_metrics["n148_merge_ms"],
                            "candidate_n148_other_ms": candidate_metrics["n148_other_ms"],
                            "candidate_jpeg_dec_ms": candidate_metrics["jpeg_dec_ms"],
                            "candidate_dec_ratio": candidate_dec_ratio,
                            "baseline_n148_dec_ms": baseline_metrics["n148_dec_ms"],
                            "baseline_n148_planes_ms": baseline_metrics["n148_planes_ms"],
                            "baseline_n148_merge_ms": baseline_metrics["n148_merge_ms"],
                            "baseline_n148_other_ms": baseline_metrics["n148_other_ms"],
                            "baseline_jpeg_dec_ms": baseline_metrics["jpeg_dec_ms"],
                            "baseline_dec_ratio": baseline_dec_ratio,
                            "dec_delta_pct": delta(
                                float(candidate_metrics["n148_dec_ms"]),
                                float(baseline_metrics["n148_dec_ms"]),
                            ),
                            "dec_normalized_delta_pct": delta(
                                candidate_dec_ratio, baseline_dec_ratio
                            ),
                            "n148_bytes": candidate_metrics["n148_bytes"],
                            "jpeg_bytes": candidate_metrics["jpeg_bytes"],
                            "ppm_sha256": ppm_sha256,
                        }
                    )
                    rows.sort(key=lambda row: (int(row["image_id"]), int(row["quality"])))
                    atomic_csv(output, rows)
            finally:
                source_path.unlink(missing_ok=True)
                ppm_path.unlink(missing_ok=True)
    finally:
        source_path.unlink(missing_ok=True)
        ppm_path.unlink(missing_ok=True)
        try:
            work_dir.rmdir()
        except OSError:
            pass

    print(f"Concluído: {len(rows)}/{len(selected)} pontos rechecados em {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

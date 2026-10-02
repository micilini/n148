#!/usr/bin/env python3
"""Frozen release comparison: quality first, then isolated in-memory timing."""
from __future__ import annotations

import argparse
from concurrent.futures import ProcessPoolExecutor
import csv
from datetime import datetime, timezone
import hashlib
import gzip
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import subprocess
import tempfile

from benchmark_metrics import quality_metrics, parse_driver

ROOT = Path(__file__).resolve().parents[1]
REPORT = Path(os.environ.get("N148_BENCH_OUTPUT", str(ROOT / "benchmarks/release"))).resolve()
WORK = ROOT / ".build/v2-release"
CODECS = {
    "n148i_v1": ("v1", "n148i"),
    "n148i_v2": ("current", "n148i"),
    "jpeg_turbo": ("current", "jpeg"),
    "webp_m4": ("current", "webp_m4"),
    "webp_m6": ("current", "webp_m6"),
    "webp_m6_historical": ("historical", "webp_m6"),
    "jxl_e3": ("current", "jxl_e3"),
    "jxl_e7": ("current", "jxl_e7"),
}
QUALITIES = [30, 45, 60, 75, 90]
DISTANCES = [8.0, 5.0, 3.0, 1.5, 0.7]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def now():
    return datetime.now(timezone.utc).isoformat()


def load_matching(images):
    """Read live checkpoints or the compact, published matching audit."""
    directory = REPORT / "matching"
    if directory.is_dir():
        rows = [json.loads((directory / f'{item["image_id"]:03d}.json').read_text())
                for item in images]
    else:
        with gzip.open(REPORT / "matching-audit.jsonl.gz", "rt") as handle:
            rows = [json.loads(line) for line in handle]
    result = {row["image_id"]: row for row in rows}
    if len(rows) != len(images) or set(result) != {item["image_id"] for item in images}:
        raise RuntimeError("Missing or duplicate matching records")
    return result


def linked_libraries(build):
    """Resolve all shared dependencies, including those of metric tools."""
    libraries = {}
    for binary in [*build["drivers"].values(), *build["metrics"].values()]:
        output = subprocess.check_output(["ldd", binary], text=True)
        if "not found" in output:
            raise RuntimeError("Unresolved shared dependency: " + binary)
        for line in output.splitlines():
            match = re.search(r"(?:=>\s*)?(/.+?)\s+\(0x[0-9a-f]+\)", line)
            if match:
                path = match.group(1)
                libraries[path] = sha(path)
    return libraries


def command(build, codec, source, parameter, *, reps=1, warmups=0,
            sample_ms=0.1, prefix=None, cpu=None):
    group, name = CODECS[codec]
    quality = 60 if codec.startswith("jxl") else parameter
    argv = [build["drivers"][group], "--input", str(source), "--codecs", name,
            "--n148-quality", str(int(quality)), "--jpeg-quality", str(int(quality)),
            "--webp-quality", str(quality), "--jxl-distance",
            str(parameter if codec.startswith("jxl") else 3),
            "--n148-effort", "3", "--n148-feature-flags", "0xd8a6d7ff",
            "--n148-chroma-quality", "0", "--chroma", "2",
            "--n148-threads", "1", "--webp-thread-level", "0", "--jxl-threads", "0",
            "--reps", str(reps), "--warmups", str(warmups), "--sample-ms", str(sample_ms)]
    if prefix:
        argv += ["--output-prefix", str(prefix)]
    if cpu is not None:
        argv = ["taskset", "-c", str(cpu), *argv]
    return argv


def verify():
    protocol = json.loads((REPORT / "protocol.json").read_text())
    if (protocol["codecs"] != {key: list(value) for key, value in CODECS.items()} or
        protocol["qualities"] != QUALITIES or protocol["jxl_distances"] != DISTANCES):
        raise RuntimeError("Measurement settings differ from the frozen protocol")
    build = json.loads((WORK / "build.json").read_text())
    frozen_build = json.loads((REPORT / "build.json").read_text())
    for name, item in frozen_build["files"].items():
        if build["files"].get(name, {}).get("sha256") != item["sha256"]:
            raise RuntimeError("Build differs from this run's freeze: " + name +
                               "; use a new N148_BENCH_OUTPUT directory")
    for name, digest in protocol["source_sha256"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("Frozen source changed: " + name)
    for item in build["files"].values():
        if sha(item["path"]) != item["sha256"]:
            raise RuntimeError("Binary/source archive changed: " + item["path"])
    linked = REPORT / "linked-library-sha256.json"
    if linked.exists():
        frozen_linkage = json.loads(linked.read_text())
        for path, digest in frozen_linkage.items():
            if sha(path) != digest:
                raise RuntimeError("Linked shared library changed: " + path)
        if linked_libraries(build) != frozen_linkage:
            raise RuntimeError("Runtime dependency resolution differs from the recorded libraries")
    images = json.loads((REPORT / "corpus.json").read_text())["images"]
    if sha(REPORT / "corpus.json") != protocol["corpus_sha256"]:
        raise RuntimeError("Frozen corpus manifest changed")
    for item in images:
        if sha(ROOT / "images" / item["filename"]) != item["ppm_sha256"]:
            raise RuntimeError("Image changed: " + item["filename"])
    return protocol, build, images


def freeze():
    if (REPORT / "protocol.json").exists():
        verify()
        print("Frozen protocol and inputs verified.")
        return
    images = json.loads((ROOT / "images/MANIFEST.json").read_text())["images"]
    if len(images) != 130 or len({x["ppm_sha256"] for x in images}) != 130:
        raise RuntimeError("Expected all 130 development images, without exact duplicates")
    save(REPORT / "corpus.json", {"scope": "development; all inputs used before release freeze", "images": images})
    build = json.loads((WORK / "build.json").read_text())
    save(REPORT / "build.json", build)
    save(REPORT / "linked-library-sha256.json", linked_libraries(build))
    configurations = WORK / "webp-configurations.json"
    if configurations.exists():
        save(REPORT / "webp-configurations.json", json.loads(configurations.read_text()))
    provenance = WORK / "jxl-provenance.json"
    if provenance.exists():
        save(REPORT / "jxl-provenance.json", json.loads(provenance.read_text()))
    sources = [p for directory in ("src", "include") for p in (ROOT / directory).glob("*")
               if p.suffix in (".c", ".h", ".inc")]
    sources += [ROOT / "Makefile", ROOT / "CMakeLists.txt", ROOT / "cmake/n148i.map"]
    protocol = {
        "schema": "n148i-release-protocol-v1", "frozen_utc": now(), "release": "V2",
        "corpus_sha256": sha(REPORT / "corpus.json"), "count": len(images),
        "scope": "Descriptive release benchmark on the complete development corpus; not an unseen holdout.",
        "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources},
        "codecs": CODECS, "qualities": QUALITIES, "jxl_distances": DISTANCES,
        "profile": {"features": "0xd8a6d7ff", "effort": 3, "chroma": "420", "chroma_quality": "luma", "threads": 1},
        "metrics": ["psnr_y_db", "psnr_rgb_db", "ssim", "ms_ssim", "ssimulacra2", "butteraugli", "psnr_chroma_db"],
        "quality": "Same definitions as quality_metrics(); all RGB reconstructions compared with original PPM. Pooled-MSE PSNR, pixel-weighted other metrics, PCHIP BD-rate over shared ranges only; no extrapolation.",
        "uncertainty": {"unit": "image", "paired": True, "stratify_by": "category", "replicates": 10000,
                        "seed": 14820261001, "interval": "percentile 95%", "joint_quality_claim": "Bonferroni 7 metrics per reference; two-sided family alpha 0.05", "limits": "Conditional on this development sample; does not remove tuning bias or estimate cross-host uncertainty."},
        "timing": {"cpu": min(os.sched_getaffinity(0)), "threads": 1, "blocks": 2, "repetitions_per_block": 7,
                   "recovery_unit": "all codecs at one image/mode/level within a block",
                   "warmups": 2, "sample_ms": 20, "order": "Seeded codec shuffle; second block reverses each first-block order",
                   "nominal_points": [30, 60, 90], "matched_modes": ["bytes", "ssim"],
                   "targets": "25%, 50%, 75% of per-image common curve range across all codecs; log scale for bytes, linear for SSIM",
                   "matching": "Bracketed parameter search inside measured endpoints; nearest achievable point; record all residuals and unmatched cases",
                   "size_tolerance_relative": 0.02, "ssim_tolerance_absolute": 0.0005,
                   "interference": "Wait on external QEMU/compiler jobs and CPU load guard; repeat all eight codecs at one image/mode/level if any before/after observation reports interference; preserve other completed groups. Never interrupt other jobs or discard based on which codec wins."},
        "hardware": "This host only; another processor remains pending.",
        "stopping": "Complete matrix and all declared timing points, independent of win/loss. No codec tuning during this benchmark.",
    }
    save(REPORT / "protocol.json", protocol)
    commands = [["uname", "-a"], ["lscpu"], ["cc", "--version"], ["cmake", "--version"],
                ["pkg-config", "--modversion", "libjpeg", "libwebp"]]
    environment = {"date": now(), "python": platform.python_version(), "platform": platform.platform(),
                   "affinity": sorted(os.sched_getaffinity(0)), "commands": {" ".join(c): subprocess.run(c, capture_output=True, text=True).stdout for c in commands}}
    for name in ("numpy", "scipy", "PIL", "skimage", "sewar"):
        module = __import__(name)
        environment[name] = getattr(module, "__version__", "see requirements")
    save(REPORT / "environment.json", environment)
    verify()
    print("Frozen: V1, V2 and six external profiles; all 130 development images.")


def measure_quality(task):
    build, item, codec, point = task
    parameter = (DISTANCES if codec.startswith("jxl") else QUALITIES)[point - 1]
    source = ROOT / "images" / item["filename"]
    base = "/dev/shm" if Path("/dev/shm").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="n148-quality-", dir=base) as directory:
        prefix = Path(directory) / "decoded"
        argv = command(build, codec, source, parameter, prefix=prefix)
        completed = subprocess.run(argv, capture_output=True, text=True, timeout=600, check=True)
        data = parse_driver(completed.stdout)
        native = CODECS[codec][1]
        reconstructed = Path(str(prefix) + "-" + native + ".ppm")
        metrics = quality_metrics(source, reconstructed, Path(build["metrics"]["ssimulacra2"]), Path(build["metrics"]["butteraugli_main"]))
        for key, value in metrics.items():
            if not math.isfinite(value):
                if key.startswith("psnr_") and value == math.inf:
                    metrics[key] = "inf"
                else:
                    raise RuntimeError(f"Invalid metric {key}: {value}")
        row = {"image_id": item["image_id"], "filename": item["filename"], "category": item["category"],
               "pixels": item["width"] * item["height"], "codec": codec, "point": point,
               "parameter": parameter, "bytes": data["sizes"][native],
               "decoded_sha256": sha(reconstructed), **metrics}
    path = REPORT / "quality" / f'{item["image_id"]:03d}-{codec}-{point}.json'
    save(path, row)
    return row


def quality(workers):
    protocol, build, images = verify()
    directory = REPORT / "quality"
    directory.mkdir(exist_ok=True)
    tasks = [(build, item, codec, point) for item in images for point in range(1, 6) for codec in CODECS
             if not (directory / f'{item["image_id"]:03d}-{codec}-{point}.json').exists()]
    total = len(images) * len(CODECS) * 5
    count = total - len(tasks)
    print(f"Quality {count}/{total}; {workers} workers; timings excluded.", flush=True)
    with ProcessPoolExecutor(max_workers=workers) as pool:
        for row in pool.map(measure_quality, tasks, chunksize=1):
            count += 1
            if count % 8 == 0 or count == total:
                print(f'Quality {count}/{total}: image {row["image_id"]}, {row["codec"]}, p{row["point"]}', flush=True)
    rows = [json.loads(path.read_text()) for path in sorted(directory.glob("*.json"))]
    if len(rows) != total:
        raise RuntimeError("Incomplete quality matrix")
    verify()
    with (REPORT / "quality.csv").open("w") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    save(REPORT / "quality-status.json", {"status": "complete", "rows": total, "completed_utc": now(), "sha256": sha(REPORT / "quality.csv")})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=["freeze", "verify", "quality"])
    parser.add_argument("--workers", type=int, default=2)
    args = parser.parse_args()
    if args.phase == "freeze": freeze()
    elif args.phase == "verify": verify(); print("Frozen inputs, sources and binaries verified.")
    else: quality(args.workers)


if __name__ == "__main__":
    main()

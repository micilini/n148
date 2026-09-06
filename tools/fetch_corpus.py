#!/usr/bin/env python3
"""Reconstruct the exact N.148i benchmark corpus from its manifest.

Each source download and normalized PPM is checked against the recorded
SHA-256. A missing or changed source is reported as an error and is never
silently substituted.

Copyright (c) Micilini Roll. Licensed under the MIT License.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


USER_AGENT = "N148-codec-benchmark/1.0 (reproducibility; github.com/micilini/n148)"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    with path.open(encoding="utf-8") as handle:
        document = json.load(handle)
    if isinstance(document, list):
        return {}, document
    if not isinstance(document, dict) or not isinstance(document.get("images"), list):
        raise ValueError("manifest must be a list or an object with an images list")
    return document, document["images"]


def converter_version(convert_binary: str) -> str:
    result = subprocess.run(
        [convert_binary, "-version"], check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=20
    )
    if result.returncode != 0 or not result.stdout.strip():
        raise RuntimeError("convert is unavailable")
    return result.stdout.splitlines()[0].strip()


def download(url: str, destination: Path, maximum_bytes: int) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    total = 0
    with urllib.request.urlopen(request, timeout=120) as response, destination.open("wb") as out:
        declared = response.headers.get("Content-Length")
        if declared and int(declared) > maximum_bytes:
            raise RuntimeError(
                f"declared download size {int(declared)} exceeds {maximum_bytes}"
            )
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > maximum_bytes:
                raise RuntimeError(f"download exceeds {maximum_bytes} bytes")
            out.write(chunk)
    if total == 0:
        raise RuntimeError("empty download")


def conversion_command(
    item: dict[str, Any], source: Path, output: Path, convert_binary: str
) -> list[str]:
    recorded = item.get("conversion_argv")
    if not isinstance(recorded, list) or not recorded:
        cap = int(item["resolution_cap"])
        recorded = [
            "convert", "SOURCE[0]", "-auto-orient", "-colorspace", "sRGB",
            "-resize", f"{cap}x{cap}>", "-background", "white", "-flatten",
            "-strip", "-depth", "8", f"PPM:images/{item['filename']}",
        ]
    command: list[str] = []
    for argument in recorded:
        argument = str(argument)
        if argument == "SOURCE[0]":
            command.append(f"{source}[0]")
        elif argument.startswith("PPM:images/"):
            command.append(f"PPM:{output}")
        else:
            command.append(argument)
    if command[0] != "convert":
        raise ValueError("manifest conversion command does not invoke convert")
    command[0] = convert_binary
    return command


def validate_ppm(path: Path, expected_width: int, expected_height: int) -> None:
    def token(handle: Any) -> bytes:
        while True:
            byte = handle.read(1)
            if not byte:
                raise ValueError("truncated PPM header")
            if byte == b"#":
                handle.readline()
            elif not byte.isspace():
                break
        value = bytearray(byte)
        while True:
            byte = handle.read(1)
            if not byte or byte.isspace():
                return bytes(value)
            value.extend(byte)

    with path.open("rb") as handle:
        magic = token(handle)
        width = int(token(handle))
        height = int(token(handle))
        maximum = int(token(handle))
        offset = handle.tell()
    if (magic, width, height, maximum) != (
        b"P6", expected_width, expected_height, 255
    ):
        raise ValueError(
            f"unexpected PPM header: {magic!r} {width}x{height} max={maximum}"
        )
    if path.stat().st_size != offset + width * height * 3:
        raise ValueError("PPM payload length is not exact")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest", type=Path,
        default=Path(__file__).resolve().parents[1] / "benchmarks" / "v1" / "corpus-manifest.json",
    )
    parser.add_argument(
        "--output-dir", type=Path,
        default=Path(__file__).resolve().parents[1] / "images",
    )
    parser.add_argument("--force", action="store_true", help="rebuild valid existing PPMs")
    parser.add_argument(
        "--convert-binary", default="convert",
        help="ImageMagick convert executable (default: convert)",
    )
    parser.add_argument("--max-download-mib", type=int, default=64)
    parser.add_argument("--only", type=int, nargs="*", help="optional image_id subset")
    parser.add_argument(
        "--politeness-seconds", type=float, default=5.0,
        help="pause after each source download (default: 5)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.max_download_mib < 1:
        raise SystemExit("--max-download-mib must be positive")
    metadata, images = load_manifest(args.manifest.resolve())
    current_converter = converter_version(args.convert_binary)
    recorded_converter = str(metadata.get("converter") or "")
    if recorded_converter and recorded_converter != current_converter:
        print(
            f"WARNING: converter differs; recorded={recorded_converter!r}, "
            f"current={current_converter!r}. Hash validation remains authoritative.",
            file=sys.stderr,
        )
    selected = set(args.only or [])
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    maximum_bytes = args.max_download_mib * 1024 * 1024
    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="n148-fetch-") as temporary_name:
        temporary = Path(temporary_name)
        source = temporary / "source.download"
        converted = temporary / "converted.ppm"
        for item in images:
            image_id = int(item["image_id"])
            if selected and image_id not in selected:
                continue
            filename = str(item["filename"])
            destination = output_dir / filename
            expected_hash = str(item["ppm_sha256"])
            if destination.is_file() and not args.force:
                try:
                    validate_ppm(
                        destination, int(item["width"]), int(item["height"])
                    )
                    if sha256(destination) == expected_hash:
                        print(f"OK existing {filename}")
                        continue
                except (OSError, ValueError) as error:
                    print(
                        f"WARNING: invalid existing {filename}: {error}",
                        file=sys.stderr,
                    )
                else:
                    print(
                        f"WARNING: replacing hash-mismatched existing {filename}",
                        file=sys.stderr,
                    )
            source.unlink(missing_ok=True)
            converted.unlink(missing_ok=True)
            try:
                download(str(item["download_url"]), source, maximum_bytes)
                expected_source = str(item.get("download_sha256") or "")
                actual_source = sha256(source)
                if expected_source and actual_source != expected_source:
                    raise RuntimeError(
                        f"source SHA-256 changed: expected {expected_source}, got {actual_source}"
                    )
                command = conversion_command(
                    item, source, converted, args.convert_binary
                )
                result = subprocess.run(
                    command, check=False, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, timeout=300
                )
                if result.returncode != 0:
                    raise RuntimeError(
                        "conversion failed: " + result.stderr.strip().replace("\n", " ")[:500]
                    )
                validate_ppm(converted, int(item["width"]), int(item["height"]))
                actual_hash = sha256(converted)
                if actual_hash != expected_hash:
                    raise RuntimeError(
                        f"PPM SHA-256 mismatch: expected {expected_hash}, got {actual_hash}"
                    )
                os.replace(converted, destination)
                print(f"OK rebuilt {filename}")
                time.sleep(max(0.0, args.politeness_seconds))
            except (OSError, ValueError, RuntimeError, urllib.error.URLError) as error:
                message = f"{filename}: {error}"
                failures.append(message)
                print(f"ERROR: {message}", file=sys.stderr)
                converted.unlink(missing_ok=True)
                if destination.is_file() and sha256(destination) != expected_hash:
                    destination.unlink()

    if failures:
        print(f"FAILED: {len(failures)} of {len(images)} requested images", file=sys.stderr)
        return 1
    print(f"PASS: {len(selected) if selected else len(images)} image(s) verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

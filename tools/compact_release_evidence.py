#!/usr/bin/env python3
"""Compact completed checkpoints while preserving the measurement evidence."""
from __future__ import annotations

import csv
import gzip
import hashlib
import json
from pathlib import Path
import shutil

from release_benchmark import REPORT, verify


def compact(directory, archive, wrap_filename=False):
    if not directory.exists():
        if not archive.exists():
            raise RuntimeError("Missing audit records: " + str(directory))
        return
    paths = sorted(directory.glob("*.json"))
    if not paths or any(p.suffix != ".json" for p in directory.iterdir()):
        raise RuntimeError("Incomplete checkpoint directory: " + str(directory))
    digests = []
    temporary = archive.with_suffix(archive.suffix + ".tmp")
    with temporary.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
        for path in paths:
            record = json.loads(path.read_text())
            if wrap_filename:
                record = {"checkpoint": path.name, "record": record}
            line = (json.dumps(record, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n").encode()
            compressed.write(line)
            digests.append(hashlib.sha256(line).hexdigest())
    with gzip.open(temporary, "rb") as handle:
        actual = [hashlib.sha256(line).hexdigest() for line in handle]
    if actual != digests:
        raise RuntimeError("Compressed audit roundtrip mismatch")
    temporary.replace(archive)
    shutil.rmtree(directory)


def main():
    verify()
    for phase in ("quality", "matching", "timing"):
        if json.loads((REPORT / f"{phase}-status.json").read_text())["status"] != "complete":
            raise RuntimeError("Refusing to compact an incomplete experiment")
    for result in ("quality-analysis.json", "timing-analysis.json", "common-size-timing.json", "rate-distortion.svg"):
        if not (REPORT / result).exists():
            raise RuntimeError("Finish analysis first: " + result)
    quality = REPORT / "quality"
    if quality.exists():
        with (REPORT / "quality.csv").open() as handle:
            rows = list(csv.DictReader(handle))
        expected = {(r["image_id"], r["codec"], r["point"]): r for r in rows}
        paths = list(quality.glob("*.json"))
        if len(paths) != len(rows):
            raise RuntimeError("Quality checkpoint/CSV counts differ")
        for path in paths:
            row = {key: str(value) for key, value in json.loads(path.read_text()).items()}
            if row != expected[row["image_id"], row["codec"], row["point"]]:
                raise RuntimeError("Quality checkpoint differs from CSV: " + str(path))
        shutil.rmtree(quality)
    compact(REPORT / "matching", REPORT / "matching-audit.jsonl.gz")
    compact(REPORT / "timing", REPORT / "timing-audit.jsonl.gz", wrap_filename=True)
    files = sorted(p for p in REPORT.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
    (REPORT / "SHA256SUMS").write_text("".join(
        hashlib.sha256(path.read_bytes()).hexdigest() + "  " + str(path.relative_to(REPORT)) + "\n" for path in files))
    print(f"Compacted audit records; {len(files)} evidence files checksummed.")


if __name__ == "__main__": main()

#!/usr/bin/env python3
"""Audit published inputs and complete raw matrices using only Python stdlib."""
from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, default=ROOT / "benchmarks/release")
    args = parser.parse_args()
    report = args.report.resolve()
    listed = set()
    for line in (report / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split("  ", 1)
        path = (report / name).resolve()
        require(path.is_relative_to(report), "Unsafe checksum path")
        require(name not in listed, "Duplicate checksum path")
        listed.add(name)
        require(sha(path) == digest, "Evidence digest mismatch: " + name)
    actual = {str(p.relative_to(report)) for p in report.rglob("*") if p.is_file() and p.name != "SHA256SUMS"}
    require(actual == listed, "Evidence files differ from checksum inventory")
    read = lambda name: json.loads((report / name).read_text())
    protocol, corpus = read("protocol.json"), read("corpus.json")
    require(sha(report / "corpus.json") == protocol["corpus_sha256"], "Corpus identity mismatch")
    images = corpus["images"]
    ids = {row["image_id"] for row in images}
    require(len(images) == len(ids) == protocol["count"], "Duplicate or missing images")
    require(len({row["ppm_sha256"] for row in images}) == len(images), "Duplicate image content")
    for row in images:
        require(sha(ROOT / "images" / row["filename"]) == row["ppm_sha256"], "Input image changed")
    for name, digest in protocol["source_sha256"].items():
        require(sha(ROOT / name) == digest, "Measured source changed: " + name)
    for name, digest in read("measurement-implementation.json")["current_tools_sha256"].items():
        require(sha(ROOT / name) == digest, "Reproduction tool changed: " + name)
    baseline = json.loads((ROOT / "benchmarks/baselines/provenance.json").read_text())
    require(sha(ROOT / "benchmarks/baselines/n148i-v1-source.tar.gz") == baseline["archive_sha256"], "V1 archive changed")
    with (report / "quality.csv").open() as handle:
        quality = list(csv.DictReader(handle))
    codecs = protocol["codecs"]
    keys = {(int(r["image_id"]), r["codec"], int(r["point"])) for r in quality}
    expected = {(image_id, codec, point) for image_id in ids for codec in codecs
                for point in range(1, len(protocol["qualities"]) + 1)}
    require(keys == expected and len(keys) == len(quality), "Quality matrix is incomplete or duplicated")
    require(sha(report / "quality.csv") == read("quality-status.json")["sha256"], "Quality completion digest changed")
    with gzip.open(report / "matching-audit.jsonl.gz", "rt") as handle:
        records = [json.loads(line) for line in handle]
    matching = {r["image_id"]: r for r in records}
    require(set(matching) == ids and len(records) == len(ids), "Matching matrix incomplete")
    groups = {(image_id, "nominal", level, codec) for image_id in ids
              for level in ("low", "medium", "high") for codec in codecs}
    for image_id, record in matching.items():
        groups.update((image_id, r["mode"], r["level"], r["codec"])
                      for r in record["selections"] if "codec" in r)
    with (report / "timing-samples.csv").open() as handle:
        samples = list(csv.DictReader(handle))
    timing = protocol["timing"]
    expected_samples = {key + (block, index) for key in groups for block in range(timing["blocks"])
                        for index in range(timing["repetitions_per_block"])}
    actual_samples = {(int(r["image_id"]), r["mode"], r["level"], r["codec"], int(r["block"]), int(r["index"])) for r in samples}
    require(actual_samples == expected_samples and len(samples) == len(expected_samples), "Timing matrix is incomplete or duplicated")
    for row in samples:
        require(all(math.isfinite(float(row[field])) and float(row[field]) > 0 for field in ("encode_ms", "decode_ms")), "Invalid timing")
    require(sha(report / "timing-samples.csv") == read("timing-status.json")["sha256"], "Timing completion digest changed")
    accepted, rejected, audit_samples = set(), 0, 0
    with gzip.open(report / "timing-audit.jsonl.gz", "rt") as handle:
        for line in handle:
            record = json.loads(line)["record"]
            if record["status"] != "accepted":
                rejected += 1
                continue
            key = (record["image_id"], record["block"])
            require(key not in accepted, "Duplicate accepted timing block")
            require(record["configuration"] == timing, "Timing audit configuration changed")
            group_audits = record["groups"]
            group_keys = {(g["mode"], g["level"]) for g in group_audits}
            expected_groups = {(mode, level) for image_id, mode, level, codec in groups if image_id == key[0]}
            require(group_keys == expected_groups and len(group_audits) == len(group_keys), "Missing/duplicate paired operating-point groups")
            for group in group_audits:
                require(group["status"] == "accepted" and not group["before"]["interference"]
                        and not group["after"]["interference"], "Noisy group was accepted")
                paired = [r for r in record["rows"] if (r["mode"], r["level"]) == (group["mode"], group["level"])]
                require(len(paired) == len(codecs) and {r["codec"] for r in paired} == set(codecs), "Partial codec pairing")
                require(all(not row[side]["interference"] for row in paired for side in ("before", "after")), "Noisy row was accepted")
            accepted.add(key)
            audit_samples += sum(len(r["samples"]) for r in record["rows"])
    require(accepted == {(i, b) for i in ids for b in range(timing["blocks"])}, "Timing audit has missing blocks")
    require(audit_samples == len(samples), "Timing audit/CSV sample counts differ")
    print(f"PASS: {len(listed)} evidence files, {len(ids)} unique inputs, {len(quality)} quality points, "
          f"{len(samples)} timing observations, {rejected} retained rejected blocks.")
    print("Digests and matrix integrity verified. This check does not rerun codecs or bootstrap calculations.")


if __name__ == "__main__": main()

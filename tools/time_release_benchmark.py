#!/usr/bin/env python3
"""Repeat actual nominal and matched operating points on one pinned CPU.

Do not run other benchmark or build jobs concurrently. External QEMU/build
jobs are awaited, never stopped. Raw samples include allocation and RGB
conversion inside the in-process library calls, excluding file I/O.
"""
from __future__ import annotations

import csv
import json
import os
from pathlib import Path
import random
import subprocess
import time

from benchmark_metrics import parse_driver
from benchmark_host import HostMonitor
from release_benchmark import ROOT, REPORT, CODECS, command, save, verify, now, sha, load_matching

HOST = HostMonitor()

def interference():
    return HOST.conflicts()


def snapshot(cpu):
    result = {"utc": now(), "loadavg": os.getloadavg(), "interference": interference()}
    result["observed_external_cpu_cores"] = HOST.external_cores
    path = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    for field in ("scaling_cur_freq", "scaling_governor"):
        try: result[field] = (path / field).read_text().strip()
        except OSError: result[field] = None
    for line in Path("/proc/stat").read_text().splitlines():
        if line.startswith(f"cpu{cpu} "): result["cpu_ticks"] = line.split()[1:]
    return result


def collect_group(tasks, run_task, observe, pause, record, first_attempt=0):
    """Retry all codecs at one operating point if any observation is noisy.

    Other operating points have separate paired comparisons and checkpoints.
    An interrupted group never contributes a partial set of codecs.
    """
    attempt = first_attempt
    while True:
        before = observe()
        while before["interference"]:
            pause()
            before = observe()
        attempt += 1
        rows, disturbed = [], False
        for task in tasks:
            if observe()["interference"]:
                disturbed = True
                break
            row = run_task(task)
            rows.append(row)
            if row["before"]["interference"] or row["after"]["interference"]:
                disturbed = True
                break
        after = observe()
        disturbed = disturbed or bool(after["interference"])
        audit = {"attempt": attempt, "before": before, "after": after,
                 "status": "interference" if disturbed else "accepted", "rows": rows}
        record(audit)
        if not disturbed:
            return audit


def validate_group(audit, tasks, configuration):
    fields = ("mode", "level", "codec", "parameter", "bytes", "ssim", "matched")
    expected = [[task[key] for key in fields] for task in tasks]
    actual = [[row[key] for key in fields] for row in audit["rows"]]
    observations = [audit["before"], audit["after"]]
    observations += [row[side] for row in audit["rows"] for side in ("before", "after")]
    if (audit["status"] != "accepted" or audit["configuration"] != configuration or
            actual != expected or any(s["interference"] for s in observations)):
        raise RuntimeError("Invalid paired operating-point checkpoint")


def main():
    protocol, build, images = verify()
    for name in ("quality", "matching"):
        if json.loads((REPORT / f"{name}-status.json").read_text())["status"] != "complete":
            raise RuntimeError("Finish untimed quality and matching first")
    if len(os.sched_getaffinity(0)) < 1: raise RuntimeError("No CPU available")
    configuration = protocol["timing"]
    cpu = configuration["cpu"]
    if cpu not in os.sched_getaffinity(0): raise RuntimeError("Frozen CPU is unavailable")
    interference()
    time.sleep(1)
    directory = REPORT / "timing"
    directory.mkdir(exist_ok=True)
    parts = REPORT / "timing-progress"
    parts.mkdir(exist_ok=True)
    if configuration.get("recovery_unit") != "all codecs at one image/mode/level within a block":
        raise RuntimeError("Timing protocol must declare the operating-point recovery unit")
    curves = list(csv.DictReader((REPORT / "quality.csv").open()))
    curve_keys = {(int(r["image_id"]), r["codec"], int(r["point"])): r for r in curves}
    matching = load_matching(images)
    rng = random.Random(protocol["uncertainty"]["seed"])
    schedule = []
    for item in images:
        selection = matching[item["image_id"]]
        groups = []
        for point, label in ((1, "low"), (3, "medium"), (5, "high")):
            entries = []
            for codec in CODECS:
                row = curve_keys[item["image_id"], codec, point]
                entries.append({"mode": "nominal", "level": label, "codec": codec,
                                "parameter": float(row["parameter"]), "bytes": int(row["bytes"]),
                                "ssim": float(row["ssim"]), "matched": False})
            groups.append(entries)
        for mode in ("bytes", "ssim"):
            for label in ("low", "medium", "high"):
                group = [s for s in selection["selections"] if s["mode"] == mode and s["level"] == label and "codec" in s]
                if group: groups.append(group)
        for group in groups:
            rng.shuffle(group)
        # The entire second pass reverses the first block's group and codec
        # orders, spreading thermals/order effects across every comparison.
        schedule.append((item, groups))
    for block in range(configuration["blocks"]):
        image_schedule = schedule if block == 0 else list(reversed(schedule))
        for image_index, (item, groups) in enumerate(image_schedule, 1):
            output = directory / f'{item["image_id"]:03d}-block{block}.json'
            if output.exists():
                for path in parts.glob(f'{item["image_id"]:03d}-block{block}-*.json'):
                    path.unlink()
                continue
            ordered_groups = groups if block == 0 else [list(reversed(g)) for g in reversed(groups)]
            completed = []
            for tasks in ordered_groups:
                if len(tasks) != len(CODECS) or {t["codec"] for t in tasks} != set(CODECS):
                    raise RuntimeError("A paired operating point must contain all codecs")
                mode, level = tasks[0]["mode"], tasks[0]["level"]
                key = f'{item["image_id"]:03d}-block{block}-{mode}-{level}'
                checkpoint = parts / (key + ".json")

                def run_task(task):
                    argv = command(build, task["codec"], ROOT / "images" / item["filename"], task["parameter"],
                                   reps=configuration["repetitions_per_block"], warmups=configuration["warmups"],
                                   sample_ms=configuration["sample_ms"], cpu=cpu)
                    start = snapshot(cpu)
                    result = subprocess.run(argv, capture_output=True, text=True, timeout=1800,
                                            env={**os.environ, "OMP_NUM_THREADS": "1", "OPENBLAS_NUM_THREADS": "1"}, check=True)
                    end = snapshot(cpu)
                    parsed = parse_driver(result.stdout)
                    native = CODECS[task["codec"]][1]
                    if parsed["sizes"][native] != task["bytes"]:
                        raise RuntimeError(f"Encoded size changed at frozen operating point: {item['image_id']} {task}")
                    samples = [{"index": s[0], "encode_ms": s[1], "decode_ms": s[2]} for s in parsed["samples"][native]]
                    return {**task, "image_id": item["image_id"], "category": item["category"],
                                 "pixels": item["width"] * item["height"], "block": block, "samples": samples,
                                 "encode_loops": parsed["calibration"][native][0], "decode_loops": parsed["calibration"][native][1],
                                 "before": start, "after": end, "argv": argv}

                def pause():
                    print("Waiting for external compute/build jobs; no process was interrupted.", flush=True)
                    time.sleep(30)

                def record(audit):
                    audit.update(image_id=item["image_id"], block=block, mode=mode, level=level,
                                 configuration=configuration, unit="paired_operating_point")
                    if audit["status"] == "accepted":
                        save(checkpoint, audit)
                    else:
                        save(directory / f'{key}-rejected{audit["attempt"]}.json', audit)
                        print(f"External interference in image {item['image_id']} {mode}/{level}; all eight codecs at this point will be repeated.", flush=True)

                if checkpoint.exists():
                    audit = json.loads(checkpoint.read_text())
                else:
                    attempts = [int(path.stem.rsplit("rejected", 1)[1]) for path in directory.glob(key + "-rejected*.json")]
                    audit = collect_group(tasks, run_task, lambda: snapshot(cpu), pause, record,
                                          first_attempt=max(attempts, default=0))
                validate_group(audit, tasks, configuration)
                completed.append(audit)
            rows = [row for group in completed for row in group["rows"]]
            save(output, {"image_id": item["image_id"], "block": block, "status": "accepted",
                          "unit": "image_block_with_individually_paired_operating_points",
                          "configuration": configuration, "before": completed[0]["before"],
                          "after": completed[-1]["after"], "rows": rows,
                          "groups": [{k: v for k, v in group.items() if k not in {"rows", "configuration"}}
                                     for group in completed]})
            for group in completed:
                (parts / f'{item["image_id"]:03d}-block{block}-{group["mode"]}-{group["level"]}.json').unlink()
            print(f"Timing block {block+1}/{configuration['blocks']}, image {image_index}/{len(images)} (id {item['image_id']})", flush=True)
    parts.rmdir()
    records = []
    for item in images:
        for block in range(configuration["blocks"]):
            audit = json.loads((directory / f'{item["image_id"]:03d}-block{block}.json').read_text())
            for row in audit["rows"]:
                for sample in row["samples"]:
                    records.append({k: row[k] for k in ("image_id", "category", "pixels", "codec", "mode", "level", "parameter", "bytes", "ssim", "matched", "block", "encode_loops", "decode_loops")} | sample)
    with (REPORT / "timing-samples.csv").open("w") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(records[0]), lineterminator="\n")
        writer.writeheader(); writer.writerows(records)
    verify()
    save(REPORT / "timing-status.json", {"status": "complete", "images": len(images), "samples": len(records), "completed_utc": now(), "sha256": sha(REPORT / "timing-samples.csv")})


if __name__ == "__main__": main()

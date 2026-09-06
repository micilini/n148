#!/usr/bin/env python3
"""Capture the final benchmark environment and mandatory correctness gates.

Copyright (c) Micilini Roll. Licensed under the MIT License.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


def run(
    command: list[str], timeout: int = 300,
    extra_env: dict[str, str] | None = None,
) -> dict[str, Any]:
    environment = {**os.environ, "LC_ALL": "C"}
    if extra_env:
        environment.update(extra_env)
    try:
        result = subprocess.run(
            command, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, timeout=timeout,
            env=environment,
        )
    except OSError as error:
        return {
            "argv": command,
            "exit_code": 127,
            "stdout": "",
            "stderr": str(error),
        }
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode(errors="replace") if isinstance(
            error.stdout, bytes
        ) else error.stdout or ""
        stderr = error.stderr.decode(errors="replace") if isinstance(
            error.stderr, bytes
        ) else error.stderr or f"timed out after {timeout} seconds"
        return {
            "argv": command,
            "exit_code": 124,
            "stdout": stdout,
            "stderr": stderr,
        }
    return {
        "argv": command,
        "exit_code": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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


def atomic_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, prefix=".atomic-", delete=False
    ) as handle:
        handle.write(value)
        handle.flush()
        os.fsync(handle.fileno())
        temporary = Path(handle.name)
    os.replace(temporary, path)


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=root)
    parser.add_argument("--jxl-source", type=Path, default=Path("/tmp/n148-libjxl-0.12.0"))
    parser.add_argument(
        "--convert-binary", type=Path,
        default=Path(shutil.which("convert") or "convert"),
    )
    parser.add_argument(
        "--ssimulacra2", type=Path,
        default=Path("/tmp/n148-libjxl-0.12.0/build/tools/ssimulacra2"),
    )
    parser.add_argument(
        "--butteraugli", type=Path,
        default=Path("/tmp/n148-libjxl-0.12.0/build/tools/butteraugli_main"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    benchmark_dir = root / "benchmarks" / "v1"
    compare = root / "compare"
    driver = root / "benchmark-final"
    validate = root / "validate"
    converter = args.convert_binary.resolve()
    converter_lib = converter.parents[1] / "lib" / "x86_64-linux-gnu"
    converter_env = {
        "LD_LIBRARY_PATH": str(converter_lib) + (
            ":" + os.environ["LD_LIBRARY_PATH"]
            if os.environ.get("LD_LIBRARY_PATH") else ""
        )
    }
    ssimulacra2 = args.ssimulacra2.resolve()
    butteraugli = args.butteraugli.resolve()
    for binary in (compare, driver, validate, converter, ssimulacra2, butteraugli):
        if not binary.is_file():
            raise SystemExit(f"missing binary: {binary}")

    packages = run(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n"])
    packages["stdout"] = "\n".join(
        line for line in packages["stdout"].splitlines()
        if re.search(r"lib(?:jpeg|jxl)", line, re.IGNORECASE)
    ) + "\n"
    commands = {
        "lscpu": run(["lscpu"]),
        "lscpu_topology": run(["lscpu", "-e=CPU,CORE,SOCKET,ONLINE,MAXMHZ,MINMHZ"]),
        "nproc": run(["nproc"]),
        "gcc": run(["gcc", "--version"]),
        "uname": run(["uname", "-a"]),
        "os_release": run(["lsb_release", "-ds"]),
        "packages": packages,
        "compare_ldd": run(["ldd", str(compare)]),
        "benchmark_final_ldd": run(["ldd", str(driver)]),
        "converter": run([str(converter), "-version"], extra_env=converter_env),
        "system_convert": run(["convert", "-version"]),
        "taskset": run(["taskset", "--version"]),
        "power_profile": run(["powerprofilesctl", "get"]),
        "ffmpeg_filters": run(["ffmpeg", "-filters"]),
        "python_metrics": run([
            sys.executable, "-c",
            "import importlib.metadata as m; "
            "print('numpy',m.version('numpy')); "
            "print('scipy',m.version('scipy')); "
            "print('scikit-image',m.version('scikit-image')); "
            "print('Pillow',m.version('Pillow')); "
            "print('sewar',m.version('sewar')); "
            "print('matplotlib',m.version('matplotlib'))",
        ]),
        "git_commit": run(["git", "rev-parse", "HEAD"]),
        "git_describe": run(["git", "describe", "--always", "--dirty"]),
        "libjxl_commit": run(["git", "-C", str(args.jxl_source), "rev-parse", "HEAD"]),
        "libjxl_describe": run(["git", "-C", str(args.jxl_source), "describe", "--tags", "--always"]),
        "libjxl_cmake_cache": run([
            "grep", "-E",
            "^(CMAKE_BUILD_TYPE|CMAKE_C_COMPILER:|CMAKE_CXX_COMPILER:|BUILD_SHARED_LIBS|JPEGXL_ENABLE_[A-Z0-9_]+):",
            str(args.jxl_source / "build" / "CMakeCache.txt"),
        ]),
        "libjxl_pkgconfig": run([
            "grep", "^Version:",
            str(args.jxl_source / "install" / "lib" / "pkgconfig" / "libjxl.pc"),
        ]),
        "ssimulacra2_help": run([str(ssimulacra2), "--help"]),
        "butteraugli_help": run([str(butteraugli), "--help"]),
    }
    flags: set[str] = set()
    try:
        cpuinfo = Path("/proc/cpuinfo").read_text(encoding="utf-8")
        for flag in ("sse2", "sse4_2", "avx", "avx2", "avx512f", "fma"):
            if re.search(rf"\b{re.escape(flag)}\b", cpuinfo):
                flags.add(flag)
    except OSError:
        pass
    governors: dict[str, str] = {}
    for cpu in range(os.cpu_count() or 1):
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor")
        try:
            governors[str(cpu)] = path.read_text(encoding="ascii").strip()
        except OSError:
            governors[str(cpu)] = "unavailable"

    sentinel = run([str(compare), "images/example.ppm", "50", "20"], timeout=300)
    validation = run([str(validate)], timeout=300)
    sentinel_match = re.search(r"N148i\s+enc.*?\s+(\d+) B\s+PSNR", sentinel["stdout"])
    sentinel_size = int(sentinel_match.group(1)) if sentinel_match else None
    gates = {
        "sentinel_exit_zero": sentinel["exit_code"] == 0,
        "sentinel_expected_bytes": 2290,
        "sentinel_actual_bytes": sentinel_size,
        "sentinel_pass": sentinel["exit_code"] == 0 and sentinel_size == 2290,
        "validation_exit_zero": validation["exit_code"] == 0,
        "validation_reports_pass": "validation PASS (0 failures)" in validation["stderr"],
        "validation_pass": (
            validation["exit_code"] == 0 and
            "validation PASS (0 failures)" in validation["stderr"] and
            ",FAIL" not in validation["stdout"]
        ),
    }
    document = {
        "captured_at": run(["date", "--iso-8601=seconds"])["stdout"].strip(),
        "platform_python": platform.python_version(),
        "cpu_flags": sorted(flags),
        "cpu_governors": governors,
        "binary_sha256": {
            "compare": sha256(compare),
            "benchmark-final": sha256(driver),
            "validate": sha256(validate),
            "converter": sha256(converter),
            "ssimulacra2": sha256(ssimulacra2),
            "butteraugli": sha256(butteraugli),
        },
        "metric_availability": {
            "psnr_rgb": True,
            "psnr_y": True,
            "ssim": True,
            "ms_ssim": True,
            "ssimulacra2": True,
            "butteraugli": True,
            "vmaf": False,
            "vmaf_reason": "installed ffmpeg has vmafmotion but no libvmaf filter",
        },
        "commands": commands,
        "correctness_gates": gates,
    }
    atomic_json(benchmark_dir / "environment.json", document)
    log = (
        "$ ./compare images/example.ppm 50 20\n" + sentinel["stdout"] +
        sentinel["stderr"] + "\n$ ./validate\n" + validation["stdout"] +
        validation["stderr"]
    )
    atomic_text(benchmark_dir / "validation.log", log)
    print(json.dumps(gates, ensure_ascii=False, indent=2))
    return 0 if all(gates.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())

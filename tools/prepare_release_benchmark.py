#!/usr/bin/env python3
"""Build isolated, version-pinned benchmark dependencies and API drivers.

Requires a C/C++ compiler, CMake, Git, pkg-config, libjpeg-turbo development
files and libbrotli development files. Nothing is installed system-wide.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WEBP_SHA = "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"
JXL_COMMIT = "a7a9c787341cf703dede03c2009fa460cae5e5df"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jxl-build", type=Path, help="Reuse a Release libjxl build; identity is recorded")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    deps = ROOT / ".benchmark-deps/final"
    work = ROOT / ".build/v2-release"
    deps.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(argv, cwd=ROOT):
        commands.append({"argv": list(map(str, argv)), "cwd": str(cwd)})
        with (work / "benchmark-build.log").open("a") as log:
            subprocess.run(list(map(str, argv)), cwd=cwd, check=True,
                           stdout=log, stderr=subprocess.STDOUT)

    archive = ROOT / "benchmarks/baselines/n148i-v1-source.tar.gz"
    provenance = json.loads((archive.parent / "provenance.json").read_text())
    if sha(archive) != provenance["archive_sha256"]:
        raise RuntimeError("Original V1 source archive differs from its recorded identity")
    baseline = deps / "v1"
    if not baseline.exists():
        baseline.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(baseline, filter="data")
    for item in provenance["files"]:
        if sha(baseline / item["path"]) != item["sha256"]:
            raise RuntimeError("Extracted V1 source changed: " + item["path"])
    run(["make", "-B", f"-j{args.jobs}", "libn148i.a",
         "CFLAGS=-O3 -Wall -Wextra -Wno-unused-parameter -pthread"], baseline)
    run(["make", "-B", f"-j{args.jobs}", "libn148i.a"])

    webp_archive = deps / "libwebp-1.6.0.tar.gz"
    url = "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz"
    if not webp_archive.exists():
        with urllib.request.urlopen(url, timeout=120) as response:
            webp_archive.write_bytes(response.read())
    if sha(webp_archive) != WEBP_SHA:
        raise RuntimeError("WebP source digest mismatch")
    webp_source = deps / "libwebp-1.6.0"
    if not webp_source.exists():
        with tarfile.open(webp_archive) as tar:
            tar.extractall(deps, filter="data")
    webp_prefix = deps / "webp"
    run(["cmake", "-S", webp_source, "-B", deps / "webp-build",
         "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON",
         *[f"-DWEBP_BUILD_{tool}=OFF" for tool in
           ("ANIM_UTILS", "CWEBP", "DWEBP", "GIF2WEBP", "IMG2WEBP", "VWEBP", "WEBPINFO", "WEBPMUX")],
         "-DCMAKE_INSTALL_PREFIX=" + str(webp_prefix)])
    run(["cmake", "--build", deps / "webp-build", "--parallel", args.jobs])
    run(["cmake", "--install", deps / "webp-build"])

    if args.jxl_build:
        jxl = args.jxl_build.resolve()
        source = jxl.parent
    else:
        source = deps / "libjxl"
        if not source.exists():
            run(["git", "clone", "--branch", "v0.12.0", "--depth", "1",
                 "--recurse-submodules", "--shallow-submodules",
                 "https://github.com/libjxl/libjxl.git", source])
        jxl = source / "build"
        run(["cmake", "-S", source, "-B", jxl, "-DCMAKE_BUILD_TYPE=Release",
             "-DBUILD_SHARED_LIBS=ON", "-DJPEGXL_ENABLE_TOOLS=ON",
             "-DJPEGXL_ENABLE_DEVTOOLS=ON", "-DJPEGXL_ENABLE_BENCHMARK=OFF",
             "-DJPEGXL_ENABLE_EXAMPLES=OFF", "-DJPEGXL_ENABLE_TESTS=OFF"])
        run(["cmake", "--build", jxl, "--parallel", args.jobs, "--target",
             "jxl", "jxl_threads", "ssimulacra2", "butteraugli_main"])
    actual_commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--short"], text=True).strip()
    if actual_commit != JXL_COMMIT or dirty:
        raise RuntimeError("Expected an unmodified official libjxl 0.12.0 source checkout")
    settings = {}
    for line in (jxl / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_FLAGS:", "CMAKE_C_FLAGS:", "JPEGXL_", "BUILD_SHARED_LIBS:")) and "=" in line:
            key, value = line.split("=", 1)
            settings[key] = value
    (work / "jxl-provenance.json").write_text(json.dumps({
        "release_tag": "v0.12.0", "commit": actual_commit, "working_tree_clean": True,
        "submodules": subprocess.check_output(["git", "-C", str(source), "submodule", "status", "--recursive"], text=True).strip().splitlines(),
        "cmake_configuration": settings,
    }, indent=2) + "\n")
    jxl_header = jxl / "lib/include/jxl/version.h"
    if "#define JPEGXL_MAJOR_VERSION 0" not in jxl_header.read_text() or \
       "#define JPEGXL_MINOR_VERSION 12" not in jxl_header.read_text() or \
       "#define JPEGXL_PATCH_VERSION 0" not in jxl_header.read_text():
        raise RuntimeError("Expected libjxl 0.12.0 headers")

    compiler = os.environ.get("CC", "cc")
    common = [compiler, "-O3", "-Wall", "-Wextra", "-Wno-unused-parameter",
              "-pthread", "-DN148I_STATIC_DEFINE"]
    driver = ROOT / "src/benchmark_final_cli.c"
    jxl_flags = ["-I" + str(jxl / "lib/include"), "-L" + str(jxl / "lib"),
                 "-ljxl", "-ljxl_threads", "-Wl,--disable-new-dtags",
                 "-Wl,-rpath," + str(jxl / "lib")]
    webp_flags = ["-I" + str(webp_prefix / "include"), "-L" + str(webp_prefix / "lib"),
                  "-lwebp", "-Wl,-rpath," + str(webp_prefix / "lib")]
    drivers = {}
    for label, flags in (("current", webp_flags), ("historical", ["-lwebp"])):
        target = work / ("benchmark-" + label)
        run([*common, "-Iinclude", "-Isrc", driver, "libn148i.a", "-o", target,
             "-lm", "-ljpeg", *jxl_flags, *flags])
        drivers[label] = str(target)
    configurations = {}
    for label, flags in (("current", webp_flags), ("historical", ["-lwebp"])):
        target = work / ("describe-webp-" + label)
        run([compiler, "-O2", ROOT / "tools/describe_webp.c", *flags, "-o", target])
        configurations[label] = json.loads(subprocess.check_output([str(target)], text=True))
    (work / "webp-configurations.json").write_text(json.dumps(configurations, indent=2) + "\n")
    target = work / "benchmark-v1"
    run([*common, "-DN148_BENCH_V1", "-DN148_BENCH_NO_JXL",
         "-I" + str(baseline / "include"), "-I" + str(baseline / "src"), driver,
         baseline / "libn148i.a", "-o", target, "-lm", "-ljpeg", "-lwebp"])
    drivers["v1"] = str(target)
    metrics = {name: str(jxl / "tools" / name) for name in ("ssimulacra2", "butteraugli_main")}
    files = {**drivers, **metrics,
             "v1_source": str(archive), "webp_source": str(webp_archive),
             "jxl": str(jxl / "lib/libjxl.so.0.12.0"),
             "jxl_threads": str(jxl / "lib/libjxl_threads.so.0.12.0"),
             "webp": str(webp_prefix / "lib/libwebp.so.7.2.0")}
    # Resolve the installed soname instead of assuming the package ABI suffix.
    files["webp"] = str((webp_prefix / "lib/libwebp.so").resolve())
    versions = subprocess.check_output(["pkg-config", "--modversion", "libjpeg", "libwebp"], text=True).splitlines()
    if versions[1] != "1.3.2":
        raise RuntimeError("Historical reference requires libwebp 1.3.2 development/runtime files; use the documented Ubuntu environment")
    metadata = {"schema": "n148i-benchmark-build-v1", "drivers": drivers,
                "metrics": metrics, "versions": {"n148i": "V2", "baseline": "V1",
                "jpeg_turbo": versions[0], "webp_historical": versions[1],
                "webp": "1.6.0", "jxl": "0.12.0"}, "commands": commands,
                "files": {k: {"path": v, "sha256": sha(v)} for k, v in files.items()},
                "linkage": {k: subprocess.check_output(["ldd", v], text=True) for k, v in drivers.items()},
                "compiler": subprocess.check_output([compiler, "--version"], text=True)}
    (work / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Drivers and dependency hashes saved:", work / "build.json")


if __name__ == "__main__":
    main()

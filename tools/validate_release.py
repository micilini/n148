#!/usr/bin/env python3
"""Run the N.148i release, ABI, installation, and consumer validation gates.

Copyright (c) Micilini Roll. Licensed under the MIT License.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any


PUBLIC_SYMBOLS = {
    "n148i_decode_memory",
    "n148i_encode_memory",
    "n148i_encode_options_init",
    "n148i_format_version",
    "n148i_free_buffer",
    "n148i_free_image",
    "n148i_library_version",
    "n148i_read_header",
    "n148i_result_string",
    "n148i_simd_force",
    "n148i_simd_level",
    "n148i_simd_name",
}


CONSUMER_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <n148i.h>

int main(void) {
    enum { WIDTH = 16, HEIGHT = 16 };
    uint8_t pixels[WIDTH * HEIGHT * 3];
    for (size_t index = 0; index < sizeof(pixels); index++)
        pixels[index] = (uint8_t) (index * 37u + 11u);
    n148i_image_t source = {pixels, WIDTH, HEIGHT, WIDTH * 3};
    n148i_encode_options_t options;
    n148i_encode_options_init(&options);
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    n148i_image_t decoded = {0};
    if (n148i_encode_memory(&source, &options, &encoded, &encoded_size) !=
            N148I_OK || encoded_size == 0 ||
        n148i_decode_memory(encoded, encoded_size, &decoded) != N148I_OK ||
        decoded.width != WIDTH || decoded.height != HEIGHT) {
        n148i_free_image(&decoded);
        n148i_free_buffer(encoded);
        return EXIT_FAILURE;
    }
    printf("%s format=%u bytes=%zu\n", n148i_library_version(),
           n148i_format_version(), encoded_size);
    n148i_free_image(&decoded);
    n148i_free_buffer(encoded);
    return EXIT_SUCCESS;
}
'''


FEATURE_VALUE_ASSERTIONS = r'''#include <stdint.h>
#include <n148i.h>

_Static_assert(N148I_FEATURE_RANS == (UINT32_C(1) << 0), "RANS bit");
_Static_assert(N148I_FEATURE_CONTEXT == (UINT32_C(1) << 1), "context bit");
_Static_assert(N148I_FEATURE_INTRA == (UINT32_C(1) << 2), "intra bit");
_Static_assert(N148I_FEATURE_PERCEPTUAL_COLOR == (UINT32_C(1) << 3), "color bit");
_Static_assert(N148I_FEATURE_ADAPTIVE_QUANT == (UINT32_C(1) << 4), "quant bit");
_Static_assert(N148I_FEATURE_VARIABLE_TRANSFORM == (UINT32_C(1) << 5), "transform bit");
_Static_assert(N148I_FEATURE_RDO == (UINT32_C(1) << 6), "RDO bit");
_Static_assert(N148I_FEATURE_LOOP_FILTER == (UINT32_C(1) << 7), "filter bit");
_Static_assert(N148I_FEATURE_DIRECTIONAL_INTRA == (UINT32_C(1) << 8), "directional bit");
_Static_assert(N148I_FEATURE_INTRA_4X4 == (UINT32_C(1) << 9), "4x4 bit");
_Static_assert(N148I_FEATURE_CONTEXTUAL_RDO == (UINT32_C(1) << 10), "contextual RDO bit");
_Static_assert(N148I_FEATURE_PERCEPTUAL_TRELLIS == (UINT32_C(1) << 11), "trellis bit");
_Static_assert(N148I_FEATURE_RECONSTRUCTION_AWARE_CHROMA == (UINT32_C(1) << 12), "chroma reconstruction bit");
_Static_assert(N148I_FEATURE_JOINT_CHROMA_INTRA == (UINT32_C(1) << 13), "joint chroma bit");
_Static_assert(N148I_FEATURE_CALIBRATED_CHROMA_QUANT == (UINT32_C(1) << 14), "chroma quant bit");
_Static_assert(N148I_FEATURE_SEGMENTATION == (UINT32_C(1) << 15), "segmentation bit");
_Static_assert(N148I_FEATURE_ADAPTIVE_LOOP_FILTER == (UINT32_C(1) << 16), "adaptive filter bit");
_Static_assert(N148I_FEATURE_CALIBRATED_LUMA_QUANT == (UINT32_C(1) << 17), "luma quant bit");
_Static_assert(N148I_FEATURE_LUMA_TRANSFORM_16X16 == (UINT32_C(1) << 18), "16x16 bit");
_Static_assert(N148I_FEATURE_QUANT_ADAPTIVE_LUMA_FILTER == (UINT32_C(1) << 19), "luma filter bit");
_Static_assert(N148I_FEATURE_FILTERED_INTRA_REFERENCES == (UINT32_C(1) << 20), "reference filter bit");
_Static_assert(N148I_FEATURE_PLANAR_INTRA_PREDICTION == (UINT32_C(1) << 21), "planar bit");
_Static_assert(N148I_FEATURE_STRUCTURAL_RDO == (UINT32_C(1) << 22), "structural RDO bit");
_Static_assert(N148I_FEATURE_QUALITY_LAMBDA == (UINT32_C(1) << 23), "lambda bit");
_Static_assert(N148I_FEATURE_LUMA_TRANSFORM_32X32 == (UINT32_C(1) << 24), "32x32 bit");
_Static_assert(N148I_FEATURE_SECOND_ORDER_DC == (UINT32_C(1) << 25), "second-order DC bit");
_Static_assert(N148I_FEATURE_FINE_INTRA_DIRECTIONS == (UINT32_C(1) << 26), "fine direction bit");
_Static_assert(N148I_FEATURE_ADAPTIVE_ENTROPY == (UINT32_C(1) << 27), "adaptive entropy bit");
_Static_assert(N148I_FEATURE_RICH_COEFFICIENT_CONTEXTS == (UINT32_C(1) << 28), "rich context bit");
_Static_assert(N148I_FEATURE_MULTIPLE_INTRA_REFERENCES == (UINT32_C(1) << 29), "multiple reference bit");
_Static_assert(N148I_FEATURE_DIRECTIONAL_SCAN == (UINT32_C(1) << 30), "scan bit");
_Static_assert(N148I_FEATURE_DIRECTIONAL_TRANSFORM == (UINT32_C(1) << 31), "directional transform bit");
_Static_assert(N148I_FEATURE_ALL == UINT32_C(0x000000ff), "base feature mask");
_Static_assert(N148I_FORMAT_2_DEFAULT_FEATURES == UINT32_C(0x000000c7), "format 2 default");
_Static_assert(N148I_FORMAT_2_FEATURE_MASK == UINT32_C(0x000000ff), "format 2 mask");
_Static_assert(N148I_PROFILE_DIRECTIONAL_INTRA == UINT32_C(0x000003c7), "directional profile");
_Static_assert(N148I_PROFILE_CONTEXTUAL_RDO == UINT32_C(0x000007c7), "contextual profile");
_Static_assert(N148I_PROFILE_PERCEPTUAL_TRELLIS == UINT32_C(0x00000fc7), "trellis profile");
_Static_assert(N148I_FORMAT_3_DEFAULT_FEATURES == UINT32_C(0x000007c7), "format 3 default");
_Static_assert(N148I_FORMAT_3_FEATURE_MASK == UINT32_C(0x00000fc7), "format 3 mask");
_Static_assert(N148I_PROFILE_RECONSTRUCTION_AWARE_CHROMA == UINT32_C(0x000017c7), "chroma reconstruction profile");
_Static_assert(N148I_PROFILE_JOINT_CHROMA_INTRA == UINT32_C(0x000037c7), "joint chroma profile");
_Static_assert(N148I_PROFILE_CALIBRATED_CHROMA_QUANT == UINT32_C(0x000057c7), "chroma quant profile");
_Static_assert(N148I_PROFILE_CHROMA_SEGMENTATION == UINT32_C(0x0000d7c7), "segmentation profile");
_Static_assert(N148I_PROFILE_ADAPTIVE_CHROMA_FILTER == UINT32_C(0x0001d7c7), "adaptive chroma profile");
_Static_assert(N148I_FORMAT_4_DEFAULT_FEATURES == UINT32_C(0x000057c7), "format 4 default");
_Static_assert(N148I_FORMAT_4_FEATURE_MASK == UINT32_C(0x0001f7c7), "format 4 mask");
_Static_assert(N148I_PROFILE_CALIBRATED_LUMA_QUANT == UINT32_C(0x000257c7), "luma quant profile");
_Static_assert(N148I_PROFILE_VARIABLE_LUMA_TRANSFORM == UINT32_C(0x000657c7), "variable luma profile");
_Static_assert(N148I_PROFILE_PERCEPTUAL_LUMA_TRELLIS == UINT32_C(0x00065fc7), "luma trellis profile");
_Static_assert(N148I_PROFILE_QUANT_ADAPTIVE_LUMA_FILTER == UINT32_C(0x000e57c7), "luma filter profile");
_Static_assert(N148I_FORMAT_5_DEFAULT_FEATURES == UINT32_C(0x000657c7), "format 5 default");
_Static_assert(N148I_FORMAT_5_FEATURE_MASK == UINT32_C(0x000ff7c7), "format 5 mask");
_Static_assert(N148I_PROFILE_LUMA_TRANSFORM_32X32 == UINT32_C(0x010657c7), "32x32 profile");
_Static_assert(N148I_PROFILE_FILTERED_INTRA_REFERENCES == UINT32_C(0x001657c7), "filtered references profile");
_Static_assert(N148I_PROFILE_PLANAR_INTRA_PREDICTION == UINT32_C(0x002657c7), "planar profile");
_Static_assert(N148I_PROFILE_COMBINED_INTRA_PREDICTION == UINT32_C(0x003657c7), "combined prediction profile");
_Static_assert(N148I_PROFILE_STRUCTURAL_RDO == UINT32_C(0x006657c7), "structural profile");
_Static_assert(N148I_PROFILE_QUALITY_LAMBDA == UINT32_C(0x00a657c7), "lambda profile");
_Static_assert(N148I_FORMAT_6_DEFAULT_FEATURES == UINT32_C(0x002657c7), "format 6 default");
_Static_assert(N148I_FORMAT_6_FEATURE_MASK == UINT32_C(0x07fff7c7), "format 6 mask");
_Static_assert(N148I_PROFILE_ADAPTIVE_ENTROPY == UINT32_C(0x082657c7), "adaptive entropy profile");
_Static_assert(N148I_PROFILE_RICH_COEFFICIENT_CONTEXTS == UINT32_C(0x182657c7), "rich context profile");
_Static_assert(N148I_PROFILE_RATE_CALIBRATED_SEARCH == UINT32_C(0x18a657c7), "calibrated search profile");
_Static_assert(N148I_PROFILE_SPECTRAL_LUMA_QUANT == UINT32_C(0x18a657cf), "spectral luma profile");
_Static_assert(N148I_FORMAT_7_REFINED_SPECTRAL_LUMA_QUANT == UINT32_C(0x00000010), "refined spectral bit");
_Static_assert(N148I_PROFILE_REFINED_SPECTRAL_LUMA_QUANT == UINT32_C(0x18a657df), "refined spectral profile");
_Static_assert(N148I_FORMAT_7_STRUCTURAL_LUMA_QUANT == UINT32_C(0x00000020), "structural luma bit");
_Static_assert(N148I_PROFILE_STRUCTURAL_LUMA_QUANT == UINT32_C(0x18a6d7ff), "structural luma profile");
_Static_assert(N148I_FORMAT_7_BALANCED_RECONSTRUCTION == UINT32_C(0x80000000), "balanced reconstruction bit");
_Static_assert(N148I_PROFILE_BALANCED_RECONSTRUCTION == UINT32_C(0x98a6d7ff), "balanced reconstruction profile");
_Static_assert(N148I_FORMAT_7_DETAIL_RECONSTRUCTION == UINT32_C(0x40000000), "detail reconstruction bit");
_Static_assert(N148I_PROFILE_DETAIL_RECONSTRUCTION == UINT32_C(0xd8a6d7ff), "detail reconstruction profile");
_Static_assert(N148I_PROFILE_MULTIPLE_INTRA_REFERENCES == UINT32_C(0x382657c7), "multiple reference profile");
_Static_assert(N148I_PROFILE_DIRECTIONAL_SCAN == UINT32_C(0x782657c7), "scan profile");
_Static_assert(N148I_PROFILE_DIRECTIONAL_TRANSFORM == UINT32_C(0xf82657c7), "directional transform profile");
_Static_assert(N148I_FORMAT_7_DEFAULT_FEATURES == UINT32_C(0x18a657c7), "format 7 default");
_Static_assert((N148I_FORMAT_7_DEFAULT_FEATURES & N148I_FEATURE_PERCEPTUAL_TRELLIS) == 0, "format 7 default excludes trellis");
_Static_assert(N148I_FORMAT_7_FEATURE_MASK == UINT32_C(0xffffffff), "format 7 mask");
_Static_assert(N148I_FORMAT_7_FULL_TRELLIS_FEATURES == UINT32_C(0x18a65fc7), "format 7 full trellis");

int main(void) { return 0; }
'''


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=root)
    parser.add_argument(
        "--output-dir", type=Path, default=root / "benchmarks/release",
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    header = (root / "include/n148i.h").read_text(encoding="utf-8")
    version_match = re.search(
        r'^#define N148I_VERSION_STRING "([^"]+)"$', header, re.MULTILINE)
    if not version_match:
        raise SystemExit("N148I_VERSION_STRING is missing from the public header")
    library_version = ".".join(re.search(r"^#define N148I_VERSION_" + part + r" (\d+)$", header, re.MULTILINE).group(1) for part in ("MAJOR", "MINOR", "PATCH"))
    output_dir = args.output_dir.resolve()
    work = root / ".build" / "release-validation"
    alias_root = Path(tempfile.mkdtemp(prefix="n148-release-prefix-"))
    expected_parent = (root / ".build").resolve()
    if work.resolve().parent != expected_parent:
        raise SystemExit(f"unsafe work directory: {work}")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    transcript: list[str] = []
    results: dict[str, dict[str, Any]] = {}

    def run(
        name: str, command: list[str], *, cwd: Path = root,
        environment: dict[str, str] | None = None, timeout: int = 1800,
    ) -> subprocess.CompletedProcess[str]:
        env = {**os.environ, "LC_ALL": "C"}
        if environment:
            env.update(environment)
        transcript.append(f"$ {' '.join(shlex.quote(part) for part in command)}\n")
        completed = subprocess.run(
            command, cwd=cwd, env=env, check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", timeout=timeout,
        )
        transcript.extend((completed.stdout, completed.stderr, "\n"))
        results[name] = {
            "argv": command,
            "cwd": str(cwd),
            "exit_code": completed.returncode,
            "passed": completed.returncode == 0,
        }
        if completed.returncode != 0:
            raise RuntimeError(f"{name} failed with exit code {completed.returncode}")
        return completed

    failure: str | None = None
    try:
        jobs = str(args.jobs)
        run("naming_audit", ["python3", "tools/audit_naming.py"])
        run("make_normal", ["make", f"-j{jobs}", "compare", "validate", "libraries"])
        sentinel = run(
            "format_1_sentinel",
            [str(root / "compare"), "images/example.ppm", "50", "20"],
        )
        match = re.search(r"N148i\s+enc.*?\s+(\d+) B\s+PSNR", sentinel.stdout)
        sentinel_bytes = int(match.group(1)) if match else None
        if sentinel_bytes != 2290:
            raise RuntimeError(f"format 1 sentinel is {sentinel_bytes}, expected 2290")
        results["format_1_sentinel"]["bytes"] = sentinel_bytes

        validation = run("validation_suite", [str(root / "validate")], timeout=1800)
        if (
            "validation PASS (0 failures)" not in validation.stderr or
            ",FAIL" in validation.stdout
        ):
            raise RuntimeError("validation suite did not report zero failures")

        run(
            "make_without_threads",
            ["make", f"-j{jobs}", "NOTHREADS=1", "-B", "validate"],
        )
        no_threads = run(
            "validation_without_threads", [str(root / "validate")], timeout=1800,
        )
        if (
            "validation PASS (0 failures)" not in no_threads.stderr or
            ",FAIL" in no_threads.stdout
        ):
            raise RuntimeError("thread-free validation did not report zero failures")

        cmake_build = work / "cmake"
        run("cmake_configure", [
            "cmake", "-S", str(root), "-B", str(cmake_build),
            "-DCMAKE_BUILD_TYPE=Release",
        ])
        run("cmake_build", [
            "cmake", "--build", str(cmake_build), "--parallel", jobs,
        ])
        run("ctest", [
            "ctest", "--test-dir", str(cmake_build), "--output-on-failure",
        ], timeout=1800)

        sanitizer_build = work / "sanitizers"
        sanitizer_flags = "-fsanitize=address,undefined -fno-omit-frame-pointer"
        run("sanitizer_configure", [
            "cmake", "-S", str(root), "-B", str(sanitizer_build),
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            f"-DCMAKE_C_FLAGS={sanitizer_flags}",
            f"-DCMAKE_EXE_LINKER_FLAGS={sanitizer_flags}",
            f"-DCMAKE_SHARED_LINKER_FLAGS={sanitizer_flags}",
        ])
        run("sanitizer_build", [
            "cmake", "--build", str(sanitizer_build), "--parallel", jobs,
        ])
        sanitizer = run(
            "asan_ubsan_lsan", [str(sanitizer_build / "validate")],
            environment={
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:strict_string_checks=1",
                "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
            }, timeout=3600,
        )
        if (
            "validation PASS (0 failures)" not in sanitizer.stderr or
            ",FAIL" in sanitizer.stdout
        ):
            raise RuntimeError("sanitizer validation did not report zero failures")

        # Restore the normal root artifacts after the thread-free Make build.
        run("make_normal_restore", [
            "make", f"-j{jobs}", "-B", "compare", "validate", "libraries",
        ])

        shared = root / f"libn148i.so.{library_version}"
        nm = run("dynamic_symbols", ["nm", "-D", "--defined-only", str(shared)])
        defined = {
            line.split()[-1].split("@", 1)[0]
            for line in nm.stdout.splitlines() if line.split()
        }
        exported = {name for name in defined if name.startswith("n148i_")}
        unexpected = defined - PUBLIC_SYMBOLS - {"N148I_2.0"}
        if exported != PUBLIC_SYMBOLS or unexpected:
            raise RuntimeError(
                f"exported symbols differ: missing={sorted(PUBLIC_SYMBOLS - exported)}, "
                f"extra={sorted((exported - PUBLIC_SYMBOLS) | unexpected)}"
            )
        results["dynamic_symbols"]["exported_symbols"] = sorted(exported)
        results["dynamic_symbols"]["exported_symbol_count"] = len(exported)

        dynamic = run("soname", ["readelf", "-d", str(shared)])
        expected_soname = f"libn148i.so.{library_version.split('.')[0]}"
        if f"Library soname: [{expected_soname}]" not in dynamic.stdout:
            raise RuntimeError(f"shared library SONAME differs from {expected_soname}")
        version_info = run("symbol_version_node", [
            "readelf", "--version-info", str(shared),
        ])
        if "N148I_2.0" not in version_info.stdout:
            raise RuntimeError("N148I_2.0 symbol version node is absent")

        install = work / "install"
        run("cmake_install", [
            "cmake", "--install", str(cmake_build), "--prefix", str(install),
        ])
        # pkgconf escapes each byte of a non-ASCII prefix independently under
        # this workspace path, yielding output that is neither valid UTF-8 nor
        # usable compiler flags. Exercise the installed tree through a stable
        # ASCII-only alias; the files and metadata remain the actual install.
        install_alias = alias_root / "install"
        install_alias.symlink_to(install, target_is_directory=True)
        pkg_env = {
            "PKG_CONFIG_PATH": str(install_alias / "lib/pkgconfig"),
            "LD_LIBRARY_PATH": str(install_alias / "lib"),
        }
        pkg = run("pkg_config", ["pkg-config", "--cflags", "--libs", "n148i"],
                  environment=pkg_env)

        consumer_dir = work / "consumers"
        consumer_dir.mkdir()
        consumer_source = consumer_dir / "consumer.c"
        consumer_source.write_text(CONSUMER_SOURCE, encoding="utf-8")
        pkg_binary = consumer_dir / "consumer-pkg-config"
        run("pkg_config_consumer_build", [
            "cc", str(consumer_source), *shlex.split(pkg.stdout), "-o", str(pkg_binary),
        ], environment=pkg_env)
        run("pkg_config_consumer_run", [str(pkg_binary)], environment=pkg_env)

        cmake_consumer = consumer_dir / "cmake"
        cmake_consumer.mkdir()
        shutil.copyfile(consumer_source, cmake_consumer / "consumer.c")
        (cmake_consumer / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(n148i_external_consumer LANGUAGES C)\n"
            "find_package(n148i 2 CONFIG REQUIRED)\n"
            "add_executable(consumer consumer.c)\n"
            "target_link_libraries(consumer PRIVATE n148i::n148i)\n",
            encoding="utf-8",
        )
        cmake_consumer_build = consumer_dir / "cmake-build"
        run("cmake_consumer_configure", [
            "cmake", "-S", str(cmake_consumer), "-B", str(cmake_consumer_build),
            f"-DCMAKE_PREFIX_PATH={install_alias}",
        ])
        run("cmake_consumer_build", [
            "cmake", "--build", str(cmake_consumer_build), "--parallel", jobs,
        ])
        run(
            "cmake_consumer_run", [str(cmake_consumer_build / "consumer")],
            environment=pkg_env,
        )

        static_binary = consumer_dir / "consumer-static"
        run("static_consumer_build", [
            "cc", "-DN148I_STATIC_DEFINE", f"-I{install_alias / 'include'}",
            str(consumer_source), str(install_alias / "lib/static/libn148i.a"),
            "-pthread", "-lm", "-o", str(static_binary),
        ])
        run("static_consumer_run", [str(static_binary)])

        feature_source = consumer_dir / "feature-values.c"
        feature_source.write_text(FEATURE_VALUE_ASSERTIONS, encoding="utf-8")
        run("feature_mask_values", [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{install_alias / 'include'}", str(feature_source),
            "-o", str(consumer_dir / "feature-values"),
        ])

        cpp_source = consumer_dir / "header.cpp"
        cpp_source.write_text(
            "#include <n148i.h>\n"
            "static_assert(N148I_FORMAT_VERSION == 7, \"format version\");\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        run("cpp_header", [
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            f"-I{install_alias / 'include'}", "-c", str(cpp_source),
            "-o", str(consumer_dir / "header.o"),
        ])

        readme_binary = consumer_dir / "readme-example"
        run("readme_example_build", [
            "cc", str(root / "examples/memory_roundtrip.c"),
            *shlex.split(pkg.stdout), "-o", str(readme_binary),
        ], environment=pkg_env)
        run("readme_example_run", [str(readme_binary)], environment=pkg_env)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        failure = str(error)
        transcript.append(f"VALIDATION FAILURE: {failure}\n")

    shutil.rmtree(alias_root, ignore_errors=True)

    summary = {
        "status": "pass" if failure is None else "fail",
        "failure": failure,
        "public_symbols_expected": sorted(PUBLIC_SYMBOLS),
        "gates": results,
    }
    rendered_transcript = "".join(transcript)
    normalized_transcript = "\n".join(
        line.rstrip() for line in rendered_transcript.splitlines()
    ) + "\n"
    (output_dir / "validation.log").write_text(
        normalized_transcript, encoding="utf-8",
    )
    (output_dir / "validation-summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if failure is None else 1


if __name__ == "__main__":
    raise SystemExit(main())

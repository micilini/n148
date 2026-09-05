#!/usr/bin/env python3
"""Benchmark N.148i against libjpeg-turbo on a streamed image corpus.

The script deliberately keeps only one downloaded source and one normalized
PPM at a time.  Results and the accepted-image manifest are replaced
atomically after every completed image, so an interrupted run can be resumed.
A prior manifest can be replayed with per-PPM hash validation for exact A/B
comparisons across codec revisions.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
import platform
import random
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


API_URL = "https://commons.wikimedia.org/w/api.php"
USER_AGENT = "N148-codec-benchmark/1.0 (local non-commercial research)"
API_RANDOM_DOC = "https://www.mediawiki.org/wiki/API:Random"
API_IMAGEINFO_DOC = "https://www.mediawiki.org/wiki/API:Imageinfo/en"
PICSUM_DOC = "https://picsum.photos/"
RESULT_FIELDS = [
    "image_id",
    "pageid",
    "width",
    "height",
    "pixels",
    "quality",
    "chroma",
    "threads",
    "reps",
    "n148_enc_ms",
    "n148_dec_ms",
    "n148_planes_ms",
    "n148_merge_ms",
    "n148_other_ms",
    "n148_bytes",
    "n148_psnr_db",
    "jpeg_enc_ms",
    "jpeg_dec_ms",
    "jpeg_bytes",
    "jpeg_psnr_db",
    "enc_ratio",
    "dec_ratio",
    "size_ratio",
]


def now_iso() -> str:
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


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
    path.chmod(0o664)


def atomic_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="", dir=path.parent, prefix=".atomic-", delete=False
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=RESULT_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
        handle.flush()
        os.fsync(handle.fileno())
        temporary = Path(handle.name)
    os.replace(temporary, path)
    path.chmod(0o664)


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
    path.chmod(0o664)


def load_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def load_results(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_line(command: list[str]) -> str:
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "indisponível"
    return result.stdout.strip().splitlines()[0] if result.stdout.strip() else "indisponível"


def environment_metadata(compare: Path, cpus: list[int]) -> dict[str, Any]:
    cpu_model = "desconhecido"
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass

    primary_cpu = cpus[0]
    governor_path = Path(
        f"/sys/devices/system/cpu/cpu{primary_cpu}/cpufreq/scaling_governor"
    )
    try:
        governor = governor_path.read_text(encoding="ascii").strip()
    except OSError:
        governor = "indisponível"

    return {
        "cpu": cpu_model,
        "logical_cpu_used": primary_cpu,
        "logical_cpus_used": cpus,
        "cpu_governor": governor,
        "kernel": platform.platform(),
        "python": platform.python_version(),
        "compiler": command_line(["gcc", "--version"]),
        "converter": command_line(["convert", "-version"]),
        "libjpeg_turbo_package": command_line(
            ["dpkg-query", "-W", "-f=${Version}", "libjpeg-turbo8"]
        ),
        "compare_binary": str(compare),
        "compare_sha256": sha256_file(compare),
    }


def api_batch(max_dimension: int) -> list[dict[str, Any]]:
    parameters = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "generator": "random",
        "grnnamespace": "6",
        "grnlimit": "50",
        "prop": "imageinfo",
        "iiprop": "url|size|mime|mediatype|sha1",
        "iiurlwidth": str(max_dimension),
    }
    request = urllib.request.Request(
        f"{API_URL}?{urllib.parse.urlencode(parameters)}",
        headers={"User-Agent": USER_AGENT},
    )
    with urllib.request.urlopen(request, timeout=45) as response:
        payload = json.load(response)
    return payload.get("query", {}).get("pages", [])


def collect_candidates(
    candidates: list[dict[str, Any]], target: int, min_dimension: int, max_dimension: int
) -> list[dict[str, Any]]:
    known = {int(candidate["pageid"]) for candidate in candidates}
    attempts = 0
    while len(candidates) < target:
        attempts += 1
        if attempts > 30:
            raise RuntimeError("a API não forneceu candidatos distintos suficientes")
        for page in api_batch(max_dimension):
            pageid = page.get("pageid")
            imageinfo = page.get("imageinfo") or []
            if pageid is None or int(pageid) in known or not imageinfo:
                continue
            info = imageinfo[0]
            if info.get("mediatype") != "BITMAP":
                continue
            width = int(info.get("width") or 0)
            height = int(info.get("height") or 0)
            if width < min_dimension or height < min_dimension:
                continue
            source_url = info.get("thumburl") or info.get("url")
            if not source_url or not str(source_url).startswith("https://"):
                continue
            candidate = {
                "provider": "commons",
                "pageid": int(pageid),
                "title": page.get("title", f"File:{pageid}"),
                "source_mime": info.get("mime", "desconhecido"),
                "original_width": width,
                "original_height": height,
                "original_bytes": int(info.get("size") or 0),
                "commons_sha1": info.get("sha1"),
                "download_url": source_url,
                "original_url": info.get("url"),
                "description_url": info.get("descriptionurl"),
                "api_thumb_width": info.get("thumbwidth"),
                "api_thumb_height": info.get("thumbheight"),
            }
            candidates.append(candidate)
            known.add(int(pageid))
            if len(candidates) >= target:
                break
    return candidates


def picsum_page(page: int, limit: int = 100) -> list[dict[str, Any]]:
    url = f"https://picsum.photos/v2/list?{urllib.parse.urlencode({'page': page, 'limit': limit})}"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=45) as response:
        payload = json.load(response)
    if not isinstance(payload, list):
        raise RuntimeError("resposta inesperada da API do Lorem Picsum")
    return payload


def collect_picsum_candidates(
    candidates: list[dict[str, Any]], target: int, max_dimension: int
) -> list[dict[str, Any]]:
    provider_count = sum(candidate.get("provider") == "picsum" for candidate in candidates)
    known_ids = {
        str(candidate.get("picsum_id"))
        for candidate in candidates
        if candidate.get("provider") == "picsum"
    }
    pool: list[dict[str, Any]] = []
    page = 1
    desired_pool = max(100, (target - provider_count) * 3)
    while len(pool) < desired_pool and page <= 20:
        batch = picsum_page(page)
        if not batch:
            break
        pool.extend(batch)
        page += 1
    random.SystemRandom().shuffle(pool)

    for item in pool:
        picsum_id = str(item.get("id", ""))
        if not picsum_id or picsum_id in known_ids:
            continue
        width = int(item.get("width") or 0)
        height = int(item.get("height") or 0)
        if width <= 0 or height <= 0:
            continue
        scale = min(1.0, max_dimension / max(width, height))
        target_width = max(1, round(width * scale))
        target_height = max(1, round(height * scale))
        try:
            numeric_id = int(picsum_id)
            pageid = -(numeric_id + 1)
        except ValueError:
            pageid = -int(hashlib.sha256(picsum_id.encode()).hexdigest()[:15], 16)
        author = str(item.get("author") or "autor desconhecido")
        candidate = {
            "provider": "picsum",
            "pageid": pageid,
            "picsum_id": picsum_id,
            "title": f"Picsum #{picsum_id} — {author}",
            "author": author,
            "source_mime": "image/jpeg",
            "original_width": width,
            "original_height": height,
            "original_bytes": 0,
            "commons_sha1": None,
            "download_url": (
                f"https://picsum.photos/id/{urllib.parse.quote(picsum_id)}/"
                f"{target_width}/{target_height}.jpg"
            ),
            "original_url": item.get("download_url"),
            "description_url": f"https://picsum.photos/id/{urllib.parse.quote(picsum_id)}/info",
            "attribution_url": item.get("url"),
            "api_thumb_width": target_width,
            "api_thumb_height": target_height,
        }
        candidates.append(candidate)
        known_ids.add(picsum_id)
        provider_count += 1
        if provider_count >= target:
            break
    if provider_count < target:
        raise RuntimeError("o catálogo do Picsum não forneceu candidatos distintos suficientes")
    return candidates


def download(
    url: str,
    destination: Path,
    maximum_bytes: int,
    attempts: int,
    retry_base_seconds: float,
) -> int:
    last_error: Exception | None = None
    for attempt in range(attempts):
        destination.unlink(missing_ok=True)
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        total = 0
        try:
            with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as out:
                declared = response.headers.get("Content-Length")
                if declared and int(declared) > maximum_bytes:
                    raise RuntimeError(f"download declarado com {int(declared)} bytes")
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > maximum_bytes:
                        raise RuntimeError(
                            f"download excedeu o limite de {maximum_bytes} bytes"
                        )
                    out.write(chunk)
            if total == 0:
                raise RuntimeError("download vazio")
            return total
        except urllib.error.HTTPError as error:
            last_error = error
            if error.code != 429 and not 500 <= error.code < 600:
                raise
            retry_after = error.headers.get("Retry-After")
            try:
                header_delay = float(retry_after) if retry_after else 0.0
            except ValueError:
                header_delay = 0.0
        except (urllib.error.URLError, TimeoutError) as error:
            last_error = error
            header_delay = 0.0

        destination.unlink(missing_ok=True)
        if attempt + 1 >= attempts:
            assert last_error is not None
            raise last_error
        delay = min(60.0, max(header_delay, retry_base_seconds * (2**attempt)))
        print(
            f"  download temporariamente bloqueado; nova tentativa em {delay:.0f}s "
            f"({attempt + 2}/{attempts})",
            flush=True,
        )
        time.sleep(delay)
    raise RuntimeError("download falhou sem erro registrado")


def ppm_dimensions(path: Path) -> tuple[int, int]:
    def token(handle: Any) -> bytes:
        while True:
            byte = handle.read(1)
            if not byte:
                raise RuntimeError("cabeçalho PPM truncado")
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
        data_offset = handle.tell()
    if magic != b"P6" or maximum != 255 or width <= 0 or height <= 0:
        raise RuntimeError("PPM não é P6 RGB de 8 bits")
    expected = data_offset + width * height * 3
    if path.stat().st_size < expected:
        raise RuntimeError("dados PPM truncados")
    return width, height


def convert_to_ppm(source: Path, ppm: Path, max_dimension: int) -> tuple[int, int]:
    command = [
        "convert",
        f"{source}[0]",
        "-auto-orient",
        "-colorspace",
        "sRGB",
        "-resize",
        f"{max_dimension}x{max_dimension}>",
        "-background",
        "white",
        "-flatten",
        "-depth",
        "8",
        str(ppm),
    ]
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=180,
    )
    if result.returncode != 0:
        message = result.stderr.strip().replace("\n", " ")
        raise RuntimeError(f"conversão falhou: {message[:400]}")
    width, height = ppm_dimensions(ppm)
    if max(width, height) > max_dimension:
        raise RuntimeError(f"redimensionamento excedeu {max_dimension}px: {width}x{height}")
    return width, height


CODEC_LINE = re.compile(
    r"^(N148i|JPEG)\s+enc\s+([0-9.]+)\s+ms\s+dec\s+([0-9.]+)\s+ms\s+"
    r"([0-9]+)\s+B\s+PSNR\s+([+\-0-9.inf]+)\s+dB$"
)
STAGE_LINE = re.compile(
    r"^N148i decode stages: planes\s+([0-9.]+)\s+ms\s+"
    r"merge\s+([0-9.]+)\s+ms\s+other\s+([+\-0-9.]+)\s+ms$"
)


def benchmark_point(
    compare: Path,
    ppm: Path,
    quality: int,
    reps: int,
    chroma: int,
    threads: int,
    cpu_set: str,
    jpeg_cpu: int,
) -> dict[str, Any]:
    command = [
        "taskset",
        "-c",
        cpu_set,
        str(compare),
        str(ppm),
        str(quality),
        str(reps),
        str(chroma),
        str(threads),
        str(jpeg_cpu),
    ]
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=600,
        env=environment,
    )
    if result.returncode != 0:
        message = (result.stderr or result.stdout).strip().replace("\n", " ")
        raise RuntimeError(f"compare retornou {result.returncode}: {message[:500]}")

    parsed: dict[str, dict[str, float | int]] = {}
    stages: tuple[float, float, float] | None = None
    for line in result.stdout.splitlines():
        match = CODEC_LINE.match(line.strip())
        if match:
            codec, enc, dec, size, psnr = match.groups()
            parsed[codec] = {
                "enc": float(enc),
                "dec": float(dec),
                "bytes": int(size),
                "psnr": float(psnr),
            }
            continue
        stage_match = STAGE_LINE.match(line.strip())
        if stage_match:
            stages = tuple(float(value) for value in stage_match.groups())
    if set(parsed) != {"N148i", "JPEG"}:
        raise RuntimeError(f"saída do comparador não reconhecida: {result.stdout[:500]!r}")
    if stages is None:
        raise RuntimeError(f"estágios do decoder não reconhecidos: {result.stdout[:500]!r}")

    n148 = parsed["N148i"]
    jpeg = parsed["JPEG"]
    if not all(float(n148[key]) > 0 and float(jpeg[key]) > 0 for key in ("enc", "dec", "bytes")):
        raise RuntimeError("comparador produziu tempo ou tamanho não positivo")
    return {
        "n148_enc_ms": n148["enc"],
        "n148_dec_ms": n148["dec"],
        "n148_planes_ms": stages[0],
        "n148_merge_ms": stages[1],
        "n148_other_ms": stages[2],
        "n148_bytes": n148["bytes"],
        "n148_psnr_db": n148["psnr"],
        "jpeg_enc_ms": jpeg["enc"],
        "jpeg_dec_ms": jpeg["dec"],
        "jpeg_bytes": jpeg["bytes"],
        "jpeg_psnr_db": jpeg["psnr"],
        "enc_ratio": float(n148["enc"]) / float(jpeg["enc"]),
        "dec_ratio": float(n148["dec"]) / float(jpeg["dec"]),
        "size_ratio": int(n148["bytes"]) / int(jpeg["bytes"]),
    }


def solve_linear(matrix: list[list[float]], vector: list[float]) -> list[float]:
    augmented = [row[:] + [value] for row, value in zip(matrix, vector)]
    size = len(vector)
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1e-14:
            raise ValueError("curva singular")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        divisor = augmented[column][column]
        augmented[column] = [value / divisor for value in augmented[column]]
        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [
                left - factor * right
                for left, right in zip(augmented[row], augmented[column])
            ]
    return [augmented[row][-1] for row in range(size)]


def bd_rate(
    candidate: list[tuple[float, float]], anchor: list[tuple[float, float]]
) -> float | None:
    """Classic cubic Bjøntegaard delta-rate; negative favours candidate."""
    candidate = sorted(candidate)
    anchor = sorted(anchor)
    if len(candidate) != 4 or len(anchor) != 4:
        return None
    low = max(candidate[0][0], anchor[0][0])
    high = min(candidate[-1][0], anchor[-1][0])
    if not math.isfinite(low + high) or high - low < 1e-6:
        return None
    center = (low + high) / 2.0
    scale = (high - low) / 2.0

    def integral(points: list[tuple[float, float]]) -> float:
        matrix: list[list[float]] = []
        values: list[float] = []
        for psnr, rate in points:
            if rate <= 0 or not math.isfinite(psnr + rate):
                raise ValueError("ponto inválido")
            x = (psnr - center) / scale
            matrix.append([1.0, x, x * x, x * x * x])
            values.append(math.log(rate))
        coefficients = solve_linear(matrix, values)
        lower, upper = -1.0, 1.0
        value = 0.0
        for power, coefficient in enumerate(coefficients):
            value += coefficient * (
                upper ** (power + 1) - lower ** (power + 1)
            ) / (power + 1)
        return value * scale

    try:
        average_difference = (integral(candidate) - integral(anchor)) / (high - low)
        return (math.exp(average_difference) - 1.0) * 100.0
    except (OverflowError, ValueError, ZeroDivisionError):
        return None


def pooled_psnr(rows: Iterable[dict[str, str]], prefix: str) -> float:
    total_pixels = 0.0
    squared_error = 0.0
    for row in rows:
        pixels = float(row["pixels"])
        psnr = float(row[f"{prefix}_psnr_db"])
        total_pixels += pixels
        squared_error += pixels * 65025.0 / (10.0 ** (psnr / 10.0))
    mse = squared_error / total_pixels
    return 10.0 * math.log10(65025.0 / mse)


def signed_percent(ratio: float) -> str:
    value = (ratio - 1.0) * 100.0
    sign = "+" if value > 0 else "−" if value < 0 else ""
    return f"{sign}{abs(value):.2f}%".replace(".", ",")


def decimal(value: float, places: int = 3) -> str:
    return f"{value:.{places}f}".replace(".", ",")


def integer(value: float | int) -> str:
    return f"{int(round(value)):,}".replace(",", ".")


def escape_table(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ")


def short_title(title: str, limit: int = 48) -> str:
    if title.startswith("File:"):
        title = title[5:]
    return title if len(title) <= limit else title[: limit - 1] + "…"


def metric_outcome(n148: float, jpeg: float, kind: str) -> str:
    if n148 == jpeg:
        return "empate"
    if n148 < jpeg:
        adjective = "menor" if kind == "size" else "mais rápido"
        amount = decimal((1.0 - n148 / jpeg) * 100.0, 2)
        return f"N.148i {amount}% {adjective}"
    adjective = "menor" if kind == "size" else "mais rápido"
    amount = decimal((1.0 - jpeg / n148) * 100.0, 2)
    return f"JPEG {amount}% {adjective}"


def render_report(
    output_dir: Path,
    manifest: list[dict[str, Any]],
    rows: list[dict[str, str]],
    metadata: dict[str, Any],
    failures: list[dict[str, Any]],
) -> str:
    qualities = [int(value) for value in metadata["config"]["qualities"]]
    rows_by_quality: dict[int, list[dict[str, str]]] = defaultdict(list)
    rows_by_image: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        rows_by_quality[int(row["quality"])].append(row)
        rows_by_image[int(row["image_id"])].append(row)

    def total(field: str, subset: Iterable[dict[str, str]] = rows) -> float:
        return sum(float(row[field]) for row in subset)

    total_n_size = total("n148_bytes")
    total_j_size = total("jpeg_bytes")
    total_n_enc = total("n148_enc_ms")
    total_j_enc = total("jpeg_enc_ms")
    total_n_dec = total("n148_dec_ms")
    total_j_dec = total("jpeg_dec_ms")
    point_count = len(rows)

    def record(field_n: str, field_j: str, subset: Iterable[dict[str, str]] = rows) -> tuple[int, int, int]:
        wins = ties = losses = 0
        for row in subset:
            left, right = float(row[field_n]), float(row[field_j])
            if left < right:
                wins += 1
            elif left > right:
                losses += 1
            else:
                ties += 1
        return wins, ties, losses

    size_record = record("n148_bytes", "jpeg_bytes")
    enc_record = record("n148_enc_ms", "jpeg_enc_ms")
    dec_record = record("n148_dec_ms", "jpeg_dec_ms")
    psnr_wins = sum(float(row["n148_psnr_db"]) > float(row["jpeg_psnr_db"]) for row in rows)
    psnr_ties = sum(float(row["n148_psnr_db"]) == float(row["jpeg_psnr_db"]) for row in rows)
    triple_wins = sum(
        float(row["n148_bytes"]) < float(row["jpeg_bytes"])
        and float(row["n148_enc_ms"]) < float(row["jpeg_enc_ms"])
        and float(row["n148_dec_ms"]) < float(row["jpeg_dec_ms"])
        for row in rows
    )

    corpus_n_curve: list[tuple[float, float]] = []
    corpus_j_curve: list[tuple[float, float]] = []
    quality_summaries: list[dict[str, Any]] = []
    for quality in qualities:
        subset = rows_by_quality[quality]
        pixels = sum(float(row["pixels"]) for row in subset)
        n_size, j_size = total("n148_bytes", subset), total("jpeg_bytes", subset)
        n_psnr, j_psnr = pooled_psnr(subset, "n148"), pooled_psnr(subset, "jpeg")
        corpus_n_curve.append((n_psnr, n_size * 8.0 / pixels))
        corpus_j_curve.append((j_psnr, j_size * 8.0 / pixels))
        quality_summaries.append(
            {
                "quality": quality,
                "n_size": n_size,
                "j_size": j_size,
                "n_enc": total("n148_enc_ms", subset),
                "j_enc": total("jpeg_enc_ms", subset),
                "n_dec": total("n148_dec_ms", subset),
                "j_dec": total("jpeg_dec_ms", subset),
                "n_psnr": n_psnr,
                "j_psnr": j_psnr,
                "size_record": record("n148_bytes", "jpeg_bytes", subset),
                "enc_record": record("n148_enc_ms", "jpeg_enc_ms", subset),
                "dec_record": record("n148_dec_ms", "jpeg_dec_ms", subset),
            }
        )
    corpus_bd = bd_rate(corpus_n_curve, corpus_j_curve)

    image_summaries: list[dict[str, Any]] = []
    image_bd_values: list[float] = []
    manifest_by_id = {int(item["image_id"]): item for item in manifest}
    provider_counts: dict[str, int] = defaultdict(int)
    for item in manifest:
        provider_counts[str(item.get("provider", "commons"))] += 1
    for image_id in sorted(rows_by_image):
        subset = sorted(rows_by_image[image_id], key=lambda row: int(row["quality"]))
        n_curve = [
            (float(row["n148_psnr_db"]), float(row["n148_bytes"]) * 8.0 / float(row["pixels"]))
            for row in subset
        ]
        j_curve = [
            (float(row["jpeg_psnr_db"]), float(row["jpeg_bytes"]) * 8.0 / float(row["pixels"]))
            for row in subset
        ]
        image_bd = bd_rate(n_curve, j_curve)
        if image_bd is not None and math.isfinite(image_bd):
            image_bd_values.append(image_bd)
        image_summaries.append(
            {
                "image_id": image_id,
                "manifest": manifest_by_id[image_id],
                "size_ratio": total("n148_bytes", subset) / total("jpeg_bytes", subset),
                "enc_ratio": total("n148_enc_ms", subset) / total("jpeg_enc_ms", subset),
                "dec_ratio": total("n148_dec_ms", subset) / total("jpeg_dec_ms", subset),
                "psnr_delta": statistics.mean(
                    float(row["n148_psnr_db"]) - float(row["jpeg_psnr_db"])
                    for row in subset
                ),
                "bd_rate": image_bd,
                "size_wins": record("n148_bytes", "jpeg_bytes", subset)[0],
                "enc_wins": record("n148_enc_ms", "jpeg_enc_ms", subset)[0],
                "dec_wins": record("n148_dec_ms", "jpeg_dec_ms", subset)[0],
            }
        )

    image_size_wins = sum(item["size_ratio"] < 1.0 for item in image_summaries)
    image_enc_wins = sum(item["enc_ratio"] < 1.0 for item in image_summaries)
    image_dec_wins = sum(item["dec_ratio"] < 1.0 for item in image_summaries)

    lines: list[str] = []
    lines.extend(
        [
            "# JPEG vs N.148i em 100 imagens aleatórias",
            "",
            f"**Execução concluída:** {metadata.get('finished_at', now_iso())}",
            "",
            "## Veredito",
            "",
            f"No agregado das {len(manifest)} imagens e {point_count} pontos de qualidade, "
            f"o tamanho ficou em **{metric_outcome(total_n_size, total_j_size, 'size')}**, "
            f"o encode em **{metric_outcome(total_n_enc, total_j_enc, 'time')}** e "
            f"o decode em **{metric_outcome(total_n_dec, total_j_dec, 'time')}**.",
            "",
        ]
    )
    if corpus_bd is not None:
        direction = "favorece o N.148i" if corpus_bd < 0 else "favorece o JPEG"
        lines.append(
            f"A comparação ajustada para a mesma qualidade deu **BD-rate de {decimal(corpus_bd, 2)}%** "
            f"para N.148i contra JPEG ({direction}; negativo favorece N.148i)."
        )
        lines.append("")

    lines.extend(
        [
            "## Placar agregado",
            "",
            "| Métrica | N.148i | JPEG | Δ N/J | V/E/D do N.148i |",
            "| --- | ---: | ---: | ---: | ---: |",
            f"| Tamanho acumulado | {integer(total_n_size)} B | {integer(total_j_size)} B | {signed_percent(total_n_size / total_j_size)} | {size_record[0]}/{size_record[1]}/{size_record[2]} |",
            f"| Soma das medianas de encode | {decimal(total_n_enc)} ms | {decimal(total_j_enc)} ms | {signed_percent(total_n_enc / total_j_enc)} | {enc_record[0]}/{enc_record[1]}/{enc_record[2]} |",
            f"| Soma das medianas de decode | {decimal(total_n_dec)} ms | {decimal(total_j_dec)} ms | {signed_percent(total_n_dec / total_j_dec)} | {dec_record[0]}/{dec_record[1]}/{dec_record[2]} |",
            "",
            f"O N.148i levou tamanho/encode/decode ao mesmo tempo em **{triple_wins}/{point_count} pontos**. "
            f"Agregando as quatro qualidades por imagem, venceu em tamanho em **{image_size_wins}/{len(manifest)}**, "
            f"encode em **{image_enc_wins}/{len(manifest)}** e decode em **{image_dec_wins}/{len(manifest)}** imagens.",
            "",
            f"No mesmo número de qualidade, o PSNR do N.148i foi maior em {psnr_wins}/{point_count}, "
            f"igual em {psnr_ties}/{point_count} e menor em {point_count - psnr_wins - psnr_ties}/{point_count} pontos. "
            "O BD-rate abaixo é a medida mais justa de eficiência de compressão porque compensa essa diferença.",
            "",
            "## Resultado por qualidade",
            "",
            "| Q | Tamanho Δ | Encode Δ | Decode Δ | PSNR N/J | Vitórias tamanho | Vitórias encode | Vitórias decode |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for item in quality_summaries:
        lines.append(
            f"| {item['quality']} | {signed_percent(item['n_size'] / item['j_size'])} | "
            f"{signed_percent(item['n_enc'] / item['j_enc'])} | "
            f"{signed_percent(item['n_dec'] / item['j_dec'])} | "
            f"{decimal(item['n_psnr'])}/{decimal(item['j_psnr'])} dB | "
            f"{item['size_record'][0]}/{len(rows_by_quality[item['quality']])} | "
            f"{item['enc_record'][0]}/{len(rows_by_quality[item['quality']])} | "
            f"{item['dec_record'][0]}/{len(rows_by_quality[item['quality']])} |"
        )

    if image_bd_values:
        lines.extend(
            [
                "",
                "## Eficiência na mesma qualidade visual",
                "",
                f"- BD-rate do corpus (curvas agregadas): **{decimal(corpus_bd or 0.0, 2)}%**.",
                f"- Mediana do BD-rate por imagem: **{decimal(statistics.median(image_bd_values), 2)}%**.",
            f"- Média do BD-rate por imagem: **{decimal(statistics.mean(image_bd_values), 2)}%** ({len(image_bd_values)}/{len(manifest)} curvas válidas).",
                "",
                f"BD-rate integra as curvas de {len(qualities)} pontos "
                f"(Q{'/'.join(map(str, qualities))}) no intervalo de PSNR comum. "
                "Valor negativo significa menos bits para o N.148i na mesma qualidade medida por PSNR.",
            ]
        )

    lines.extend(
        [
            "",
            "## Tabela das 100 imagens",
            "",
            "As três colunas Δ usam `(N.148i/JPEG − 1)`; portanto, **negativo favorece N.148i**. "
            "`V T/E/D` mostra em quantas das quatro qualidades ele venceu em tamanho, encode e decode.",
            "",
            "| ID / imagem | Dimensão | Δ tamanho | Δ encode | Δ decode | BD-rate | V T/E/D |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for item in image_summaries:
        source = item["manifest"]
        label = escape_table(short_title(str(source["title"])))
        url = str(source.get("description_url") or source.get("original_url") or "")
        linked = f"[{item['image_id']:03d} — {label}](<{url}>)" if url else f"{item['image_id']:03d} — {label}"
        bd_text = "n/d" if item["bd_rate"] is None else f"{decimal(item['bd_rate'], 2)}%"
        lines.append(
            f"| {linked} | {source['width']}×{source['height']} | "
            f"{signed_percent(item['size_ratio'])} | {signed_percent(item['enc_ratio'])} | "
            f"{signed_percent(item['dec_ratio'])} | {bd_text} | "
            f"{item['size_wins']}/{item['enc_wins']}/{item['dec_wins']} |"
        )

    config = metadata["config"]
    environment = metadata["environment"]
    if config.get("frozen_manifest_sha256"):
        selection_note = (
            "- Reexecução A/B: ordem e identidade copiadas de um manifesto anterior; "
            "cada PPM foi aceito somente após repetir dimensão e SHA-256. "
            f"SHA-256 do manifesto-fonte: `{config['frozen_manifest_sha256']}`."
        )
        candidate_description = (
            "- `candidates.json`: cópia ordenada do manifesto congelado usado na reexecução."
        )
    else:
        selection_note = (
            "- Seleção: candidatos aleatórios foram congelados em `candidates.json` "
            "à medida que a execução avançou."
        )
        candidate_description = (
            "- `candidates.json`: sequência de candidatos congelada antes/durante a execução."
        )
    baseline_description = (
        "- `baseline-results.csv`: os mesmos pontos medidos com o binário-base, "
        "alternando qual revisão rodou primeiro."
        if config.get("baseline_compare_sha256")
        else "- `baseline-results.csv`: não gerado nesta execução."
    )
    max_work = int(metadata.get("peak_temporary_bytes", 0))
    retained = sum(
        path.stat().st_size
        for path in output_dir.iterdir()
        if path.is_file() and path.name != "RELATORIO.md"
    )
    lines.extend(
        [
            "",
            "## Metodologia",
            "",
            f"- Corpus: {provider_counts.get('commons', 0)} páginas do namespace `File:` escolhidas pela [API Random]({API_RANDOM_DOC}) do Wikimedia Commons "
            f"(metadados/thumbnails pela [API Imageinfo]({API_IMAGEINFO_DOC})) e {provider_counts.get('picsum', 0)} fotos sorteadas do catálogo oficial do [Lorem Picsum]({PICSUM_DOC}). "
            f"No Commons foram aceitos bitmaps com ambos os lados originais ≥ {config['min_dimension']} px que puderam ser baixados e convertidos.",
            selection_note,
            f"- Normalização: primeiro frame, orientação EXIF aplicada, sRGB, transparência sobre branco, PPM RGB P6 de 8 bits, sem ampliar e limitado a {config['max_dimension']}×{config['max_dimension']} px.",
            f"- Matriz: {len(manifest)} imagens × qualidades {','.join(map(str, qualities))}; 4:2:0, Huffman otimizado, {config['reps']} repetições medidas por ponto mais aquecimento interno.",
            f"- Afinidade de CPU: `taskset` no conjunto lógico `{config.get('cpu_set', config['cpu'])}`; N.148i com {config['threads']} thread(s). O libjpeg-turbo permanece single-thread e sua thread é fixada no P-core lógico {config.get('jpeg_cpu', config['cpu'])}; as mudanças de afinidade ficam fora da janela cronometrada. O driver alterna qual codec roda primeiro.",
            "- Cronômetro: encode/decode em memória; download, conversão, I/O e cálculo do PSNR ficam fora do tempo. Cada número de tempo é a mediana.",
            "- Validação: cada ponto só é aceito se ambos codificarem, decodificarem, preservarem as dimensões e produzirem PSNR/tamanho válidos.",
            f"- Ambiente: {environment['cpu']}; governor `{environment['cpu_governor']}`; {environment['compiler']}; libjpeg-turbo {environment['libjpeg_turbo_package']}.",
            f"- Binário `compare`: SHA-256 `{environment['compare_sha256']}`.",
            "",
            "## Uso de disco e rastreabilidade",
            "",
            f"O maior par fonte+PPM observado ocupou cerca de **{decimal(max_work / 1048576.0, 1)} MiB**. "
            "Depois de concluir as quatro qualidades de cada imagem, tanto o download quanto o PPM foram apagados. "
            f"Os artefatos permanentes (sem este relatório) ocupam cerca de {decimal(retained / 1048576.0, 2)} MiB.",
            "",
            "Arquivos preservados:",
            "",
            "- `results.csv`: os 400 pontos brutos.",
            baseline_description,
            "- `manifest.json`: títulos, URLs, dimensões e hashes das 100 entradas aceitas.",
            candidate_description,
            "- `metadata.json`: configuração, ambiente e hash do binário.",
            "- `failures.json`: candidatos descartados e motivo, se houver.",
            "",
            "## Limitações",
            "",
            "- É uma amostra aleatória condicionada, não um corpus acadêmico balanceado; fotografias, scans, mapas e arte gráfica podem aparecer.",
            "- As entradas servidas pelo Commons/Picsum podem carregar artefatos de uma compressão anterior. Ambos os codecs, porém, recebem exatamente o mesmo PPM decodificado.",
            "- Mesmo `Q` não implica qualidade idêntica entre formatos; por isso o relatório separa o placar no mesmo Q do BD-rate na mesma qualidade medida.",
            "- Tempos valem para esta máquina, este binário e esta versão do libjpeg-turbo; uma única execução de desktop ainda sofre ruído de frequência, temperatura e tarefas do sistema.",
            "- PSNR não captura sozinho toda a qualidade perceptual, e o ensaio cobre apenas 4:2:0.",
            "",
            f"Candidatos rejeitados durante coleta/processamento: **{len(failures)}**.",
            "",
        ]
    )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare", type=Path, default=Path("./compare"))
    parser.add_argument(
        "--baseline-compare",
        type=Path,
        help="also benchmark this older compare binary on every frozen PPM",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--qualities", type=int, nargs="+", default=[25, 50, 75, 90])
    parser.add_argument("--reps", type=int, default=31)
    parser.add_argument("--chroma", type=int, default=2)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument(
        "--cpu",
        type=int,
        default=2,
        help="logical CPU used when --cpu-set is omitted (default: 2)",
    )
    parser.add_argument(
        "--cpu-set",
        help=(
            "taskset CPU list/ranges (for example 0,2,4-5); it must contain "
            "at least --threads logical CPUs"
        ),
    )
    parser.add_argument("--max-dimension", type=int, default=1600)
    parser.add_argument("--min-dimension", type=int, default=256)
    parser.add_argument("--max-download-mib", type=int, default=32)
    parser.add_argument("--min-free-gib", type=float, default=2.0)
    parser.add_argument("--download-attempts", type=int, default=6)
    parser.add_argument("--retry-base-seconds", type=float, default=5.0)
    parser.add_argument("--provider", choices=("commons", "picsum"), default="commons")
    parser.add_argument(
        "--frozen-manifest",
        type=Path,
        help="reuse this accepted manifest in order and reject any changed PPM",
    )
    return parser.parse_args()


def parse_cpu_set(value: str) -> list[int]:
    cpus: list[int] = []
    for item in value.split(","):
        item = item.strip()
        if re.fullmatch(r"[0-9]+", item):
            values = [int(item)]
        else:
            match = re.fullmatch(r"([0-9]+)-([0-9]+)", item)
            if match is None:
                raise ValueError(f"lista de CPUs inválida: {value!r}")
            first, last = (int(part) for part in match.groups())
            if last < first:
                raise ValueError(f"intervalo de CPUs invertido: {item!r}")
            values = list(range(first, last + 1))
        for cpu in values:
            if cpu in cpus:
                raise ValueError(f"CPU lógico repetido: {cpu}")
            cpus.append(cpu)
    if not cpus:
        raise ValueError("a lista de CPUs está vazia")
    return cpus


def main() -> int:
    args = parse_args()
    if args.count < 1 or args.reps < 1 or args.threads < 1 or not args.qualities:
        raise SystemExit("count, reps, threads e qualities devem ser positivos")
    if any(quality < 1 or quality > 100 for quality in args.qualities):
        raise SystemExit("qualidade fora do intervalo 1..100")
    try:
        cpu_ids = parse_cpu_set(args.cpu_set or str(args.cpu))
    except ValueError as error:
        raise SystemExit(str(error)) from error
    if len(cpu_ids) < args.threads:
        raise SystemExit(
            f"--cpu-set oferece {len(cpu_ids)} CPU(s), menos que "
            f"--threads={args.threads}"
        )
    try:
        allowed_cpus = os.sched_getaffinity(0)
    except AttributeError:
        allowed_cpus = set(range(os.cpu_count() or 1))
    unavailable_cpus = [cpu for cpu in cpu_ids if cpu not in allowed_cpus]
    if unavailable_cpus:
        raise SystemExit(
            "CPU(s) fora da afinidade permitida: "
            + ", ".join(map(str, unavailable_cpus))
        )
    cpu_set = ",".join(map(str, cpu_ids))

    compare = args.compare.resolve()
    if not compare.is_file() or not os.access(compare, os.X_OK):
        raise SystemExit(f"comparador ausente ou não executável: {compare}")
    baseline_compare = (
        args.baseline_compare.resolve() if args.baseline_compare is not None else None
    )
    if baseline_compare is not None and (
        not baseline_compare.is_file() or not os.access(baseline_compare, os.X_OK)
    ):
        raise SystemExit(
            f"comparador-base ausente ou não executável: {baseline_compare}"
        )
    if shutil.which("convert") is None or shutil.which("taskset") is None:
        raise SystemExit("convert (GraphicsMagick) e taskset são obrigatórios")

    frozen_manifest_path = (
        args.frozen_manifest.resolve() if args.frozen_manifest is not None else None
    )
    frozen_manifest_sha256 = None
    frozen_candidates: list[dict[str, Any]] | None = None
    if frozen_manifest_path is not None:
        if not frozen_manifest_path.is_file():
            raise SystemExit(f"manifesto congelado ausente: {frozen_manifest_path}")
        loaded_frozen = load_json(frozen_manifest_path, None)
        if not isinstance(loaded_frozen, list):
            raise SystemExit("manifesto congelado deve conter uma lista JSON")
        if len(loaded_frozen) < args.count:
            raise SystemExit(
                f"manifesto congelado tem {len(loaded_frozen)} imagens; "
                f"a execução pediu {args.count}"
            )
        frozen_candidates = []
        for expected_id, item in enumerate(loaded_frozen[: args.count], 1):
            if not isinstance(item, dict):
                raise SystemExit(f"entrada {expected_id} do manifesto não é um objeto")
            required = ("pageid", "title", "download_url", "width", "height", "ppm_sha256")
            missing = [field for field in required if field not in item]
            if missing:
                raise SystemExit(
                    f"entrada {expected_id} sem campos obrigatórios: {', '.join(missing)}"
                )
            if int(item.get("image_id", -1)) != expected_id:
                raise SystemExit(
                    f"ordem inválida no manifesto: esperado image_id {expected_id}"
                )
            frozen_candidates.append(dict(item))
        frozen_manifest_sha256 = sha256_file(frozen_manifest_path)

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    for stale_atomic_file in output_dir.glob(".atomic-*"):
        stale_atomic_file.unlink(missing_ok=True)
    work_dir = output_dir / ".work"
    work_dir.mkdir(exist_ok=True)
    source_path = work_dir / "source.download"
    ppm_path = work_dir / "input.ppm"
    source_path.unlink(missing_ok=True)
    ppm_path.unlink(missing_ok=True)

    candidate_path = output_dir / "candidates.json"
    manifest_path = output_dir / "manifest.json"
    results_path = output_dir / "results.csv"
    baseline_results_path = output_dir / "baseline-results.csv"
    failures_path = output_dir / "failures.json"
    metadata_path = output_dir / "metadata.json"
    report_path = output_dir / "RELATORIO.md"

    config = {
        "count": args.count,
        "qualities": args.qualities,
        "reps": args.reps,
        "chroma": args.chroma,
        "threads": args.threads,
        "cpu": cpu_ids[0],
        "cpu_set": cpu_set,
        "jpeg_cpu": cpu_ids[0],
        "max_dimension": args.max_dimension,
        "min_dimension": args.min_dimension,
        "max_download_mib": args.max_download_mib,
        "min_free_gib": args.min_free_gib,
        "download_attempts": args.download_attempts,
        "retry_base_seconds": args.retry_base_seconds,
        "active_provider": "frozen" if frozen_manifest_path else args.provider,
        "frozen_manifest": str(frozen_manifest_path) if frozen_manifest_path else None,
        "frozen_manifest_sha256": frozen_manifest_sha256,
        "baseline_compare": str(baseline_compare) if baseline_compare else None,
        "baseline_compare_sha256": (
            sha256_file(baseline_compare) if baseline_compare else None
        ),
        "api": API_URL,
        "selection": (
            "ordered replay of a frozen accepted manifest"
            if frozen_manifest_path
            else "random providers frozen in candidates.json"
        ),
    }
    metadata = load_json(metadata_path, {})
    if metadata:
        previous = metadata.get("config", {})
        for key in (
            "count", "qualities", "reps", "chroma", "threads", "cpu",
            "cpu_set", "jpeg_cpu",
            "max_dimension", "min_dimension", "frozen_manifest_sha256",
            "baseline_compare_sha256",
        ):
            if previous.get(key) != config[key]:
                raise SystemExit(f"configuração de retomada diverge em {key}")
        metadata["config"] = config
    else:
        metadata = {
            "status": "running",
            "started_at": now_iso(),
            "config": config,
            "environment": environment_metadata(compare, cpu_ids),
            "baseline_environment": (
                environment_metadata(baseline_compare, cpu_ids)
                if baseline_compare else None
            ),
            "disk_free_start": shutil.disk_usage(output_dir).free,
            "peak_temporary_bytes": 0,
            "downloaded_bytes_total": 0,
        }
        atomic_json(metadata_path, metadata)

    candidates: list[dict[str, Any]] = load_json(candidate_path, [])
    if frozen_candidates is not None:
        if candidates:
            current_identity = [
                (int(item["pageid"]), str(item.get("ppm_sha256", "")))
                for item in candidates
            ]
            frozen_identity = [
                (int(item["pageid"]), str(item["ppm_sha256"]))
                for item in frozen_candidates
            ]
            if current_identity != frozen_identity:
                raise SystemExit("candidates.json diverge do manifesto congelado")
        else:
            candidates = frozen_candidates
            atomic_json(candidate_path, candidates)
    manifest: list[dict[str, Any]] = load_json(manifest_path, [])
    for candidate in candidates:
        candidate.setdefault("provider", "commons")
    for accepted in manifest:
        accepted.setdefault("provider", "commons")
    results: list[dict[str, Any]] = load_results(results_path)
    baseline_results: list[dict[str, Any]] = load_results(baseline_results_path)
    failures: list[dict[str, Any]] = load_json(failures_path, [])
    retryable_failures = [
        failure
        for failure in failures
        if (frozen_manifest_path is not None or args.provider == "commons")
        and "HTTP Error 429" in str(failure.get("reason", ""))
    ]
    if retryable_failures:
        failures = [failure for failure in failures if failure not in retryable_failures]
        metadata["transient_failures_requeued"] = int(
            metadata.get("transient_failures_requeued", 0)
        ) + len(retryable_failures)
        atomic_json(failures_path, failures)
        atomic_json(metadata_path, metadata)

    expected_rows = len(manifest) * len(args.qualities)
    if len(results) != expected_rows:
        raise SystemExit(
            f"retomada inconsistente: {len(manifest)} imagens, {len(results)} linhas (esperado {expected_rows})"
        )
    if baseline_compare is not None and len(baseline_results) != expected_rows:
        raise SystemExit(
            f"retomada inconsistente no baseline: {len(manifest)} imagens, "
            f"{len(baseline_results)} linhas (esperado {expected_rows})"
        )
    if baseline_compare is None and baseline_results:
        raise SystemExit(
            "baseline-results.csv existe, mas --baseline-compare não foi informado"
        )
    accepted_ids = {int(item["pageid"]) for item in manifest}
    failed_ids = {int(item["pageid"]) for item in failures if "pageid" in item}

    maximum_download = args.max_download_mib * 1024 * 1024
    minimum_free = int(args.min_free_gib * 1024**3)
    while len(manifest) < args.count:
        untried = [
            candidate
            for candidate in candidates
            if (frozen_manifest_path is not None
                or candidate.get("provider") == args.provider)
            and int(candidate["pageid"]) not in accepted_ids | failed_ids
        ]
        if not untried:
            if frozen_manifest_path is not None:
                raise SystemExit(
                    "manifesto congelado esgotado antes de completar a execução; "
                    "consulte failures.json"
                )
            provider_candidates = sum(
                candidate.get("provider") == args.provider for candidate in candidates
            )
            desired = provider_candidates + max(100, args.count - len(manifest))
            print(
                f"Coletando candidatos aleatórios de {args.provider} até {desired}...",
                flush=True,
            )
            if args.provider == "commons":
                collect_candidates(
                    candidates, len(candidates) + max(100, args.count - len(manifest)),
                    args.min_dimension, args.max_dimension
                )
            else:
                collect_picsum_candidates(candidates, desired, args.max_dimension)
            atomic_json(candidate_path, candidates)
            untried = [
                candidate
                for candidate in candidates
                if candidate.get("provider") == args.provider
                and int(candidate["pageid"]) not in accepted_ids | failed_ids
            ]

        candidate = untried[0]
        pageid = int(candidate["pageid"])
        next_id = len(manifest) + 1
        title = str(candidate["title"])
        print(f"[{next_id:03d}/{args.count}] {short_title(title, 70)}", flush=True)
        try:
            free = shutil.disk_usage(output_dir).free
            if free < minimum_free:
                raise RuntimeError(
                    f"espaço livre caiu para {free / 1024**3:.2f} GiB, abaixo do limite"
                )
            downloaded_bytes = download(
                str(candidate["download_url"]),
                source_path,
                maximum_download,
                args.download_attempts,
                args.retry_base_seconds,
            )
            width, height = convert_to_ppm(source_path, ppm_path, args.max_dimension)
            ppm_bytes = ppm_path.stat().st_size
            ppm_sha256 = sha256_file(ppm_path)
            if frozen_manifest_path is not None:
                expected_width = int(candidate["width"])
                expected_height = int(candidate["height"])
                expected_sha256 = str(candidate["ppm_sha256"])
                if (width, height) != (expected_width, expected_height):
                    raise RuntimeError(
                        "entrada congelada mudou de dimensão: "
                        f"{width}x{height}, esperado {expected_width}x{expected_height}"
                    )
                if ppm_sha256 != expected_sha256:
                    raise RuntimeError(
                        "SHA-256 do PPM congelado divergiu: "
                        f"{ppm_sha256}, esperado {expected_sha256}"
                    )
            temporary_bytes = downloaded_bytes + ppm_bytes
            metadata["peak_temporary_bytes"] = max(
                int(metadata.get("peak_temporary_bytes", 0)), temporary_bytes
            )

            point_rows: list[dict[str, Any]] = []
            baseline_point_rows: list[dict[str, Any]] = []
            for quality_index, quality in enumerate(args.qualities):
                benchmark_arguments = (
                    ppm_path,
                    quality,
                    args.reps,
                    args.chroma,
                    args.threads,
                    cpu_set,
                    cpu_ids[0],
                )
                baseline_metrics = None
                if baseline_compare is not None and (next_id + quality_index) % 2 == 0:
                    baseline_metrics = benchmark_point(
                        baseline_compare, *benchmark_arguments
                    )
                metrics = benchmark_point(compare, *benchmark_arguments)
                if baseline_compare is not None and baseline_metrics is None:
                    baseline_metrics = benchmark_point(
                        baseline_compare, *benchmark_arguments
                    )

                identity = {
                    "image_id": next_id,
                    "pageid": pageid,
                    "width": width,
                    "height": height,
                    "pixels": width * height,
                    "quality": quality,
                    "chroma": args.chroma,
                    "threads": args.threads,
                    "reps": args.reps,
                }
                point_rows.append({**identity, **metrics})
                if baseline_metrics is not None:
                    baseline_point_rows.append({**identity, **baseline_metrics})

            accepted = {
                **candidate,
                "image_id": next_id,
                "width": width,
                "height": height,
                "downloaded_bytes": downloaded_bytes,
                "source_sha256": sha256_file(source_path),
                "ppm_bytes": ppm_bytes,
                "ppm_sha256": ppm_sha256,
                "accepted_at": now_iso(),
            }
            results.extend(point_rows)
            baseline_results.extend(baseline_point_rows)
            manifest.append(accepted)
            accepted_ids.add(pageid)
            metadata["downloaded_bytes_total"] = int(
                metadata.get("downloaded_bytes_total", 0)
            ) + downloaded_bytes
            metadata["completed_images"] = len(manifest)
            metadata["completed_points"] = len(results)
            metadata["last_checkpoint"] = now_iso()
            atomic_csv(results_path, results)
            if baseline_compare is not None:
                atomic_csv(baseline_results_path, baseline_results)
            atomic_json(manifest_path, manifest)
            atomic_json(metadata_path, metadata)
        except Exception as error:  # keep the long run moving and document exclusions
            failure = {
                "provider": candidate.get("provider", args.provider),
                "pageid": pageid,
                "title": title,
                "at": now_iso(),
                "reason": str(error)[:1000],
            }
            failures.append(failure)
            failed_ids.add(pageid)
            atomic_json(failures_path, failures)
            print(f"  descartada: {failure['reason']}", flush=True)
        finally:
            source_path.unlink(missing_ok=True)
            ppm_path.unlink(missing_ok=True)

    try:
        work_dir.rmdir()
    except OSError:
        pass
    metadata["status"] = "complete"
    metadata["finished_at"] = now_iso()
    metadata["disk_free_end"] = shutil.disk_usage(output_dir).free
    metadata["completed_images"] = len(manifest)
    metadata["completed_points"] = len(results)
    metadata["rejected_candidates"] = len(failures)
    atomic_json(failures_path, failures)
    atomic_json(metadata_path, metadata)
    report = render_report(output_dir, manifest, results, metadata, failures)
    atomic_text(report_path, report)
    print(f"Concluído: {len(manifest)} imagens, {len(results)} pontos.", flush=True)
    print(f"Relatório: {report_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

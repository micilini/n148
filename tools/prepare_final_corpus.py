#!/usr/bin/env python3
"""Build the licensed, classified corpus used by the final N.148i benchmark.

This is an additive extension of benchmark_random_corpus.py: it reuses its
atomic writers, bounded downloader, PPM parser, and hashing helper, while
adding strict license filtering, category quotas, persistent PPMs and full
attribution metadata.

Copyright (c) Micilini Roll. Licensed under the MIT License.
"""

from __future__ import annotations

import argparse
import hashlib
import html
from html.parser import HTMLParser
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Iterator

from benchmark_random_corpus import (
    API_URL,
    USER_AGENT,
    atomic_json,
    atomic_text,
    download,
    now_iso,
    ppm_dimensions,
    sha256_file,
)


SCHEMA_VERSION = 1
DEFAULT_PER_CATEGORY = 15
# These five caps provide resolution diversity. New entries are reconstructed
# from immutable original-upload URLs, not from on-demand thumbnails.
RESOLUTION_CAPS = (512, 640, 800, 1024, 1280)
ALLOWED_MIME = {"image/jpeg", "image/png", "image/tiff", "image/webp"}
ALLOWED_LICENSE = re.compile(
    r"^(?:CC0(?: 1\.0)?|Public domain|CC BY (?:1\.0|2\.0|2\.5|3\.0|4\.0))$",
    re.IGNORECASE,
)
OPENVERSE_API = "https://api.openverse.org/v1/images/"
PERSON_TERMS = re.compile(
    r"\b(?:people|person|woman|women|man|men|girl|boy|child|children|crowd|"
    r"portrait|selfie|family|wedding|protest|tourist|model)\b",
    re.IGNORECASE,
)
ART_TERMS = re.compile(
    r"\b(?:painting|painted|engrav(?:ing|ed)|drawing|illustration|lithograph|"
    r"watercolou?r|woodcut|etching|pastel|miniature)\b",
    re.IGNORECASE,
)
PHOTO_TERMS = re.compile(r"\b(?:photo|photograph|photographic|selfie)\b", re.I)


CATEGORIES: tuple[dict[str, Any], ...] = (
    {
        "id": "retratos_historicos",
        "label": "Retratos (obras 2D históricas; sem pessoa fotografada)",
        "queries": (
            'incategory:"CC-Zero" "portrait painting"',
            'incategory:"CC-Zero" portrait engraving',
            'incategory:"CC-Zero" portrait lithograph',
            'incategory:"CC-Zero" portrait illustration',
        ),
        "portrait_art": True,
    },
    {
        "id": "paisagens_naturais",
        "label": "Paisagens naturais",
        "queries": (
            'incategory:"CC-Zero" landscape mountain',
            'incategory:"CC-Zero" landscape coast',
            'incategory:"CC-Zero" forest landscape',
            'incategory:"CC-Zero" desert landscape',
        ),
    },
    {
        "id": "urbano_arquitetura",
        "label": "Cenas urbanas e arquitetura",
        "queries": (
            'incategory:"CC-Zero" architecture building exterior',
            'incategory:"CC-Zero" city architecture',
            'incategory:"CC-Zero" bridge architecture',
            'incategory:"CC-Zero" historic building',
        ),
    },
    {
        "id": "texturas_alta_frequencia",
        "label": "Texturas de alta frequência",
        "queries": (
            'incategory:"CC-Zero" grass texture',
            'incategory:"CC-Zero" fabric texture',
            'incategory:"CC-Zero" bark texture',
            'incategory:"CC-Zero" sand texture',
            'incategory:"CC-Zero" foliage texture',
        ),
    },
    {
        "id": "superficies_suaves_degrades",
        "label": "Superfícies suaves e degradês",
        "queries": (
            'incategory:"CC-Zero" blue sky clouds',
            'incategory:"CC-Zero" fog mist landscape',
            'incategory:"CC-Zero" calm water',
            'incategory:"CC-Zero" smooth gradient abstract',
        ),
    },
    {
        "id": "detalhe_fino",
        "label": "Muito detalhe fino",
        "queries": (
            'incategory:"CC-Zero" dense foliage',
            'incategory:"CC-Zero" aerial landscape detail',
            'incategory:"CC-Zero" intricate pattern',
            'incategory:"CC-Zero" flowers field',
        ),
    },
    {
        "id": "texto_bordas_nitidas",
        "label": "Texto ou bordas nítidas",
        "queries": (
            'incategory:"CC-Zero" diagram',
            'incategory:"CC-Zero" map',
            'incategory:"CC-Zero" typography poster',
            'incategory:"CC-Zero" sign text',
        ),
    },
    {
        "id": "cores_saturadas_dessaturadas",
        "label": "Cores saturadas e dessaturadas",
        "queries": (
            'incategory:"CC-Zero" colorful abstract',
            'incategory:"CC-Zero" vivid colors',
            'incategory:"CC-Zero" monochrome still life',
            'incategory:"CC-Zero" pastel colors',
        ),
    },
)


class _PlainText(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.parts: list[str] = []

    def handle_data(self, data: str) -> None:
        self.parts.append(data)


def plain_text(value: Any, limit: int = 1000) -> str:
    parser = _PlainText()
    try:
        parser.feed(str(value or ""))
    except Exception:
        parser.parts = [str(value or "")]
    text = html.unescape(" ".join(parser.parts))
    text = re.sub(r"\s+", " ", text).strip()
    return text[:limit] or "não informado"


def ext_value(info: dict[str, Any], key: str) -> str:
    return plain_text((info.get("extmetadata") or {}).get(key, {}).get("value"))


def search_pages(query: str) -> Iterator[dict[str, Any]]:
    continuation: dict[str, str] = {}
    for _ in range(20):
        parameters = {
            "action": "query",
            "format": "json",
            "formatversion": "2",
            "generator": "search",
            "gsrsearch": query,
            "gsrnamespace": "6",
            "gsrlimit": "25",
            "prop": "imageinfo",
            "iiprop": "url|size|mime|mediatype|sha1|extmetadata",
            "iiextmetadatafilter": (
                "LicenseShortName|LicenseUrl|Artist|Credit|ImageDescription|"
                "UsageTerms|AttributionRequired|Restrictions"
            ),
            **continuation,
        }
        request = urllib.request.Request(
            f"{API_URL}?{urllib.parse.urlencode(parameters)}",
            headers={"User-Agent": USER_AGENT},
        )
        with urllib.request.urlopen(request, timeout=60) as response:
            payload = json.load(response)
        yield from payload.get("query", {}).get("pages", [])
        continuation = payload.get("continue") or {}
        if not continuation:
            break


def commons_page(pageid: int) -> dict[str, Any]:
    """Fetch one explicitly curated Commons file with full attribution data."""
    parameters = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "pageids": str(pageid),
        "prop": "imageinfo",
        "iiprop": "url|size|mime|mediatype|sha1|extmetadata",
        "iiextmetadatafilter": (
            "LicenseShortName|LicenseUrl|Artist|Credit|ImageDescription|"
            "UsageTerms|AttributionRequired|Restrictions"
        ),
    }
    request = urllib.request.Request(
        f"{API_URL}?{urllib.parse.urlencode(parameters)}",
        headers={"User-Agent": USER_AGENT},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = json.load(response)
    pages = payload.get("query", {}).get("pages", [])
    if len(pages) != 1 or pages[0].get("missing"):
        raise RuntimeError(f"Commons pageid {pageid} was not found")
    return pages[0]


def openverse_pages(query: str) -> Iterator[dict[str, Any]]:
    """Expose Openverse results through the subset of imageinfo we consume."""
    query = re.sub(r'^incategory:"CC-Zero"\s*', "", query)
    for page_number in range(1, 11):
        parameters = {
            "q": query,
            "license": "cc0,pdm,by",
            "mature": "false",
            # Anonymous Openverse clients are capped at 20 results per page.
            "page_size": "20",
            "page": str(page_number),
        }
        request = urllib.request.Request(
            OPENVERSE_API + "?" + urllib.parse.urlencode(parameters),
            headers={"User-Agent": USER_AGENT},
        )
        with urllib.request.urlopen(request, timeout=60) as response:
            payload = json.load(response)
        results = payload.get("results") or []
        for item in results:
            provider = str(item.get("provider") or "").lower()
            source_name = str(item.get("source") or "").lower()
            if "wikimedia" in provider or "wikimedia" in source_name:
                continue
            license_slug = str(item.get("license") or "").lower()
            version = str(item.get("license_version") or "").strip()
            if license_slug == "cc0":
                license_name = "CC0 1.0"
            elif license_slug == "pdm":
                license_name = "Public domain"
            elif license_slug == "by" and version in {"1.0", "2.0", "2.5", "3.0", "4.0"}:
                license_name = f"CC BY {version}"
            else:
                continue
            url = str(item.get("url") or "")
            suffix = Path(urllib.parse.urlparse(url).path).suffix.lower()
            filetype = str(item.get("filetype") or suffix.lstrip(".")).lower()
            mime = {
                "jpg": "image/jpeg", "jpeg": "image/jpeg", "png": "image/png",
                "tif": "image/tiff", "tiff": "image/tiff", "webp": "image/webp",
            }.get(filetype)
            if mime is None:
                continue
            identifier = str(item.get("id") or "")
            if not identifier:
                continue
            synthetic_pageid = -int(hashlib.sha256(identifier.encode()).hexdigest()[:15], 16)
            description = str((item.get("meta_data") or {}).get("description") or "")
            tags = " ".join(str(tag.get("name") or "") for tag in (item.get("tags") or []))
            yield {
                "pageid": synthetic_pageid,
                "title": str(item.get("title") or f"Openverse {identifier}"),
                "openverse_id": identifier,
                "openverse_provider": provider,
                "openverse_source": source_name,
                "openverse_tags": tags,
                "imageinfo": [{
                    "mediatype": "BITMAP",
                    "mime": mime,
                    "width": int(item.get("width") or 0),
                    "height": int(item.get("height") or 0),
                    "size": int(item.get("filesize") or 0),
                    "url": url,
                    "descriptionurl": str(item.get("foreign_landing_url") or ""),
                    "sha1": "",
                    "extmetadata": {
                        "LicenseShortName": {"value": license_name},
                        "LicenseUrl": {"value": str(item.get("license_url") or "")},
                        "Artist": {"value": str(item.get("creator") or "")},
                        "Credit": {"value": str(item.get("creator_url") or "")},
                        "ImageDescription": {"value": description + " " + tags},
                        "AttributionRequired": {"value": "true" if license_slug == "by" else "false"},
                        "Restrictions": {"value": ""},
                    },
                }],
            }
        if not payload.get("next") or not results:
            break


def acceptable(page: dict[str, Any], category: dict[str, Any],
               used_pageids: set[int], min_dimension: int) -> tuple[bool, str]:
    pageid = int(page.get("pageid") or 0)
    imageinfo = page.get("imageinfo") or []
    if not pageid or pageid in used_pageids or not imageinfo:
        return False, "duplicate_or_missing"
    info = imageinfo[0]
    if info.get("mediatype") != "BITMAP" or info.get("mime") not in ALLOWED_MIME:
        return False, "not_supported_bitmap"
    width = int(info.get("width") or 0)
    height = int(info.get("height") or 0)
    if min(width, height) < min_dimension or min(width, height) * 4 < max(width, height):
        return False, "dimensions"
    license_name = ext_value(info, "LicenseShortName")
    if not ALLOWED_LICENSE.fullmatch(license_name):
        return False, f"license:{license_name}"
    restrictions = ext_value(info, "Restrictions")
    if restrictions != "não informado":
        return False, f"restrictions:{restrictions}"
    title_and_description = (
        str(page.get("title") or "") + " " + ext_value(info, "ImageDescription")
    )
    if category.get("portrait_art"):
        if not ART_TERMS.search(title_and_description) or PHOTO_TERMS.search(
            title_and_description
        ):
            return False, "portrait_not_verified_2d_art"
    elif PERSON_TERMS.search(title_and_description):
        return False, "possible_identifiable_person"
    return True, "ok"


def identify_channels(path: Path) -> str:
    result = subprocess.run(
        ["identify", "-format", "%[channels]", f"{path}[0]"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=60,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def convert_final(
    source: Path, ppm: Path, cap: int, convert_binary: str
) -> tuple[int, int, list[str]]:
    command = [
        convert_binary,
        f"{source}[0]",
        "-auto-orient",
        "-colorspace",
        "sRGB",
        "-resize",
        f"{cap}x{cap}>",
        "-background",
        "white",
        "-flatten",
        "-strip",
        "-depth",
        "8",
        f"PPM:{ppm}",
    ]
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=240,
    )
    if result.returncode != 0:
        message = result.stderr.strip().replace("\n", " ")
        raise RuntimeError(f"conversion failed: {message[:500]}")
    width, height = ppm_dimensions(ppm)
    if max(width, height) > cap:
        raise RuntimeError(f"conversion exceeded cap: {width}x{height} > {cap}")
    expected_minimum = width * height * 3
    if ppm.stat().st_size < expected_minimum:
        raise RuntimeError("truncated PPM")
    return width, height, command


def converter_version(convert_binary: str) -> str:
    result = subprocess.run(
        [convert_binary, "-version"], check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=20
    )
    return result.stdout.splitlines()[0].strip()


def write_documents(manifest: list[dict[str, Any]], root: Path,
                    generated_at: str, version: str) -> None:
    document = {
        "schema_version": SCHEMA_VERSION,
        "project": "N.148i final benchmark corpus",
        "copyright": "Copyright (c) Micilini Roll",
        "project_license": "MIT",
        "generated_at": generated_at,
        "source_policy": (
            "Wikimedia Commons and Openverse; CC0, public domain, or CC BY only"
        ),
        "converter": version,
        "conversion_policy": (
            "first frame; auto-orient; sRGB; bounded resize without upscaling; "
            "alpha flattened on white; metadata stripped; 8-bit binary P6"
        ),
        "jpeg_source_policy": (
            "every JPEG source is spatially resampled at least once; sources "
            "that would otherwise retain their dimensions are reduced by 10%, "
            "while keeping the shorter side at least 320 pixels, to avoid "
            "source-DCT-grid recompression artifacts"
        ),
        "images": manifest,
    }
    atomic_json(root / "benchmarks" / "corpus-manifest.json", document)
    atomic_json(root / "images" / "MANIFEST.json", document)

    lines = [
        "N.148i — créditos do corpus do benchmark final",
        "Copyright (c) Micilini Roll. Projeto sob licença MIT.",
        "",
        "Cada item abaixo conserva a licença da obra de origem; a inclusão no",
        "repositório não relicencia fotografias ou obras de terceiros como MIT.",
        "Os PPMs são adaptações normalizadas conforme images/MANIFEST.json:",
        "redimensionamento, conversão sRGB, remoção de metadados e alpha.",
        "",
    ]
    for item in manifest:
        lines.extend(
            [
                item["filename"],
                f"  Título: {item['title']}",
                f"  Autor: {item['author']}",
                f"  Licença: {item['license']}",
                f"  URL da licença: {item['license_url']}",
                f"  Página de origem: {item['source_page_url']}",
                f"  Arquivo baixado: {item['download_url']}",
                f"  Provedor: {item.get('provider', 'wikimedia_commons')}",
                f"  Categoria: {item['category_label']}",
                "",
            ]
        )
    atomic_text(root / "images" / "CREDITOS.txt", "\n".join(lines))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--per-category", type=int, default=DEFAULT_PER_CATEGORY)
    parser.add_argument("--min-dimension", type=int, default=320)
    parser.add_argument("--max-download-mib", type=int, default=48)
    parser.add_argument(
        "--convert-binary", default="convert",
        help="ImageMagick convert executable (default: convert)",
    )
    parser.add_argument("--attempts", type=int, default=4)
    parser.add_argument(
        "--provider", choices=("openverse", "commons"), default="openverse",
        help="metadata/search provider for new entries (default: openverse)",
    )
    parser.add_argument(
        "--politeness-seconds", type=float, default=10.0,
        help="pause after each accepted Commons download (default: 10)",
    )
    parser.add_argument(
        "--renormalize-existing", action="store_true",
        help="rebuild every manifested PPM with --convert-binary and update hashes",
    )
    parser.add_argument(
        "--replace-pageid", action="append", default=[], metavar="IMAGE_ID:PAGE_ID",
        help=(
            "replace one entry after visual curation with an explicit Wikimedia "
            "Commons pageid; may be repeated"
        ),
    )
    parser.add_argument(
        "--resample-unchanged-jpeg", action="store_true",
        help=(
            "force one deterministic spatial resampling for manifested JPEG "
            "sources whose normalized dimensions still equal the source"
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.per_category < 1 or args.min_dimension < 1:
        raise SystemExit("per-category and min-dimension must be positive")
    root = args.root.resolve()
    images_dir = root / "images"
    benchmark_dir = root / "benchmarks"
    images_dir.mkdir(parents=True, exist_ok=True)
    benchmark_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = benchmark_dir / "corpus-manifest.json"
    if manifest_path.exists():
        loaded = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest = list(loaded.get("images", []))
        generated_at = str(loaded.get("generated_at") or now_iso())
    else:
        manifest = []
        generated_at = now_iso()
    for item in manifest:
        item.setdefault("provider", "wikimedia_commons")
        item.setdefault("source_id", str(item.get("pageid", "")))
    expected_existing = list(range(1, len(manifest) + 1))
    if [int(item.get("image_id", -1)) for item in manifest] != expected_existing:
        raise SystemExit("existing manifest image_id sequence is invalid")
    for item in manifest:
        path = images_dir / item["filename"]
        if not path.is_file() or sha256_file(path) != item["ppm_sha256"]:
            raise SystemExit(f"existing corpus file differs: {path}")

    used_pageids = {int(item["pageid"]) for item in manifest}
    version = converter_version(args.convert_binary)
    maximum_bytes = args.max_download_mib * 1024 * 1024
    work_dir = benchmark_dir / ".work" / "corpus"
    work_dir.mkdir(parents=True, exist_ok=True)

    if args.resample_unchanged_jpeg:
        revised = 0
        for item in manifest:
            if not item.get("source_is_jpeg"):
                continue
            original_width = int(item.get("original_width") or 0)
            original_height = int(item.get("original_height") or 0)
            if (
                int(item["width"]) != original_width or
                int(item["height"]) != original_height
            ):
                continue
            longest = max(original_width, original_height)
            shortest = min(original_width, original_height)
            if shortest < args.min_dimension or longest < 2:
                raise RuntimeError(
                    f"cannot safely resample {item['filename']}: "
                    f"{original_width}x{original_height}"
                )
            ninety_percent = int(math.floor(longest * 0.9))
            preserve_minimum = int(
                math.ceil(longest * args.min_dimension / shortest)
            )
            cap = max(ninety_percent, preserve_minimum)
            if cap >= longest:
                cap = longest - 1
            if cap < 1:
                raise RuntimeError(f"invalid forced cap for {item['filename']}")
            command = list(item.get("conversion_argv") or [])
            try:
                resize_index = command.index("-resize") + 1
                command[resize_index] = f"{cap}x{cap}>"
            except (ValueError, IndexError) as error:
                raise RuntimeError(
                    f"manifest has no reproducible resize for {item['filename']}"
                ) from error
            item["resolution_cap"] = cap
            item["conversion_argv"] = command
            item["forced_source_resample"] = True
            item["forced_source_resample_reason"] = (
                "break the original JPEG DCT grid and avoid artificial "
                "same-quality recompression peaks"
            )
            item["normalization_converter"] = ""
            revised += 1
        if revised:
            write_documents(manifest, root, generated_at, version)
            args.renormalize_existing = True
        print(f"JPEG entries scheduled for forced resampling: {revised}")

    with tempfile.TemporaryDirectory(prefix="n148-corpus-", dir=work_dir) as temp:
        temporary = Path(temp)
        source = temporary / "source.download"
        pending_ppm = temporary / "converted.ppm"
        if args.replace_pageid:
            replacements: list[tuple[int, int]] = []
            for specification in args.replace_pageid:
                try:
                    image_text, page_text = specification.split(":", 1)
                    image_id, pageid = int(image_text), int(page_text)
                except (TypeError, ValueError) as error:
                    raise SystemExit(
                        f"invalid --replace-pageid value: {specification!r}"
                    ) from error
                if image_id < 1 or image_id > len(manifest) or pageid < 1:
                    raise SystemExit(
                        f"replacement outside valid range: {specification!r}"
                    )
                replacements.append((image_id, pageid))
            if len({image_id for image_id, _ in replacements}) != len(replacements):
                raise SystemExit("each IMAGE_ID may be replaced only once per run")

            categories = {category["id"]: category for category in CATEGORIES}
            for ordinal, (image_id, pageid) in enumerate(replacements, 1):
                old_item = manifest[image_id - 1]
                category = categories[str(old_item["category"])]
                page = commons_page(pageid)
                accepted, reason = acceptable(
                    page, category,
                    used_pageids - {int(old_item["pageid"])},
                    args.min_dimension,
                )
                if not accepted:
                    raise RuntimeError(
                        f"curated Commons pageid {pageid} was rejected: {reason}"
                    )
                info = page["imageinfo"][0]
                source_url = str(info.get("url") or "")
                if not source_url.startswith("https://"):
                    raise RuntimeError(f"Commons pageid {pageid} has no HTTPS source")
                if int(info.get("size") or 0) > maximum_bytes:
                    raise RuntimeError(f"Commons pageid {pageid} exceeds download limit")
                cap = int(old_item["resolution_cap"])
                filename = str(old_item["filename"])
                print(
                    f"[{ordinal}/{len(replacements)}] replacing {filename} with "
                    f"{page.get('title')}",
                    flush=True,
                )
                source.unlink(missing_ok=True)
                pending_ppm.unlink(missing_ok=True)
                downloaded_bytes = download(
                    source_url, source, maximum_bytes, args.attempts, 2.0
                )
                channels = identify_channels(source)
                width, height, _ = convert_final(
                    source, pending_ppm, cap, args.convert_binary
                )
                destination = images_dir / filename
                shutil.move(pending_ppm, destination)
                license_name = ext_value(info, "LicenseShortName")
                license_url = ext_value(info, "LicenseUrl")
                if (
                    license_url == "não informado" and
                    license_name.lower().startswith("cc0")
                ):
                    license_url = "https://creativecommons.org/publicdomain/zero/1.0/"
                item = {
                    "image_id": image_id,
                    "filename": filename,
                    "pageid": int(page["pageid"]),
                    "provider": "wikimedia_commons",
                    "source_id": str(page["pageid"]),
                    "openverse_id": "",
                    "openverse_source": "",
                    "title": plain_text(page.get("title"), 500),
                    "description": ext_value(info, "ImageDescription"),
                    "author": ext_value(info, "Artist"),
                    "credit": ext_value(info, "Credit"),
                    "license": license_name,
                    "license_url": license_url,
                    "attribution_required": ext_value(info, "AttributionRequired"),
                    "source_page_url": str(info.get("descriptionurl") or ""),
                    "download_url": source_url,
                    "original_url": source_url,
                    "source_mime": str(info.get("mime") or "unknown"),
                    "source_is_jpeg": info.get("mime") == "image/jpeg",
                    "source_sha1_commons": str(info.get("sha1") or ""),
                    "download_sha256": sha256_file(source),
                    "downloaded_bytes": downloaded_bytes,
                    "original_width": int(info.get("width") or 0),
                    "original_height": int(info.get("height") or 0),
                    "api_thumb_width": 0,
                    "api_thumb_height": 0,
                    "source_channels": channels,
                    "width": width,
                    "height": height,
                    "pixels": width * height,
                    "resolution_cap": cap,
                    "category": category["id"],
                    "category_label": category["label"],
                    "classification_query": f"curated Commons pageid:{pageid}",
                    "contains_identifiable_photographed_person": False,
                    "curation_reason": (
                        "manual visual audit: duplicate or identifiable modern "
                        "photographed person replaced by historical 2D artwork"
                    ),
                    "ppm_format": "P6 RGB 8-bit",
                    "ppm_sha256": sha256_file(destination),
                    "conversion_argv": [
                        "convert", "SOURCE[0]", "-auto-orient", "-colorspace",
                        "sRGB", "-resize", f"{cap}x{cap}>", "-background",
                        "white", "-flatten", "-strip", "-depth", "8",
                        f"PPM:images/{filename}",
                    ],
                    "retrieved_at": now_iso(),
                    "normalization_converter": version,
                    "normalization_revised_at": now_iso(),
                }
                used_pageids.discard(int(old_item["pageid"]))
                used_pageids.add(int(page["pageid"]))
                manifest[image_id - 1] = item
                write_documents(manifest, root, generated_at, version)
                source.unlink(missing_ok=True)
                time.sleep(max(12.0, args.politeness_seconds))
            print(f"replacement complete: {len(replacements)} images")
            return 0

        if args.renormalize_existing:
            for ordinal, item in enumerate(manifest, 1):
                destination = images_dir / item["filename"]
                if (
                    item.get("normalization_converter") == version and
                    destination.is_file() and
                    sha256_file(destination) == item["ppm_sha256"]
                ):
                    print(f"[{ordinal}/{len(manifest)}] already normalized {item['filename']}")
                    continue
                print(f"[{ordinal}/{len(manifest)}] renormalizing {item['filename']}", flush=True)
                source.unlink(missing_ok=True)
                pending_ppm.unlink(missing_ok=True)
                downloaded_bytes = download(
                    str(item["download_url"]), source, maximum_bytes,
                    args.attempts, 2.0
                )
                actual_source_hash = sha256_file(source)
                expected_source_hash = str(item.get("download_sha256") or "")
                if expected_source_hash and actual_source_hash != expected_source_hash:
                    raise RuntimeError(
                        f"source changed for {item['filename']}: "
                        f"{actual_source_hash} != {expected_source_hash}"
                    )
                width, height, _ = convert_final(
                    source, pending_ppm, int(item["resolution_cap"]),
                    args.convert_binary,
                )
                shutil.move(pending_ppm, destination)
                item["downloaded_bytes"] = downloaded_bytes
                item["download_sha256"] = actual_source_hash
                item["width"] = width
                item["height"] = height
                item["pixels"] = width * height
                item["ppm_sha256"] = sha256_file(destination)
                item["normalization_converter"] = version
                item["normalization_revised_at"] = now_iso()
                write_documents(manifest, root, generated_at, version)
                source.unlink(missing_ok=True)
                delay = max(0.0, args.politeness_seconds)
                if item.get("provider") == "wikimedia_commons":
                    delay = max(delay, 12.0)
                time.sleep(delay)
            print(f"renormalization complete: {len(manifest)} images")
            return 0

        for category in CATEGORIES:
            have = sum(item["category"] == category["id"] for item in manifest)
            if have >= args.per_category:
                continue
            print(f"{category['id']}: {have}/{args.per_category}", flush=True)
            per_query_quota = math.ceil(args.per_category / len(category["queries"]))
            for query in category["queries"]:
                if have >= args.per_category:
                    break
                have_query = sum(
                    item["category"] == category["id"] and
                    item.get("classification_query") == query
                    for item in manifest
                )
                if have_query >= per_query_quota:
                    continue
                page_iterator = (
                    openverse_pages(query) if args.provider == "openverse"
                    else search_pages(query)
                )
                for page in page_iterator:
                    if have >= args.per_category or have_query >= per_query_quota:
                        break
                    accepted, reason = acceptable(
                        page, category, used_pageids, args.min_dimension
                    )
                    if not accepted:
                        continue
                    info = page["imageinfo"][0]
                    source_url = str(info.get("url") or "")
                    if not source_url.startswith("https://"):
                        continue
                    if int(info.get("size") or 0) > maximum_bytes:
                        continue
                    image_id = len(manifest) + 1
                    filename = f"corpus-{image_id:04d}.ppm"
                    cap = RESOLUTION_CAPS[(image_id - 1) % len(RESOLUTION_CAPS)]
                    print(f"  {filename}: {page.get('title')} [{query}]", flush=True)
                    try:
                        downloaded_bytes = download(
                            source_url, source, maximum_bytes, args.attempts, 2.0
                        )
                        channels = identify_channels(source)
                        width, height, command = convert_final(
                            source, pending_ppm, cap, args.convert_binary
                        )
                    except Exception as error:
                        print(f"    rejected after download: {error}", file=sys.stderr)
                        source.unlink(missing_ok=True)
                        pending_ppm.unlink(missing_ok=True)
                        continue
                    destination = images_dir / filename
                    shutil.move(pending_ppm, destination)
                    license_name = ext_value(info, "LicenseShortName")
                    license_url = ext_value(info, "LicenseUrl")
                    if license_url == "não informado" and license_name.lower().startswith("cc0"):
                        license_url = "https://creativecommons.org/publicdomain/zero/1.0/"
                    item = {
                        "image_id": image_id,
                        "filename": filename,
                        "pageid": int(page["pageid"]),
                        "provider": (
                            f"openverse:{page.get('openverse_provider', 'unknown')}"
                            if page.get("openverse_id") else "wikimedia_commons"
                        ),
                        "source_id": str(page.get("openverse_id") or page["pageid"]),
                        "openverse_id": str(page.get("openverse_id") or ""),
                        "openverse_source": str(page.get("openverse_source") or ""),
                        "title": plain_text(page.get("title"), 500),
                        "description": ext_value(info, "ImageDescription"),
                        "author": ext_value(info, "Artist"),
                        "credit": ext_value(info, "Credit"),
                        "license": license_name,
                        "license_url": license_url,
                        "attribution_required": ext_value(info, "AttributionRequired"),
                        "source_page_url": str(info.get("descriptionurl") or ""),
                        "download_url": source_url,
                        "original_url": str(info.get("url") or ""),
                        "source_mime": str(info.get("mime") or "unknown"),
                        "source_is_jpeg": info.get("mime") == "image/jpeg",
                        "source_sha1_commons": str(info.get("sha1") or ""),
                        "download_sha256": sha256_file(source),
                        "downloaded_bytes": downloaded_bytes,
                        "original_width": int(info.get("width") or 0),
                        "original_height": int(info.get("height") or 0),
                        "api_thumb_width": int(info.get("thumbwidth") or 0),
                        "api_thumb_height": int(info.get("thumbheight") or 0),
                        "source_channels": channels,
                        "width": width,
                        "height": height,
                        "pixels": width * height,
                        "resolution_cap": cap,
                        "category": category["id"],
                        "category_label": category["label"],
                        "classification_query": query,
                        "contains_identifiable_photographed_person": False,
                        "ppm_format": "P6 RGB 8-bit",
                        "ppm_sha256": sha256_file(destination),
                        "conversion_argv": [
                            "convert",
                            "SOURCE[0]",
                            "-auto-orient",
                            "-colorspace",
                            "sRGB",
                            "-resize",
                            f"{cap}x{cap}>",
                            "-background",
                            "white",
                            "-flatten",
                            "-strip",
                            "-depth",
                            "8",
                            f"PPM:images/{filename}",
                        ],
                        "retrieved_at": now_iso(),
                    }
                    manifest.append(item)
                    used_pageids.add(int(page["pageid"]))
                    have += 1
                    have_query += 1
                    write_documents(manifest, root, generated_at, version)
                    source.unlink(missing_ok=True)
                    time.sleep(max(0.0, args.politeness_seconds))
            if have < args.per_category:
                raise RuntimeError(
                    f"category {category['id']} stopped at {have}/{args.per_category}"
                )
    write_documents(manifest, root, generated_at, version)
    print(f"complete: {len(manifest)} images", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

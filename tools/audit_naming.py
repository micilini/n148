#!/usr/bin/env python3
"""Reject implementation-round nomenclature in the active N.148i tree.

Normative names such as N148I_FORMAT_VERSION_7 are intentionally accepted:
their number determines the on-stream syntax. Historical documents and the
published benchmark archives are outside this implementation audit.

Copyright (c) Micilini Roll. Licensed under the MIT License.
"""

from __future__ import annotations

import re
from pathlib import Path


OLD_PATH = re.compile(r"(?:^|/)v[1-7]_(?:codec|intra)\.[ch]$")
OLD_CONTENT = (
    re.compile(r"\bN148I?_V[1-7](?:_|\b)"),
    re.compile(r"\bn148_v[1-7](?:_|\b)"),
    re.compile(r"\bN148V[1-7][A-Za-z0-9_]*"),
    re.compile(r"\bV[1-7]_[A-Za-z0-9_]"),
    re.compile(r"\bv[1-7]_[A-Za-z0-9_]"),
    re.compile(r"\bv[3-7]\b", re.IGNORECASE),
    re.compile(r"\b7\.[01]\.\d+\b"),
    re.compile(r"\b(?:phase|stage)[ _-]?[0-9]+\b", re.IGNORECASE),
)


def audited_paths(root: Path) -> list[Path]:
    paths: list[Path] = []
    for directory in ("src", "include", "examples"):
        base = root / directory
        paths.extend(path for path in base.rglob("*") if path.suffix in (".c", ".h", ".inc"))
    paths.extend((root / "packaging").glob("*.txt"))
    paths.extend((root / "Makefile", root / "CMakeLists.txt"))
    return sorted(set(paths))


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    failures: list[str] = []
    for path in audited_paths(root):
        relative = path.relative_to(root).as_posix()
        if OLD_PATH.search(relative):
            failures.append(f"{relative}: phase-named implementation file")
        text = path.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), 1):
            for pattern in OLD_CONTENT:
                match = pattern.search(line)
                if match:
                    failures.append(
                        f"{relative}:{number}: implementation-round token "
                        f"{match.group(0)!r}"
                    )
                    break
    if failures:
        print("\n".join(failures))
        return 1
    print(f"naming audit PASS ({len(audited_paths(root))} active files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Regenerates data/public-hash-list.bin from an upstream
public-hash-list.dat snapshot (https://github.com/WICG/cross-origin-storage,
public-hash-list/implementation/data/public-hash-list.dat -- fetch the real
bytes from media.githubusercontent.com, not raw.githubusercontent.com, which
serves only a Git LFS pointer for that path).

Extracts the three sections the source file's own README documents as
MUST-or-SHOULD-adopt (core popularity-corroborated, Hugging Face model-hub,
and manual additions -- see that README's Output format / Model-hub source /
Manual additions sections for the exact semantics of each), validates every
entry is a 64-character lowercase hex SHA-256 digest, deduplicates, sorts,
and packs the result as raw 32-byte digests -- CrossOriginStoragePublicHashList
binary-searches this file directly rather than the hex text, so lookups never
need to touch the ~2x larger textual form at runtime.

Usage:
    python3 generate_public_hash_list.py path/to/public-hash-list.dat
"""

import hashlib
import re
import sys
from pathlib import Path

SECTIONS = [
    ("=== BEGIN SHA-256 ===", "=== END SHA-256 ==="),
    ("=== BEGIN SHA-256 HUGGING-FACE ===", "=== END SHA-256 HUGGING-FACE ==="),
    ("=== BEGIN SHA-256 MANUAL ===", "=== END SHA-256 MANUAL ==="),
]

HEX64 = re.compile(r"^[0-9a-f]{64}$")


def extract_section(lines, begin_marker, end_marker):
    begin = begin_marker.replace(" ", "")
    end = end_marker.replace(" ", "")
    in_section = False
    for line in lines:
        stripped = line.strip()
        marker = stripped.replace(" ", "").lstrip("/").strip()
        if marker == begin:
            in_section = True
            continue
        if marker == end:
            in_section = False
            continue
        if not in_section:
            continue
        if stripped.startswith("//") or not stripped:
            continue
        yield stripped


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} path/to/public-hash-list.dat", file=sys.stderr)
        return 1

    src = Path(sys.argv[1])
    lines = src.read_text(encoding="utf-8").splitlines()

    digests = set()
    for begin, end in SECTIONS:
        count_before = len(digests)
        for entry in extract_section(lines, begin, end):
            if not HEX64.match(entry):
                print(f"skipping malformed entry: {entry!r}", file=sys.stderr)
                continue
            digests.add(entry)
        print(f"{begin}: {len(digests) - count_before} entries")

    sorted_digests = sorted(digests)
    print(f"total unique entries: {len(sorted_digests)}")

    blob = b"".join(bytes.fromhex(d) for d in sorted_digests)

    out_dir = Path(__file__).parent / "data"
    out_dir.mkdir(exist_ok=True)
    out_path = out_dir / "public-hash-list.bin"
    out_path.write_bytes(blob)

    sha256 = hashlib.sha256(blob).hexdigest()
    print(f"wrote {out_path} ({len(blob)} bytes, {len(sorted_digests)} entries)")
    print(f"sha256: {sha256}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

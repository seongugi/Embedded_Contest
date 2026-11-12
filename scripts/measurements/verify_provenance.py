#!/usr/bin/env python3
"""Verify current published measurement bytes and optional archived sources.

Run from any directory:
    python scripts/measurements/verify_provenance.py
    python scripts/measurements/verify_provenance.py --source-root RUN_DIRECTORY

RUN_DIRECTORY contains the original analysis/ directory. The verifier is read-only;
it never regenerates hashes or reads HEAD in place of the current working files.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys


def relative_file(root, name):
    """Resolve manifest paths without permitting absolute paths or traversal."""
    path = PurePosixPath(name.replace("\\", "/"))
    if path.is_absolute() or ".." in path.parts or ":" in name or not path.parts:
        raise ValueError(f"invalid relative manifest path: {name!r}")
    result = root.joinpath(*path.parts).resolve()
    if not result.is_relative_to(root.resolve()):
        raise ValueError(f"manifest path escapes its root: {name!r}")
    return result


def canonical_bytes(raw):
    """Match UTF-8/LF Git blobs, preserving BOMs and accepting CRLF checkouts."""
    raw.decode("utf-8")
    result = raw.replace(b"\r\n", b"\n")
    if b"\r" in result:
        raise ValueError("bare CR line endings are not an approved transformation")
    return result


def require_hash(raw, expected, label):
    actual = hashlib.sha256(raw).hexdigest()
    if actual != expected:
        raise ValueError(f"{label}: SHA-256 mismatch (expected {expected}, got {actual})")


def string_value_span(document, pointer):
    """Find one top-level JSON string token without reserializing the object."""
    if not pointer.startswith("/") or "/" in pointer[1:]:
        raise ValueError(f"unsupported JSON pointer: {pointer!r}")
    key = pointer[1:].replace("~1", "/").replace("~0", "~")
    decoder = json.JSONDecoder()
    pos = len(document) - len(document.lstrip())
    if document[pos:pos + 1] != "{":
        raise ValueError("path redaction requires a JSON object")
    pos += 1
    found = []
    while True:
        while pos < len(document) and document[pos].isspace():
            pos += 1
        if document[pos:pos + 1] == "}":
            pos += 1
            break
        member, pos = decoder.raw_decode(document, pos)
        if not isinstance(member, str):
            raise ValueError("JSON object member is not a string")
        while pos < len(document) and document[pos].isspace():
            pos += 1
        if document[pos:pos + 1] != ":":
            raise ValueError("missing JSON object colon")
        pos += 1
        while pos < len(document) and document[pos].isspace():
            pos += 1
        start = pos
        value, pos = decoder.raw_decode(document, pos)
        if member == key:
            if not isinstance(value, str):
                raise ValueError(f"{pointer} is not a JSON string")
            found.append((start, pos))
        while pos < len(document) and document[pos].isspace():
            pos += 1
        if document[pos:pos + 1] == ",":
            pos += 1
        elif document[pos:pos + 1] != "}":
            raise ValueError("missing JSON object separator")
    if document[pos:].strip() or len(found) != 1:
        raise ValueError(f"expected exactly one {pointer} in a complete JSON object")
    return found[0]


def redact_path(raw, rule):
    """Replace only the declared string token; preserve every other source byte."""
    text = raw.decode("utf-8")
    offset, document = 0, text
    if rule["container"] == "html_script_json":
        script_id = re.escape(rule["script_id"])
        matches = [
            match for match in re.finditer(
                r"<script\b(?P<attrs>[^>]*)>(?P<data>.*?)</script\s*>",
                text, re.IGNORECASE | re.DOTALL,
            )
            if re.search(r'\bid\s*=\s*([\"\'])' + script_id + r"\1", match["attrs"])
        ]
        if len(matches) != 1:
            raise ValueError(f"expected one JSON script with id {rule['script_id']!r}")
        offset = matches[0].start("data")
        document = matches[0]["data"]
    elif rule["container"] != "json":
        raise ValueError(f"unsupported redaction container: {rule['container']!r}")
    start, end = string_value_span(document, rule["json_pointer"])
    replacement = json.dumps(rule["replacement"], ensure_ascii=False)
    return (text[:offset + start] + replacement + text[offset + end:]).encode("utf-8")


def verify(metrics_path, source_root=None):
    metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
    provenance = metrics["provenance"]
    if provenance["schema_version"] != 1 or provenance["newline_transform"] != "CRLF_to_LF":
        raise ValueError("unsupported provenance schema or newline transformation")
    files = provenance["files"]
    sources = [entry["source"] for entry in files]
    published = [entry["published"] for entry in files]
    if (len(sources) != len(set(sources)) or len(published) != len(set(published))
            or set(sources) != set(metrics["source_sha256"])
            or set(published) != set(metrics["published_sha256"])):
        raise ValueError("provenance mapping must cover every source and published hash exactly once")
    for entry in files:
        name = entry["published"]
        current = canonical_bytes(relative_file(metrics_path.parent, name).read_bytes())
        require_hash(current, metrics["published_sha256"][name], name)
        if source_root is not None:
            source_name = entry["source"]
            original = relative_file(source_root, source_name).read_bytes()
            require_hash(original, metrics["source_sha256"][source_name], source_name)
            transformed = original
            if "path_redaction" in entry:
                transformed = redact_path(transformed, entry["path_redaction"])
            if canonical_bytes(transformed) != current:
                raise ValueError(f"{name}: archived source does not match after approved transformations")
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--metrics", type=Path,
        default=Path(__file__).resolve().parents[2] / "docs/measurements/metrics.json",
        help="manifest containing the recorded hashes (default: repository metrics.json)",
    )
    parser.add_argument(
        "--source-root", type=Path,
        help="archived run directory containing analysis/; also verify original bytes and transformations",
    )
    args = parser.parse_args()
    try:
        count = verify(args.metrics.resolve(), args.source_root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"PASS: {count} published files match their UTF-8/LF hashes in the current working tree.")
    if args.source_root is not None:
        print(f"PASS: {count} archived source hashes and exact publication transformations match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Aggregate manual pump metric snapshots emitted by declarative UITests.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
from pathlib import Path
from typing import Any, Dict, List, Tuple


def _relative_path(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def _load_file(path: Path) -> Tuple[List[Dict[str, Any]], List[str]]:
    entries: List[Dict[str, Any]] = []
    errors: List[str] = []
    if not path.is_file():
        return entries, errors
    with path.open("r", encoding="utf-8") as handle:
        for idx, line in enumerate(handle, start=1):
            data = line.strip()
            if not data:
                continue
            try:
                parsed = json.loads(data)
            except json.JSONDecodeError as exc:
                errors.append(f"{path}:{idx}: {exc}")
                continue
            parsed["_source"] = str(path)
            parsed["_line"] = idx
            entries.append(parsed)
    return entries, errors


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--log-root",
        required=True,
        help="Root directory containing test logs/artifacts.",
    )
    parser.add_argument(
        "--output",
        help="Destination JSON file for the aggregated summary. Defaults to stdout.",
    )
    parser.add_argument(
        "--fail-on-error",
        action="store_true",
        help="Exit with a non-zero status when malformed JSON entries are found.",
    )
    args = parser.parse_args()

    root = Path(args.log_root).resolve()
    files = sorted(root.rglob("manual_pump_metrics.jsonl"))

    entries: List[Dict[str, Any]] = []
    errors: List[str] = []
    for file_path in files:
        file_entries, file_errors = _load_file(file_path)
        for entry in file_entries:
            entry["_source_relative"] = _relative_path(Path(entry["_source"]), root)
        entries.extend(file_entries)
        errors.extend(file_errors)

    summary: Dict[str, Any] = {
        "generated_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "log_root": str(root),
        "file_count": len(files),
        "entry_count": len(entries),
        "entries": entries,
        "errors": errors,
    }

    output_data = json.dumps(summary, indent=2)
    if args.output:
        Path(args.output).write_text(output_data, encoding="utf-8")
    else:
        print(output_data)

    if args.fail_on_error and errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

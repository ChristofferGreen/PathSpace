#!/usr/bin/env python3
"""Collect history CLI roundtrip telemetry artifacts for dashboards/inspector.

This helper scans the test artifact tree for `history_cli_roundtrip/telemetry.json`
files, aggregates their contents, and emits a consolidated JSON report that
CI dashboards or the inspector backend can consume. Each entry exposes the
original/roundtrip/import metrics along with relative paths to the archived
PSHD bundles so downstream tooling can link directly to the artifacts.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


ISO_Z_SUFFIX = "Z"


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _parse_iso(value: str) -> Optional[datetime]:
    if not value:
        return None
    candidate = value
    if candidate.endswith(ISO_Z_SUFFIX):
        candidate = candidate[:-1] + "+00:00"
    try:
        return datetime.fromisoformat(candidate)
    except ValueError:
        return None


def _read_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as infile:
        return json.load(infile)


def _relativize(path: Path, base: Optional[Path]) -> str:
    if base is None:
        return str(path)
    try:
        return str(path.relative_to(base))
    except ValueError:
        return str(path)


def _ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def _series(entries: Iterable["TelemetryEntry"], field: str) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for entry in entries:
        row: Dict[str, Any] = {"timestampIso": entry.timestamp_iso}
        if field in entry.original:
            row["original"] = entry.original[field]
        if field in entry.roundtrip:
            row["roundtrip"] = entry.roundtrip[field]
        if field in entry.import_stats:
            row["import"] = entry.import_stats[field]
        rows.append(row)
    return rows


def _string_series(entries: Iterable["TelemetryEntry"], field: str) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for entry in entries:
        row: Dict[str, Any] = {"timestampIso": entry.timestamp_iso}
        original = entry.original.get(field)
        roundtrip = entry.roundtrip.get(field)
        if original is not None:
            row["original"] = str(original)
        if roundtrip is not None:
            row["roundtrip"] = str(roundtrip)
        rows.append(row)
    return rows


@dataclass
class TelemetryEntry:
    timestamp_iso: str
    timestamp: datetime
    artifact_dir: str
    files: Dict[str, str]
    original: Dict[str, Any]
    roundtrip: Dict[str, Any]
    import_stats: Dict[str, Any]
    raw: Dict[str, Any]

    @classmethod
    def from_file(
        cls,
        telemetry_path: Path,
        *,
        relative_base: Optional[Path],
    ) -> "TelemetryEntry":
        payload = _read_json(telemetry_path)
        timestamp_iso = str(payload.get("timestampIso", ""))
        parsed = _parse_iso(timestamp_iso) or datetime.fromtimestamp(
            telemetry_path.stat().st_mtime, timezone.utc
        )

        archive_dir = telemetry_path.parent
        artifact_dir = _relativize(archive_dir, relative_base)

        def _ensure_map(value: Any) -> Dict[str, Any]:
            if isinstance(value, dict):
                return value
            return {}

        files: Dict[str, str] = {
            "telemetry": _relativize(telemetry_path, relative_base),
        }
        for name in ("original", "roundtrip"):
            candidate = archive_dir / f"{name}.pshd"
            if candidate.exists():
                files[name] = _relativize(candidate, relative_base)

        return cls(
            timestamp_iso=timestamp_iso or parsed.isoformat(),
            timestamp=parsed,
            artifact_dir=artifact_dir,
            files=files,
            original=_ensure_map(payload.get("original")),
            roundtrip=_ensure_map(payload.get("roundtrip")),
            import_stats=_ensure_map(payload.get("import")),
            raw=payload,
        )


def collect_entries(artifacts_root: Path, relative_base: Optional[Path]) -> List[TelemetryEntry]:
    entries: List[TelemetryEntry] = []
    for telemetry_path in artifacts_root.rglob("telemetry.json"):
        archive_dir = telemetry_path.parent
        if not (
            archive_dir.name == "history_cli_roundtrip"
            or (archive_dir / "original.pshd").exists()
            or (archive_dir / "roundtrip.pshd").exists()
        ):
            continue
        try:
            entry = TelemetryEntry.from_file(telemetry_path, relative_base=relative_base)
        except json.JSONDecodeError:
            continue
        entries.append(entry)
    entries.sort(key=lambda item: item.timestamp, reverse=True)
    return entries


def build_report(
    entries: List[TelemetryEntry],
    *,
    artifacts_root: Path,
    relative_base: Optional[Path],
    max_runs: Optional[int],
) -> Dict[str, Any]:
    limited = entries if max_runs is None else entries[:max_runs]
    chronological = list(reversed(limited))

    runs: List[Dict[str, Any]] = []
    for entry in limited:
        runs.append(
            {
                "timestampIso": entry.timestamp_iso,
                "artifactDir": entry.artifact_dir,
                "files": entry.files,
                "original": entry.original,
                "roundtrip": entry.roundtrip,
                "import": entry.import_stats,
            }
        )

    report: Dict[str, Any] = {
        "generatedAt": _now_iso(),
        "artifactsRoot": _relativize(artifacts_root, relative_base),
        "runCount": len(runs),
        "runs": runs,
        "series": {
            "undoCount": _series(chronological, "undoCount"),
            "redoCount": _series(chronological, "redoCount"),
            "diskEntries": _series(chronological, "diskEntries"),
            "diskBytes": _series(chronological, "diskBytes"),
            "hashFnv1a64": _string_series(chronological, "hashFnv1a64"),
        },
    }

    if limited:
        report["latest"] = runs[0]

    return report


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Aggregate history CLI telemetry")
    parser.add_argument(
        "--artifacts-root",
        default="build/test-logs",
        help="Directory to scan for test artifacts",
    )
    parser.add_argument(
        "--output",
        default="build/test-logs/history_cli_roundtrip/index.json",
        help="Where to write the aggregated JSON report (use '-' for stdout)",
    )
    parser.add_argument(
        "--relative-base",
        default=None,
        help="Optional base path for relative file entries (defaults to artifacts root)",
    )
    parser.add_argument(
        "--max-runs",
        type=int,
        default=200,
        help="Limit the number of runs included in the report (default: 200)",
    )
    parser.add_argument(
        "--fail-when-missing",
        action="store_true",
        help="Return non-zero when no telemetry entries are found",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress status output",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)

    artifacts_root = Path(args.artifacts_root).resolve()
    if not artifacts_root.exists():
        if args.fail_when_missing:
            return 1
        if not args.quiet:
            print(f"[history] artifacts root not found: {artifacts_root}", file=sys.stderr)
        return 0

    relative_base = (
        Path(args.relative_base).resolve() if args.relative_base else artifacts_root
    )

    entries = collect_entries(artifacts_root, relative_base)
    if not entries and not args.quiet:
        print("[history] no telemetry entries located", file=sys.stderr)
    if not entries and args.fail_when_missing:
        return 1

    report = build_report(
        entries,
        artifacts_root=artifacts_root,
        relative_base=relative_base,
        max_runs=args.max_runs,
    )

    if args.output == "-":
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        print()
    else:
        output_path = Path(args.output)
        _ensure_parent(output_path)
        with output_path.open("w", encoding="utf-8") as outfile:
            json.dump(report, outfile, indent=2, sort_keys=True)
            outfile.write("\n")
        if not args.quiet:
            print(f"[history] wrote {output_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

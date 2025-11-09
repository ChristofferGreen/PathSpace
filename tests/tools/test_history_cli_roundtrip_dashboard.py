#!/usr/bin/env python3

"""Regression test for the history CLI telemetry dashboard generator."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict


def _write_telemetry(path: Path) -> None:
    payload: Dict[str, Any] = {
        "timestampIso": datetime(2025, 11, 7, 12, 30, tzinfo=timezone.utc).isoformat(),
        "original": {
            "undoCount": 4,
            "redoCount": 1,
            "diskEntries": 5,
            "diskBytes": 32768,
            "hashFnv1a64": "0123456789abcdef",
        },
        "roundtrip": {
            "undoCount": 4,
            "redoCount": 1,
            "diskEntries": 5,
            "diskBytes": 32768,
        },
        "import": {
            "undoCount": 4,
            "redoCount": 1,
            "diskEntries": 5,
            "diskBytes": 32768,
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


def run_test(repo_root: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="history-dashboard-") as tmp_dir_str:
        tmp_dir = Path(tmp_dir_str)
        artifacts_root = tmp_dir / "test-logs"
        archive_dir = (
            artifacts_root
            / "HistorySavefileCLIRoundTrip_sample_20251107-000000.artifacts"
            / "history_cli_roundtrip"
        )
        telemetry_path = archive_dir / "telemetry.json"
        _write_telemetry(telemetry_path)
        for name in ("original", "roundtrip"):
            (archive_dir / f"{name}.psjl").write_bytes(b"psjl")

        output_dir = artifacts_root / "history_cli_roundtrip"
        index_path = output_dir / "index.json"
        html_path = output_dir / "dashboard.html"

        cmd = [
            sys.executable,
            str(repo_root / "scripts/history_cli_roundtrip_ingest.py"),
            "--artifacts-root",
            str(artifacts_root),
            "--relative-base",
            str(tmp_dir),
            "--output",
            str(index_path),
            "--html-output",
            str(html_path),
            "--max-runs",
            "5",
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise SystemExit(
                f"history_cli_roundtrip_ingest.py failed with code {result.returncode}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

        if not index_path.exists():
            raise SystemExit(f"index.json not generated at {index_path}")
        if not html_path.exists():
            raise SystemExit(f"dashboard.html not generated at {html_path}")

        report = json.loads(index_path.read_text(encoding="utf-8"))
        if report.get("runCount") != 1:
            raise SystemExit(f"unexpected runCount: {report.get('runCount')}")
        runs = report.get("runs", [])
        expected_path = os.path.normpath(
            "test-logs/HistorySavefileCLIRoundTrip_sample_20251107-000000.artifacts/history_cli_roundtrip/original.psjl"
        )
        if not runs or runs[0]["files"]["original"] != expected_path:
            raise SystemExit("original.psjl link missing from report")

        html_content = html_path.read_text(encoding="utf-8")
        if "PathSpace Undo History Dashboard" not in html_content:
            raise SystemExit("dashboard title missing from HTML output")
        if "Original PSJL" not in html_content:
            raise SystemExit("Original PSJL link label missing from HTML output")
        if "../HistorySavefileCLIRoundTrip_sample_20251107-000000.artifacts/history_cli_roundtrip/original.psjl" not in html_content:
            raise SystemExit("original.psjl relative link missing from HTML output")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Path to the repository root",
    )
    args = parser.parse_args()
    run_test(args.repo_root.resolve())


if __name__ == "__main__":
    main()

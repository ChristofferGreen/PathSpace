#!/usr/bin/env python3

"""Regression test for the paint_example diagnostics aggregator."""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


def _write_snapshot(path: Path, tag: str, status: str, timestamp_ns: int) -> None:
    payload = {
        "schema_version": 1,
        "manifest": {
            "manifest_revision": 3,
            "tag": tag,
            "sha256": "abc",
            "width": 1280,
            "height": 800,
            "renderer": "metal_present",
        },
        "run": {
            "timestamp_ns": timestamp_ns,
            "status": status,
            "hardware_capture": True,
            "mean_error": 0.0,
            "max_channel_delta": 0,
            "screenshot_path": f"/tmp/{tag}.png",
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


def run_test(repo_root: Path) -> None:
    aggregator = repo_root / "scripts" / "paint_example_diagnostics_ingest.py"
    with tempfile.TemporaryDirectory(prefix="paint-diag-") as tmp_dir:
        tmp_path = Path(tmp_dir)
        snap_a = tmp_path / "a_metrics.json"
        snap_b = tmp_path / "b_metrics.json"
        _write_snapshot(snap_a, "1280x800", "match", 2000)
        _write_snapshot(snap_b, "paint_720", "mismatch", 1000)
        report_json = tmp_path / "report" / "index.json"
        report_html = tmp_path / "report" / "index.html"
        cmd = [
            "python3",
            str(aggregator),
            "--inputs",
            str(snap_a),
            str(snap_b),
            "--output-json",
            str(report_json),
            "--output-html",
            str(report_html),
            "--max-runs",
            "5",
            "--repo-root",
            str(repo_root),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise SystemExit(
                f"paint_example_diagnostics_ingest.py failed with code {result.returncode}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )
        report = json.loads(report_json.read_text(encoding="utf-8"))
        if report.get("runCount") != 2:
            raise SystemExit(f"Expected 2 runs, found {report.get('runCount')}")
        runs = report.get("runs", [])
        if runs[0].get("tag") != "1280x800" or runs[0].get("status") != "match":
            raise SystemExit("Latest run missing or out of order in report")
        if "Paint Example Screenshot Diagnostics" not in report_html.read_text(encoding="utf-8"):
            raise SystemExit("Dashboard HTML missing expected title")


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    run_test(repo_root)


if __name__ == "__main__":
    main()

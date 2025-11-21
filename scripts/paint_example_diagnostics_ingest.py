#!/usr/bin/env python3
"""Aggregate paint_example screenshot metrics into dashboard-friendly outputs."""

from __future__ import annotations

import argparse
import glob
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List


def parse_args(repo_root: Path) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Aggregate paint_example screenshot metrics into JSON/HTML dashboards."
    )
    parser.add_argument(
        "--inputs",
        nargs="+",
        required=True,
        help="Metrics JSON files or glob patterns (repeatable).",
    )
    parser.add_argument(
        "--output-json",
        type=Path,
        required=True,
        help="Path to write the aggregated JSON report.",
    )
    parser.add_argument(
        "--output-html",
        type=Path,
        help="Optional path to write an HTML dashboard summary.",
    )
    parser.add_argument(
        "--max-runs",
        type=int,
        default=20,
        help="Maximum number of most-recent runs to keep in the report.",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root,
        help="Repository root used for relative path calculations (default: %(default)s)",
    )
    return parser.parse_args()


def expand_inputs(patterns: List[str]) -> List[Path]:
    paths: List[Path] = []
    for pattern in patterns:
        matches = [Path(p) for p in glob.glob(pattern, recursive=True)]
        if not matches:
            candidate = Path(pattern)
            if candidate.exists():
                matches = [candidate]
        paths.extend(matches)
    return sorted(set(paths))


def load_snapshot(path: Path) -> Dict[str, Any] | None:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def ns_to_iso(timestamp_ns: int | None) -> str:
    if not timestamp_ns:
        return ""
    seconds = timestamp_ns / 1_000_000_000
    return datetime.fromtimestamp(seconds, tz=timezone.utc).isoformat()


def ensure_parent(path: Path) -> None:
    if not path:
        return
    path.parent.mkdir(parents=True, exist_ok=True)


def write_json(path: Path, payload: Dict[str, Any]) -> None:
    ensure_parent(path)
    path.write_text(json.dumps(payload, indent=2) + "\n")


def write_html(path: Path, runs: List[Dict[str, Any]]) -> None:
    ensure_parent(path)
    rows = []
    for run in runs:
        status_class = "ok" if run.get("ok") else "fail"
        rows.append(
            "<tr class=\"{cls}\">"
            "<td>{tag}</td><td>{rev}</td><td>{status}</td><td>{hardware}</td>"
            "<td>{mean_error}</td><td>{max_delta}</td><td>{timestamp}</td><td>{source}</td>"
            "</tr>".format(
                cls=status_class,
                tag=(run.get("tag") or ""),
                rev=(run.get("manifest_revision") or ""),
                status=run.get("status", ""),
                hardware="true" if run.get("hardware_capture") else "false",
                mean_error=run.get("mean_error", ""),
                max_delta=run.get("max_channel_delta", ""),
                timestamp=run.get("timestamp_iso", ""),
                source=run.get("source", ""),
            )
        )
    html = """<!DOCTYPE html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\" />
<title>Paint Example Screenshot Diagnostics</title>
<style>
body {{ font-family: sans-serif; margin: 2rem; }}
table {{ border-collapse: collapse; width: 100%; }}
th, td {{ border: 1px solid #ccc; padding: 0.25rem 0.5rem; text-align: left; }}
tr.ok {{ background: #f2fff2; }}
tr.fail {{ background: #fff2f2; }}
</style>
</head>
<body>
<h1>Paint Example Screenshot Diagnostics</h1>
<table>
<thead>
<tr><th>Tag</th><th>Manifest Rev</th><th>Status</th><th>Hardware</th><th>Mean Error</th><th>Max Δ</th><th>Timestamp (UTC)</th><th>Source</th></tr>
</thead>
<tbody>
{rows}
</tbody>
</table>
</body>
</html>
""".format(rows="\n".join(rows))
    path.write_text(html)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    args = parse_args(repo_root)
    input_paths = expand_inputs(args.inputs)
    if not input_paths:
        print("[paint-example-diag] No metrics inputs matched", flush=True)
        return 1

    run_records: List[Dict[str, Any]] = []
    for path in input_paths:
        snapshot = load_snapshot(path)
        if not snapshot:
            continue
        manifest = snapshot.get("manifest", {})
        run = snapshot.get("run", {})
        timestamp_ns = run.get("timestamp_ns")
        record: Dict[str, Any] = {
            "source": str(path),
            "timestamp_ns": timestamp_ns or 0,
            "timestamp_iso": ns_to_iso(timestamp_ns),
            "tag": manifest.get("tag"),
            "manifest_revision": manifest.get("manifest_revision"),
            "sha256": manifest.get("sha256"),
            "renderer": manifest.get("renderer"),
            "width": manifest.get("width"),
            "height": manifest.get("height"),
            "status": run.get("status"),
            "hardware_capture": run.get("hardware_capture"),
            "mean_error": run.get("mean_error"),
            "max_channel_delta": run.get("max_channel_delta"),
            "screenshot_path": run.get("screenshot_path"),
            "diff_path": run.get("diff_path"),
        }
        success_status = {"match", "captured"}
        record["ok"] = bool(run.get("status") in success_status)
        run_records.append(record)

    if not run_records:
        print("[paint-example-diag] No readable metrics snapshots", flush=True)
        return 1

    run_records.sort(key=lambda item: item.get("timestamp_ns", 0), reverse=True)
    max_runs = max(args.max_runs, 1)
    run_records = run_records[:max_runs]

    report = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "runCount": len(run_records),
        "runs": run_records,
    }
    output_json = args.output_json if args.output_json.is_absolute() else args.repo_root / args.output_json
    write_json(output_json, report)
    if args.output_html:
        output_html = args.output_html if args.output_html.is_absolute() else args.repo_root / args.output_html
        write_html(output_html, run_records)
    print(f"[paint-example-diag] Wrote {len(run_records)} runs to {output_json}")
    if args.output_html:
        print(f"[paint-example-diag] HTML dashboard: {output_html}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

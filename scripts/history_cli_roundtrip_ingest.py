#!/usr/bin/env python3
"""Collect history CLI roundtrip telemetry artifacts for dashboards/inspector.

This helper scans the test artifact tree for `history_cli_roundtrip/telemetry.json`
files, aggregates their contents, and emits a consolidated JSON report that
CI dashboards or the inspector backend can consume. Each entry exposes the
original/roundtrip/import metrics along with relative paths to the archived
PSJL bundles so downstream tooling can link directly to the artifacts.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from html import escape
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


def _relpath_for_html(target: Path, html_dir: Path) -> str:
    target_str = os.path.abspath(str(target))
    return os.path.relpath(target_str, str(html_dir))


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
            for suffix in (".psjl", ".pshd"):
                candidate = archive_dir / f"{name}{suffix}"
                if candidate.exists():
                    files[name] = _relativize(candidate, relative_base)
                    break

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
            or any((archive_dir / f"original{suffix}").exists() for suffix in (".psjl", ".pshd"))
            or any((archive_dir / f"roundtrip{suffix}").exists() for suffix in (".psjl", ".pshd"))
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


def _format_number_script() -> str:
    return (
        "function formatNumber(value) {"
        "if (!isFinite(value)) { return '–'; }"
        "return value.toLocaleString(undefined, {maximumFractionDigits: 2});"
        "}"
        "function formatBytes(value) {"
        "if (!isFinite(value)) { return '–'; }"
        "const units = ['B','KiB','MiB','GiB','TiB'];"
        "let idx = 0;"
        "let scaled = value;"
        "while (scaled >= 1024 && idx < units.length - 1) {"
        "scaled /= 1024;"
        "idx += 1;"
        "}"
        "if (scaled >= 100) { return scaled.toFixed(0) + ' ' + units[idx]; }"
        "if (scaled >= 10) { return scaled.toFixed(1) + ' ' + units[idx]; }"
        "return scaled.toFixed(2) + ' ' + units[idx];"
        "}"
    )


def write_html_dashboard(
    report: Dict[str, Any],
    output_path: Path,
    *,
    relative_base: Optional[Path] = None,
    title: str = "PathSpace Undo History Dashboard",
) -> None:
    html_dir = output_path.parent.resolve()
    base_root = relative_base.resolve() if relative_base is not None else html_dir

    # Prepare a data copy with link targets relative to the dashboard.
    runs: List[Dict[str, Any]] = []
    for run in report.get("runs", []):
        files = run.get("files", {})
        link_map: Dict[str, str] = {}
        for name, path_str in files.items():
            candidate = Path(path_str)
            if candidate.is_absolute():
                abs_candidate = candidate
            else:
                abs_candidate = (base_root / candidate)
            link_map[name] = _relpath_for_html(abs_candidate, html_dir)
        prepared = dict(run)
        prepared["fileLinks"] = link_map
        runs.append(prepared)

    prepared_report: Dict[str, Any] = dict(report)
    prepared_report["runs"] = runs

    data_json = json.dumps(prepared_report, ensure_ascii=True)
    generated_at = escape(str(report.get("generatedAt", "")))
    title_text = escape(title)
    number_script = _format_number_script()

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title_text}</title>
<style>
body {{
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  margin: 24px;
  color: #111;
  background: #fafafa;
}}
h1 {{
  margin-bottom: 0.25rem;
}}
.subtitle {{
  color: #555;
  margin-bottom: 2rem;
}}
.charts {{
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
  gap: 24px;
  margin-bottom: 32px;
}}
.chart-card {{
  background: #fff;
  border-radius: 8px;
  border: 1px solid #ddd;
  padding: 16px;
  box-shadow: 0 1px 2px rgba(0,0,0,0.06);
}}
.chart-card h2 {{
  font-size: 1.1rem;
  margin: 0 0 12px 0;
}}
.chart-container {{
  width: 100%;
  height: 220px;
}}
.legend {{
  display: flex;
  gap: 12px;
  margin-top: 8px;
  font-size: 0.9rem;
  flex-wrap: wrap;
}}
.legend span {{
  display: inline-flex;
  align-items: center;
  gap: 6px;
}}
.legend .swatch {{
  width: 12px;
  height: 12px;
  border-radius: 2px;
  display: inline-block;
}}
table {{
  width: 100%;
  border-collapse: collapse;
  background: #fff;
  border: 1px solid #ddd;
  box-shadow: 0 1px 2px rgba(0,0,0,0.06);
}}
th, td {{
  padding: 8px 10px;
  border-bottom: 1px solid #eee;
  text-align: left;
  font-size: 0.9rem;
  vertical-align: top;
}}
thead tr {{
  background: #f3f4f6;
}}
tbody tr:hover {{
  background: #f9fafb;
}}
.empty-state {{
  padding: 16px;
  color: #666;
}}
code {{
  font-family: Menlo, Consolas, "Liberation Mono", monospace;
  background: #f1f3f5;
  padding: 2px 4px;
  border-radius: 4px;
}}
</style>
</head>
<body>
<h1>{title_text}</h1>
<div class="subtitle">Generated at {generated_at}</div>
<div class="charts">
  <div class="chart-card">
    <h2>Undo Count</h2>
    <div id="undo-chart" class="chart-container"></div>
    <div class="legend">
      <span><span class="swatch" style="background:#1f77b4"></span>Original</span>
      <span><span class="swatch" style="background:#ff7f0e"></span>Roundtrip</span>
      <span><span class="swatch" style="background:#2ca02c"></span>Import</span>
    </div>
  </div>
  <div class="chart-card">
    <h2>Disk Bytes Retained</h2>
    <div id="disk-chart" class="chart-container"></div>
    <div class="legend">
      <span><span class="swatch" style="background:#1f77b4"></span>Original</span>
      <span><span class="swatch" style="background:#ff7f0e"></span>Roundtrip</span>
      <span><span class="swatch" style="background:#2ca02c"></span>Import</span>
    </div>
  </div>
</div>
<div id="runs-table-container"></div>
<script>
const REPORT = {data_json};
{number_script}
const SERIES_DEFS = [
  {{ key: "original", color: "#1f77b4", label: "Original" }},
  {{ key: "roundtrip", color: "#ff7f0e", label: "Roundtrip" }},
  {{ key: "import", color: "#2ca02c", label: "Import" }}
];

function parseTimestamp(value) {{
  const time = Date.parse(value);
  return Number.isNaN(time) ? null : time;
}}

function renderChart(seriesKey, containerId, formatter) {{
  const container = document.getElementById(containerId);
  if (!container) {{
    return;
  }}
  const entries = (REPORT.series && REPORT.series[seriesKey]) || [];
  container.innerHTML = "";
  if (!entries.length) {{
    const state = document.createElement("div");
    state.className = "empty-state";
    state.textContent = "No telemetry collected yet.";
    container.appendChild(state);
    return;
  }}

  const width = container.clientWidth || 640;
  const height = container.clientHeight || 220;
  const padding = 36;
  const svgNS = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(svgNS, "svg");
  svg.setAttribute("viewBox", "0 0 " + width + " " + height);
  svg.setAttribute("width", width);
  svg.setAttribute("height", height);
  container.appendChild(svg);

  const timestamps = [];
  const values = [];
  entries.forEach((entry) => {{
    const parsed = parseTimestamp(entry.timestampIso);
    if (parsed !== null) {{
      timestamps.push(parsed);
      SERIES_DEFS.forEach((def) => {{
        const val = entry[def.key];
        if (typeof val === "number" && !Number.isNaN(val)) {{
          values.push(val);
        }}
      }});
    }}
  }});

  if (!timestamps.length || !values.length) {{
    const text = document.createElementNS(svgNS, "text");
    text.setAttribute("x", padding);
    text.setAttribute("y", padding);
    text.setAttribute("fill", "#666");
    text.setAttribute("font-size", "14");
    text.textContent = "Insufficient data to chart.";
    svg.appendChild(text);
    return;
  }}

  const minT = Math.min(...timestamps);
  const maxT = Math.max(...timestamps);
  let minV = Math.min(...values);
  let maxV = Math.max(...values);
  if (minV === maxV) {{
    minV -= 1;
    maxV += 1;
  }}

  const innerWidth = width - padding * 2;
  const innerHeight = height - padding * 2;
  const mapX = (value) => {{
    if (maxT === minT) {{
      return padding + innerWidth / 2;
    }}
    return padding + ((value - minT) / (maxT - minT)) * innerWidth;
  }};
  const mapY = (value) => {{
    return padding + innerHeight - ((value - minV) / (maxV - minV)) * innerHeight;
  }};

  const axis = document.createElementNS(svgNS, "path");
  const axisPath = [
    "M",
    padding,
    padding,
    "L",
    padding,
    height - padding,
    "L",
    width - padding,
    height - padding,
  ].join(" ");
  axis.setAttribute("d", axisPath);
  axis.setAttribute("stroke", "#bbb");
  axis.setAttribute("stroke-width", "1");
  axis.setAttribute("fill", "none");
  svg.appendChild(axis);

  const label = document.createElementNS(svgNS, "text");
  label.setAttribute("x", padding);
  label.setAttribute("y", padding - 12);
  label.setAttribute("fill", "#444");
  label.setAttribute("font-size", "12");
  label.textContent = formatter(minV) + " ➝ " + formatter(maxV);
  svg.appendChild(label);

  SERIES_DEFS.forEach((def) => {{
    let pathData = "";
    let started = false;
    entries.forEach((entry) => {{
      const time = parseTimestamp(entry.timestampIso);
      const value = entry[def.key];
      if (time === null || typeof value !== "number" || Number.isNaN(value)) {{
        return;
      }}
      const x = mapX(time);
      const y = mapY(value);
      pathData += (started ? " L " : "M ") + x + " " + y;
      started = true;
    }});
    if (!started) {{
      return;
    }}
    const path = document.createElementNS(svgNS, "path");
    path.setAttribute("d", pathData);
    path.setAttribute("stroke", def.color);
    path.setAttribute("stroke-width", "2");
    path.setAttribute("fill", "none");
    svg.appendChild(path);
  }});

  const first = parseTimestamp(entries[0].timestampIso);
  const last = parseTimestamp(entries[entries.length - 1].timestampIso);
  if (first !== null && last !== null) {{
    const caption = document.createElementNS(svgNS, "text");
    caption.setAttribute("x", width - padding);
    caption.setAttribute("y", height - padding + 20);
    caption.setAttribute("fill", "#444");
    caption.setAttribute("font-size", "12");
    caption.setAttribute("text-anchor", "end");
    const rangeText = new Date(first).toLocaleString() + " → " + new Date(last).toLocaleString();
    caption.textContent = rangeText;
    svg.appendChild(caption);
  }}
}}

function buildRunsTable() {{
  const container = document.getElementById("runs-table-container");
  container.innerHTML = "";
  const runs = REPORT.runs || [];
  if (!runs.length) {{
    const state = document.createElement("div");
    state.className = "empty-state";
    state.textContent = "No CLI roundtrip runs recorded yet.";
    container.appendChild(state);
    return;
  }}

  const table = document.createElement("table");
  const thead = document.createElement("thead");
  const headerRow = document.createElement("tr");
  ["Timestamp", "Undo Count", "Redo Count", "Disk Entries", "Disk Bytes", "Hash", "Artifacts"].forEach((label) => {{
    const th = document.createElement("th");
    th.textContent = label;
    headerRow.appendChild(th);
  }});
  thead.appendChild(headerRow);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  runs.forEach((run) => {{
    const row = document.createElement("tr");
    const timestampCell = document.createElement("td");
    const runDate = parseTimestamp(run.timestampIso);
    timestampCell.textContent = runDate ? new Date(runDate).toLocaleString() : run.timestampIso || "Unknown";
    row.appendChild(timestampCell);

    const undoCell = document.createElement("td");
    undoCell.textContent = formatNumber(run.original && run.original.undoCount);
    row.appendChild(undoCell);

    const redoCell = document.createElement("td");
    redoCell.textContent = formatNumber(run.original && run.original.redoCount);
    row.appendChild(redoCell);

    const entriesCell = document.createElement("td");
    entriesCell.textContent = formatNumber(run.original && run.original.diskEntries);
    row.appendChild(entriesCell);

    const diskCell = document.createElement("td");
    diskCell.textContent = formatBytes(
      run.original && typeof run.original.diskBytes === "number" ? run.original.diskBytes : NaN
    );
    row.appendChild(diskCell);

    const hashCell = document.createElement("td");
    const hash = (run.original && run.original.hashFnv1a64) || "";
    if (hash) {{
      const code = document.createElement("code");
      code.textContent = hash;
      hashCell.appendChild(code);
    }} else {{
      hashCell.textContent = "–";
    }}
    row.appendChild(hashCell);

    const artifactsCell = document.createElement("td");
    const fileLinks = run.fileLinks || {{}};
    const linkParts = [];
    [["telemetry", "Telemetry"], ["original", "Original PSJL"], ["roundtrip", "Roundtrip PSJL"]].forEach(([key, label]) => {{
      const href = fileLinks[key];
      if (href) {{
        const link = document.createElement("a");
        link.href = href;
        link.textContent = label;
        linkParts.push(link);
      }}
    }});
    if (!linkParts.length) {{
      artifactsCell.textContent = "–";
    }} else {{
      linkParts.forEach((link, index) => {{
        artifactsCell.appendChild(link);
        if (index + 1 < linkParts.length) {{
          artifactsCell.appendChild(document.createTextNode(" · "));
        }}
      }});
    }}
    row.appendChild(artifactsCell);
    tbody.appendChild(row);
  }});

  table.appendChild(tbody);
  container.appendChild(table);
}}

function initialize() {{
  renderChart("undoCount", "undo-chart", formatNumber);
  renderChart("diskBytes", "disk-chart", formatBytes);
  buildRunsTable();
}}

window.addEventListener("DOMContentLoaded", initialize);
</script>
</body>
</html>
"""

    _ensure_parent(output_path)
    with output_path.open("w", encoding="utf-8") as outfile:
        outfile.write(html)


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
        "--html-output",
        default=None,
        help="Optional path to write a static HTML dashboard summarizing the telemetry",
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

    if args.html_output:
        html_path = Path(args.html_output)
        write_html_dashboard(report, html_path, relative_base=relative_base)
        if not args.quiet:
            print(f"[history] wrote {html_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3

"""
Capture comparative PathRenderer2D benchmark traces for small and fullscreen canvases.

This script runs the release benchmark binary, parses its stdout, and writes a JSON
summary so we can track incremental vs. full-repaint performance over time.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


CANVAS_PATTERN = re.compile(
    r"Canvas:\s*(\d+)x(\d+)\s+progressive tiles=(\d+)\s+initial tile size=(\d+)px"
)
SMALL_SURFACE_PATTERN = re.compile(r"Small-surface tiles:\s*(\d+)\s*\(count=(\d+)\)")
KV_PATTERN = re.compile(r"([A-Za-z_]+)=([^\s]+)")


def parse_numeric(value: str) -> Tuple[Optional[float], Optional[str]]:
    cleaned = value.rstrip(",")
    match = re.match(r"([-+]?\d+(?:\.\d+)?)([A-Za-z%]+)?", cleaned)
    if not match:
        return None, None
    number = float(match.group(1))
    unit = match.group(2)
    if unit and unit.lower() in {"mb", "gb"}:
        multiplier = 1.0
        if unit.lower() == "mb":
            multiplier = 1024.0 * 1024.0
        elif unit.lower() == "gb":
            multiplier = 1024.0 * 1024.0 * 1024.0
        return number * multiplier, "bytes"
    return (number, unit)


def parse_metrics_line(line: str) -> Dict[str, Dict[str, Any]]:
    metrics: Dict[str, Dict[str, Any]] = {}
    for key, value in KV_PATTERN.findall(line):
        numeric, unit = parse_numeric(value)
        entry: Dict[str, Any] = {"raw": value.rstrip(",")}
        if numeric is not None:
            entry["value"] = numeric
            if unit:
                entry["unit"] = unit
        metrics[key] = entry
    return metrics


def run_benchmark(executable: Path, canvas: str, enable_metrics: bool) -> Dict[str, Any]:
    args = [str(executable), f"--canvas={canvas}"]
    if enable_metrics:
        args.append("--metrics")

    result = subprocess.run(args, check=True, capture_output=True, text=True)
    stdout = result.stdout.splitlines()

    canvas_info: Dict[str, Any] = {}
    full_repaint: Dict[str, Dict[str, Any]] = {}
    incremental: Dict[str, Dict[str, Any]] = {}
    small_surface: Optional[Dict[str, Any]] = None

    for line in stdout:
        line = line.strip()
        if not line:
            continue
        canvas_match = CANVAS_PATTERN.match(line)
        if canvas_match:
            width, height, tiles, tile_px = canvas_match.groups()
            canvas_info = {
                "width_px": int(width),
                "height_px": int(height),
                "progressive_tiles": int(tiles),
                "initial_tile_size_px": int(tile_px),
            }
            continue
        if line.startswith("Full repaint stats:"):
            full_repaint = parse_metrics_line(line)
            continue
        if line.startswith("Incremental stroke stats:"):
            incremental = parse_metrics_line(line)
            continue
        small_surface_match = SMALL_SURFACE_PATTERN.match(line)
        if small_surface_match:
            tiles, count = small_surface_match.groups()
            small_surface = {
                "tiles": int(tiles),
                "sample_count": int(count),
            }

    if not canvas_info:
        raise RuntimeError(f"Failed to parse canvas line from benchmark output:\n{result.stdout}")

    return {
        "canvas": canvas,
        "canvas_info": canvas_info,
        "full_repaint": full_repaint,
        "incremental_stroke": incremental,
        "small_surface": small_surface,
        "raw_output": stdout,
    }


def capture_traces(
    executable: Path,
    canvases: Iterable[str],
    enable_metrics: bool,
) -> Dict[str, Any]:
    samples: List[Dict[str, Any]] = []
    for canvas in canvases:
        samples.append(run_benchmark(executable, canvas, enable_metrics))
    return {
        "captured_at_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "benchmark": str(executable),
        "metrics_enabled": enable_metrics,
        "samples": samples,
    }


def default_output_path(repo_root: Path) -> Path:
    return repo_root / "docs" / "perf" / "renderer_fps_traces.json"


def find_repo_root(start: Path) -> Path:
    current = start
    while current != current.parent:
        if (current / ".git").exists():
            return current
        current = current.parent
    raise RuntimeError("Unable to locate repository root from {}".format(start))


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="Path to the CMake build directory (default: %(default)s)",
    )
    parser.add_argument(
        "--benchmark",
        type=Path,
        default=None,
        help="Path to the path_renderer2d_benchmark executable "
        "(default: <build-dir>/benchmarks/path_renderer2d_benchmark)",
    )
    parser.add_argument(
        "--canvas",
        action="append",
        help="Canvas resolution(s) to sample (e.g. 1280x720). "
        "Defaults to two runs: 1280x720 and 3840x2160.",
    )
    parser.add_argument(
        "--metrics",
        action="store_true",
        help="Enable benchmark damage metrics via --metrics.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Destination JSON file (default: docs/perf/renderer_fps_traces.json).",
    )
    parser.add_argument(
        "--pretty",
        action="store_true",
        help="Pretty-print JSON output.",
    )

    args = parser.parse_args(argv)

    canvases = args.canvas if args.canvas else ["1280x720", "3840x2160"]

    repo_root = find_repo_root(Path.cwd())
    build_dir = args.build_dir if args.build_dir.is_absolute() else (repo_root / args.build_dir)
    if args.benchmark is not None:
        benchmark_path = args.benchmark if args.benchmark.is_absolute() else (repo_root / args.benchmark)
    else:
        benchmark_path = build_dir / "benchmarks" / "path_renderer2d_benchmark"

    if not benchmark_path.exists():
        raise FileNotFoundError(f"Benchmark executable not found: {benchmark_path}")

    traces = capture_traces(benchmark_path, canvases, args.metrics)

    output_path = args.output
    if output_path is None:
        output_path = default_output_path(repo_root)
    elif not output_path.is_absolute():
        output_path = repo_root / output_path

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(traces, fh, indent=2 if args.pretty else None)
        if not args.pretty:
            fh.write("\n")

    print(f"Wrote renderer FPS traces to {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

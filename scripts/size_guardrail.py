#!/usr/bin/env python3
"""Binary size guardrail helper for PathSpace examples.

This helper collects the sizes of key demo/example binaries after a build,
generates a human-readable report, and optionally checks the sizes against an
on-disk baseline (with configurable growth tolerances). It is intended to be
invoked from scripts/compile.sh via the --size-report / --size-write-baseline
flags but can be used directly as well.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Iterable, List, Optional


DEFAULT_TOLERANCE_PERCENT = 5.0
DEFAULT_TOLERANCE_ABS_BYTES = 262_144  # 256 KiB slack per binary


@dataclass(frozen=True)
class TargetBinary:
    name: str
    relative_path: str


TARGET_BINARIES: List[TargetBinary] = [
    TargetBinary("devices_example", "examples/devices_example"),
    TargetBinary("html_replay_example", "examples/html_replay_example"),
    TargetBinary("paint_example", "examples/paint_example"),
    TargetBinary("pixel_noise_example", "examples/pixel_noise_example"),
    TargetBinary("widgets_example", "examples/widgets_example"),
]


def _candidate_paths(build_dir: str, relative_path: str) -> Iterable[str]:
    """Yield plausible filesystem locations for a built binary."""

    sanitized = relative_path.strip("/\\")
    base = os.path.basename(sanitized)
    name_variants = []
    for variant in (sanitized, base):
        if variant and variant not in name_variants:
            name_variants.append(variant)
    if base:
        for prefix in ("examples", "bin"):
            candidate = os.path.join(prefix, base)
            if candidate not in name_variants:
                name_variants.append(candidate)

    if not name_variants:
        return []

    configs = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
    win_suffixes = [".exe"] if os.name == "nt" else []
    seen = set()
    paths: List[str] = []

    def consider(path: str) -> None:
        if path and path not in seen:
            seen.add(path)
            paths.append(path)

    for variant in name_variants:
        consider(os.path.join(build_dir, variant))
        if sys.platform == "darwin" and base:
            consider(
                os.path.join(build_dir, f"{variant}.app", "Contents", "MacOS", base)
            )
        for config in configs:
            consider(os.path.join(build_dir, config, variant))
            if sys.platform == "darwin" and base:
                consider(
                    os.path.join(
                        build_dir,
                        config,
                        f"{variant}.app",
                        "Contents",
                        "MacOS",
                        base,
                    )
                )
        for suffix in win_suffixes:
            consider(os.path.join(build_dir, variant + suffix))
            for config in configs:
                consider(os.path.join(build_dir, config, variant + suffix))

    return paths


def locate_binary(build_dir: str, target: TargetBinary) -> Optional[str]:
    for candidate in _candidate_paths(build_dir, target.relative_path):
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def collect_sizes(build_dir: str) -> List[dict]:
    results: List[dict] = []
    for target in TARGET_BINARIES:
        resolved = locate_binary(build_dir, target)
        if resolved is None:
            results.append(
                {
                    "name": target.name,
                    "relativePath": target.relative_path,
                    "found": False,
                    "sizeBytes": None,
                    "resolvedPath": None,
                }
            )
        else:
            size_bytes = os.path.getsize(resolved)
            results.append(
                {
                    "name": target.name,
                    "relativePath": target.relative_path,
                    "found": True,
                    "sizeBytes": size_bytes,
                    "resolvedPath": resolved,
                }
            )
    return results


def print_table(entries: Iterable[dict]) -> None:
    rows = list(entries)
    header = ("Binary", "Size (MiB)", "Status", "Path")
    print("\n[compile] Example binary size report")
    print("[compile] " + "; ".join(header))
    for entry in rows:
        if entry["found"]:
            size_mib = entry["sizeBytes"] / (1024 * 1024)
            status = "ok"
            path = entry["resolvedPath"]
            size_display = f"{size_mib:6.2f}"
        else:
            size_display = "   n/a"
            status = "missing"
            path = "(not found)"
        print(
            f"[compile] {entry['name']:<22} {size_display}    {status:<8} {path}"
        )


def ensure_baseline(entries: Iterable[dict], baseline_path: str, tolerance_percent: float,
                    tolerance_abs: int, record: bool) -> None:
    entries_list = list(entries)
    if record:
        data = {
            "generatedAt": datetime.now(timezone.utc).isoformat(),
            "tolerance": {
                "percent": tolerance_percent,
                "absoluteBytes": tolerance_abs,
            },
            "binaries": {},
        }
        for entry in entries_list:
            if not entry["found"]:
                raise SystemExit(
                    f"cannot record baseline; binary '{entry['relativePath']}' was not built"
                )
            data["binaries"][entry["relativePath"]] = {
                "name": entry["name"],
                "sizeBytes": entry["sizeBytes"],
            }
        os.makedirs(os.path.dirname(baseline_path), exist_ok=True)
        with open(baseline_path, "w", encoding="utf-8") as outf:
            json.dump(data, outf, indent=2, sort_keys=True)
            outf.write("\n")
        print(f"[compile] Wrote size baseline to {baseline_path}")
        return

    # Checking mode.
    if not os.path.exists(baseline_path):
        raise SystemExit(
            f"size baseline not found at {baseline_path}. Run with --size-write-baseline first."
        )

    with open(baseline_path, "r", encoding="utf-8") as inf:
        baseline = json.load(inf)

    tolerance = baseline.get("tolerance", {})
    percent = float(tolerance.get("percent", tolerance_percent))
    abs_bytes = int(tolerance.get("absoluteBytes", tolerance_abs))
    recorded = baseline.get("binaries", {})

    problems: List[str] = []
    current_by_path = {entry["relativePath"]: entry for entry in entries_list}

    for rel_path, info in recorded.items():
        entry = current_by_path.get(rel_path)
        if entry is None:
            problems.append(
                f"baseline expects binary '{rel_path}', but it was not tracked in this run"
            )
            continue
        if not entry["found"]:
            problems.append(
                f"binary '{rel_path}' missing (expected size {info['sizeBytes']} bytes)"
            )
            continue

        baseline_size = int(info.get("sizeBytes", 0))
        size_bytes = entry["sizeBytes"]
        allowed_growth = max(abs_bytes, int(baseline_size * (percent / 100.0)))
        limit = baseline_size + allowed_growth
        if size_bytes > limit:
            excess = size_bytes - baseline_size
            problems.append(
                f"binary '{rel_path}' grew by {excess} bytes (limit {limit}); baseline {baseline_size}, current {size_bytes}"
            )

    # Detect new binaries being tracked automatically.
    for rel_path, entry in current_by_path.items():
        if rel_path not in recorded and entry["found"]:
            problems.append(
                f"binary '{rel_path}' is new; record a baseline before enabling guardrails"
            )

    if problems:
        for problem in problems:
            print(f"[compile] size guardrail failure: {problem}", file=sys.stderr)
        raise SystemExit(1)


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="PathSpace binary size guardrail")
    parser.add_argument("--build-dir", required=True, help="CMake build directory")
    parser.add_argument(
        "--baseline",
        help="Path to baseline JSON file (used for either recording or checking)",
    )
    parser.add_argument(
        "--tolerance-percent",
        type=float,
        default=DEFAULT_TOLERANCE_PERCENT,
        help="Growth allowed relative to baseline size (percent)",
    )
    parser.add_argument(
        "--tolerance-bytes",
        type=int,
        default=DEFAULT_TOLERANCE_ABS_BYTES,
        help="Minimum absolute bytes of growth allowed beyond baseline",
    )
    parser.add_argument(
        "--record-baseline",
        action="store_true",
        help="Record a new baseline instead of checking current sizes",
    )
    parser.add_argument(
        "--print",
        action="store_true",
        help="Print a human-readable table of binary sizes",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)

    build_dir = os.path.abspath(args.build_dir)
    if not os.path.isdir(build_dir):
        raise SystemExit(f"build directory does not exist: {build_dir}")

    entries = collect_sizes(build_dir)
    if args.print:
        print_table(entries)

    if args.baseline:
        baseline_path = os.path.abspath(args.baseline)
        ensure_baseline(
            entries,
            baseline_path,
            args.tolerance_percent,
            args.tolerance_bytes,
            record=args.record_baseline,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

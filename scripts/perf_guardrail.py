#!/usr/bin/env python3
"""Performance guardrail helper for PathSpace renderer/presenter flows.

This script runs key renderer benchmarks and examples, captures their metrics,
compares the results against a checked-in baseline, and fails when regressions
exceed the permitted tolerances. It can also refresh the baseline and append
run outputs to a local history for diagnostics.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Callable, Dict, Iterable, List, Optional, Tuple


def _default_jobs() -> int:
    return max(os.cpu_count() or 4, 1)


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _ensure_directory(path: pathlib.Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def _load_json(path: pathlib.Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as infile:
        return json.load(infile)


def _dump_json(path: pathlib.Path, payload: Dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")


def _max_tolerance(baseline: float, percent: Optional[float], absolute: Optional[float]) -> float:
    percent_val = 0.0
    if percent is not None:
        percent_val = abs(baseline) * (percent / 100.0)
    abs_val = absolute if absolute is not None else 0.0
    return max(percent_val, abs(abs_val))


def _dot_join(parts: Iterable[str]) -> str:
    return ".".join(parts)


def _flatten(prefix: List[str], payload: Dict[str, Any], dest: Dict[str, float]) -> None:
    for key, value in payload.items():
        if isinstance(value, dict):
            _flatten(prefix + [key], value, dest)
        elif isinstance(value, (int, float)):
            dest[_dot_join(prefix + [key])] = float(value)
        else:
            continue


def _relative_candidates(name: str) -> List[str]:
    base = name.strip("/\\")
    variants = [base]
    if os.sep in base:
        variants.append(os.path.basename(base))
    suffixes: List[str] = []
    if sys.platform == "win32":
        suffixes.append(".exe")
    candidates: List[str] = []
    for variant in variants:
        candidates.append(variant)
        candidates.append(os.path.join("benchmarks", variant))
        candidates.append(os.path.join("examples", variant))
        for config in ("Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
            candidates.append(os.path.join(config, variant))
            candidates.append(os.path.join(config, "benchmarks", variant))
            candidates.append(os.path.join(config, "examples", variant))
        for suffix in suffixes:
            candidates.append(variant + suffix)
            for config in ("Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
                candidates.append(os.path.join(config, variant + suffix))
    return candidates


class GuardrailError(RuntimeError):
    """Raised when the guardrail detects a regression."""


@dataclass
class ScenarioResult:
    metrics: Dict[str, float]
    metadata: Dict[str, Any]
    raw: Dict[str, Any]


@dataclass(frozen=True)
class Tolerance:
    direction: str  # "increase" (value should not grow) or "decrease" (value should not fall)
    percent: Optional[float]
    absolute: Optional[float]


@dataclass(frozen=True)
class ScenarioDefinition:
    name: str
    description: str
    runner: Callable[["PerfContext"], ScenarioResult]
    tolerances: Dict[str, Tolerance]


class PerfContext:
    def __init__(self, repo_root: pathlib.Path, build_dir: pathlib.Path, build_type: str, jobs: int,
                 verbose: bool = False) -> None:
        self.repo_root = repo_root
        self.build_dir = build_dir
        self.build_type = build_type
        self.jobs = jobs
        self.verbose = verbose

    def log(self, message: str) -> None:
        prefix = "[perf]"
        print(f"{prefix} {message}")

    def run(self, args: List[str], *, cwd: Optional[pathlib.Path] = None,
            env: Optional[Dict[str, str]] = None, allow_fail: bool = False) -> subprocess.CompletedProcess:
        if self.verbose:
            self.log(" ".join(args))
        process = subprocess.run(
            args,
            cwd=cwd or self.repo_root,
            env=env,
            check=False,
            capture_output=False,
        )
        if process.returncode != 0 and not allow_fail:
            raise RuntimeError(f"command failed ({process.returncode}): {' '.join(args)}")
        return process

    def ensure_configured(self) -> None:
        required_options = {
            "BUILD_PATHSPACE_BENCHMARKS": "ON",
            "BUILD_PATHSPACE_EXAMPLES": "ON",
            "PATHSPACE_ENABLE_UI": "ON",
            "PATHSPACE_UI_SOFTWARE": "ON",
        }
        cache_path = self.build_dir / "CMakeCache.txt"
        needs_configure = True
        missing: List[str] = []
        existing_values: Dict[str, str] = {}

        if cache_path.exists():
            with cache_path.open("r", encoding="utf-8") as cache_file:
                for line in cache_file:
                    line = line.strip()
                    if not line or line.startswith(("#", "//")) or "=" not in line:
                        continue
                    key_type, value = line.split("=", 1)
                    if ":" not in key_type:
                        continue
                    key, _type = key_type.split(":", 1)
                    existing_values[key] = value

            missing = [
                name for name, expected in required_options.items()
                if existing_values.get(name, "").upper() != expected
            ]
            current_build_type = existing_values.get("CMAKE_BUILD_TYPE", "")
            if current_build_type.upper() != self.build_type.upper():
                missing.append("CMAKE_BUILD_TYPE")

            if not missing:
                needs_configure = False

        if not needs_configure:
            return

        if missing:
            self.log(
                "Reconfiguring build to enable required options: "
                + ", ".join(sorted(missing))
            )
        else:
            self.log(f"Configuring build directory at {self.build_dir}")

        _ensure_directory(self.build_dir)
        configure_cmd = [
            "cmake",
            "-S",
            str(self.repo_root),
            "-B",
            str(self.build_dir),
            f"-DCMAKE_BUILD_TYPE={self.build_type}",
        ]

        for option, expected in required_options.items():
            configure_cmd.append(f"-D{option}={expected}")

        env_args: List[str] = []
        for env_var in ("PATHSPACE_PERF_CMAKE_ARGS", "PATHSPACE_CMAKE_ARGS"):
            value = os.environ.get(env_var, "")
            if value:
                env_args.extend(shlex.split(value))
        configure_cmd.extend(env_args)

        self.run(configure_cmd)

    def build_targets(self, targets: Iterable[str]) -> None:
        effective_targets = list(dict.fromkeys(targets))
        if not effective_targets:
            return
        args = [
            "cmake",
            "--build",
            str(self.build_dir),
            "--parallel",
            str(self.jobs),
        ]
        for target in effective_targets:
            args.extend(["--target", target])
        self.log(f"Building targets: {', '.join(effective_targets)}")
        self.run(args)

    def locate_binary(self, names: Iterable[str]) -> pathlib.Path:
        for name in names:
            for candidate in _relative_candidates(name):
                path = self.build_dir / candidate
                if path.exists() and os.access(path, os.X_OK):
                    return path
        raise FileNotFoundError(f"unable to locate binary for {', '.join(names)} (looked under {self.build_dir})")


def run_path_renderer_benchmark(ctx: PerfContext) -> ScenarioResult:
    binary = ctx.locate_binary(["path_renderer2d_benchmark"])
    ctx.log(f"Running {binary.name} (renderer benchmark)")
    with tempfile.TemporaryDirectory(prefix="perf_guardrail_") as tmp:
        report_path = pathlib.Path(tmp) / "renderer_metrics.json"
        args = [
            str(binary),
            "--canvas=3840x2160",
            "--metrics",
            f"--write-json={report_path}",
        ]
        ctx.run(args)
        report = _load_json(report_path)

    frames = report.get("frames", {})
    full = frames.get("fullRepaint", {})
    incremental = frames.get("incremental", {})
    metrics: Dict[str, float] = {}
    for prefix, payload in (("full", full), ("incremental", incremental)):
        flat: Dict[str, float] = {}
        _flatten([], payload, flat)
        for key, value in flat.items():
            metrics[f"{prefix}.{key}"] = value
    # Damage aggregates
    full_damage = full.get("damage", {})
    incremental_damage = incremental.get("damage", {})
    for key, value in full_damage.items():
        if isinstance(value, (int, float)):
            metrics[f"full.damage.{key}"] = float(value)
    for key, value in incremental_damage.items():
        if isinstance(value, (int, float)):
            metrics[f"incremental.damage.{key}"] = float(value)

    metadata = {
        "canvas": report.get("canvas"),
        "progressive": report.get("progressive"),
        "command": report.get("command"),
        "metricsEnabled": report.get("metricsEnabled"),
    }
    return ScenarioResult(metrics=metrics, metadata=metadata, raw=report)


def run_pixel_noise_example(ctx: PerfContext) -> ScenarioResult:
    binary = ctx.locate_binary(["pixel_noise_example", os.path.join("examples", "pixel_noise_example")])
    ctx.log(f"Running {binary.name} (pixel noise example)")
    with tempfile.TemporaryDirectory(prefix="perf_guardrail_noise_") as tmp:
        report_path = pathlib.Path(tmp) / "pixel_noise_metrics.json"
        args = [
            str(binary),
            "--headless",
            "--width=1280",
            "--height=720",
            "--frames=120",
            "--present-refresh=0",
            "--report-metrics",
            "--report-interval=0.5",
            "--present-call-metric",
            "--seed=123456789",
            "--budget-present-ms=20",
            "--budget-render-ms=20",
            "--min-fps=50",
            f"--write-baseline={report_path}",
        ]
        ctx.run(args)
        report = _load_json(report_path)

    metrics: Dict[str, float] = {}
    summary = report.get("summary", {})
    tile_stats = report.get("tileStats", {})
    for key, value in summary.items():
        if isinstance(value, (int, float)):
            metrics[f"summary.{key}"] = float(value)
    for key, value in tile_stats.items():
        if isinstance(value, (int, float)):
            metrics[f"tileStats.{key}"] = float(value)

    metadata = {
        "command": report.get("command"),
        "tileStats": {
            "tileSize": tile_stats.get("tileSize"),
            "backendKind": tile_stats.get("backendKind"),
        },
        "residency": report.get("residency", {}).get("overallStatus"),
    }
    return ScenarioResult(metrics=metrics, metadata=metadata, raw=report)


SCENARIOS: List[ScenarioDefinition] = [
    ScenarioDefinition(
        name="path_renderer2d",
        description="PathRenderer2D 4K brush benchmark",
        runner=run_path_renderer_benchmark,
        tolerances={
            "full.avgMs": Tolerance(direction="increase", percent=15.0, absolute=1.5),
            "full.fps": Tolerance(direction="decrease", percent=15.0, absolute=20.0),
            "incremental.avgMs": Tolerance(direction="increase", percent=20.0, absolute=0.5),
            "incremental.fps": Tolerance(direction="decrease", percent=20.0, absolute=100.0),
            "incremental.damage.averageCoverage": Tolerance(direction="increase", percent=25.0, absolute=0.05),
            "incremental.damage.averageTilesDirty": Tolerance(direction="increase", percent=35.0, absolute=25.0),
        },
    ),
    ScenarioDefinition(
        name="pixel_noise_software",
        description="Pixel noise software presenter",
        runner=run_pixel_noise_example,
        tolerances={
            "summary.averageFps": Tolerance(direction="decrease", percent=12.0, absolute=15.0),
            "summary.averageRenderMs": Tolerance(direction="increase", percent=20.0, absolute=0.4),
            "summary.averagePresentCallMs": Tolerance(direction="increase", percent=25.0, absolute=3.0),
            "tileStats.averageBytesCopied": Tolerance(direction="increase", percent=25.0, absolute=200_000.0),
        },
    ),
]


def _scenario_map() -> Dict[str, ScenarioDefinition]:
    return {scenario.name: scenario for scenario in SCENARIOS}


def build_required_targets(ctx: PerfContext, selected: Iterable[ScenarioDefinition]) -> None:
    targets: List[str] = []
    for scenario in selected:
        if scenario.name == "path_renderer2d":
            targets.append("path_renderer2d_benchmark")
        elif scenario.name == "pixel_noise_software":
            targets.append("pixel_noise_example")
    ctx.build_targets(targets)


def compare_metrics(baseline: Dict[str, Any], scenario: ScenarioDefinition,
                    result: ScenarioResult) -> List[str]:
    expected_metrics: Dict[str, float] = {
        key: float(value)
        for key, value in baseline.get("metrics", {}).items()
    }
    tolerances_raw = baseline.get("tolerances", {})

    problems: List[str] = []
    for metric_name, baseline_value in expected_metrics.items():
        if metric_name not in result.metrics:
            problems.append(f"{scenario.name}: missing metric '{metric_name}' in current run")
            continue

        actual_value = result.metrics[metric_name]
        tol_info = tolerances_raw.get(metric_name)
        if tol_info is None:
            scenario_tol = scenario.tolerances.get(metric_name)
            if scenario_tol is None:
                problems.append(f"{scenario.name}: no tolerance configured for '{metric_name}'")
                continue
            tol = scenario_tol
        else:
            direction = tol_info.get("direction")
            tol = Tolerance(
                direction=direction if direction else "increase",
                percent=tol_info.get("percent"),
                absolute=tol_info.get("absolute"),
            )

        allowed = _max_tolerance(baseline_value, tol.percent, tol.absolute)
        if tol.direction == "increase":
            limit = baseline_value + allowed
            if actual_value > limit:
                problems.append(
                    f"{scenario.name}: {metric_name} regressed "
                    f"(baseline {baseline_value:.4f}, actual {actual_value:.4f}, "
                    f"limit {limit:.4f})"
                )
        elif tol.direction == "decrease":
            limit = baseline_value - allowed
            if actual_value < limit:
                problems.append(
                    f"{scenario.name}: {metric_name} dropped "
                    f"(baseline {baseline_value:.4f}, actual {actual_value:.4f}, "
                    f"limit {limit:.4f})"
                )
        else:
            problems.append(f"{scenario.name}: unsupported tolerance direction '{tol.direction}' for {metric_name}")
    return problems


def write_history(history_dir: Optional[pathlib.Path],
                  scenario_name: str,
                  result: ScenarioResult) -> None:
    if history_dir is None:
        return
    _ensure_directory(history_dir)
    entry = {
        "timestamp": _now_iso(),
        "metrics": result.metrics,
        "metadata": result.metadata,
    }
    path = history_dir / f"{scenario_name}.jsonl"
    with path.open("a", encoding="utf-8") as outfile:
        json.dump(entry, outfile, sort_keys=True)
        outfile.write("\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="PathSpace performance guardrail")
    parser.add_argument("--build-dir", default="build",
                        help="CMake build directory (default: build)")
    parser.add_argument("--build-type", default="Release",
                        help="CMake build type (default: Release)")
    parser.add_argument("--jobs", type=int, default=_default_jobs(),
                        help="Parallel build jobs (default: cpu count)")
    parser.add_argument("--baseline", default="docs/perf/performance_baseline.json",
                        help="Baseline JSON path (default: docs/perf/performance_baseline.json)")
    parser.add_argument("--write-baseline", action="store_true",
                        help="Record a new baseline using current measurements")
    parser.add_argument("--history-dir", default=None,
                        help="Directory to append run history (JSONL files)")
    parser.add_argument("--print", action="store_true",
                        help="Print metrics for each scenario after execution")
    parser.add_argument("--scenarios", default="all",
                        help="Comma-separated list of scenarios to run (default: all)")
    parser.add_argument("--skip-build", action="store_true",
                        help="Skip building targets (assumes up-to-date binaries)")
    parser.add_argument("--verbose", action="store_true",
                        help="Echo commands before executing them")
    return parser.parse_args()


def select_scenarios(names_arg: str) -> List[ScenarioDefinition]:
    mapping = _scenario_map()
    if names_arg.strip().lower() == "all":
        return list(mapping.values())
    selected: List[ScenarioDefinition] = []
    for name in (segment.strip() for segment in names_arg.split(",")):
        if not name:
            continue
        if name not in mapping:
            raise SystemExit(f"unknown scenario '{name}'. Available: {', '.join(mapping.keys())}")
        selected.append(mapping[name])
    return selected


def main() -> int:
    args = parse_args()
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    build_dir = pathlib.Path(args.build_dir).resolve()
    history_dir = pathlib.Path(args.history_dir).resolve() if args.history_dir else None

    ctx = PerfContext(
        repo_root=repo_root,
        build_dir=build_dir,
        build_type=args.build_type,
        jobs=args.jobs,
        verbose=args.verbose,
    )

    scenarios = select_scenarios(args.scenarios)
    if not scenarios:
        ctx.log("No scenarios selected; nothing to do.")
        return 0

    ctx.ensure_configured()
    if not args.skip_build:
        build_required_targets(ctx, scenarios)

    results: Dict[str, ScenarioResult] = {}
    for scenario in scenarios:
        result = scenario.runner(ctx)
        result.metrics = {
            key: value
            for key, value in result.metrics.items()
            if key in scenario.tolerances
        }
        results[scenario.name] = result
        write_history(history_dir, scenario.name, result)
        if args.print:
            ctx.log(f"{scenario.name} metrics:")
            for key in sorted(result.metrics.keys()):
                ctx.log(f"  {key}: {result.metrics[key]:.6f}")

    baseline_path = pathlib.Path(args.baseline).resolve()
    if args.write_baseline:
        ctx.log(f"Writing new baseline to {baseline_path}")
        baseline_payload = {
            "generatedAt": _now_iso(),
            "scenarios": {},
        }
        for scenario in scenarios:
            result = results[scenario.name]
            baseline_payload["scenarios"][scenario.name] = {
                "metrics": result.metrics,
                "tolerances": {
                    metric: {
                        "direction": tol.direction,
                        "percent": tol.percent,
                        "absolute": tol.absolute,
                    }
                    for metric, tol in scenario.tolerances.items()
                },
                "metadata": result.metadata,
            }
        _ensure_directory(baseline_path.parent)
        _dump_json(baseline_path, baseline_payload)
        return 0

    if not baseline_path.exists():
        raise SystemExit(
            f"baseline not found at {baseline_path}. Run with --write-baseline to create it first."
        )

    baseline = _load_json(baseline_path)
    baseline_scenarios = baseline.get("scenarios", {})
    failures: List[str] = []

    for scenario in scenarios:
        if scenario.name not in baseline_scenarios:
            failures.append(f"scenario '{scenario.name}' missing from baseline; rerun with --write-baseline")
            continue
        scenario_baseline = baseline_scenarios[scenario.name]
        result = results[scenario.name]
        failures.extend(compare_metrics(scenario_baseline, scenario, result))

    if failures:
        ctx.log("Performance guardrail detected regressions:")
        for failure in failures:
            ctx.log(f"  {failure}")
        raise GuardrailError("\n".join(failures))

    ctx.log("Performance guardrail checks passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GuardrailError as exc:
        print(f"[perf] ERROR {exc}", file=sys.stderr)
        raise SystemExit(1)
    except KeyboardInterrupt:
        print("[perf] interrupted", file=sys.stderr)
        raise SystemExit(1)

#!/usr/bin/env python3
"""
Compare PixelNoise perf results against the recorded baseline budgets.

The script reads docs/perf/pixel_noise_baseline.json (or a custom path), runs
pixel_noise_example with the same dimensions/seed as the baseline, captures a
fresh metrics snapshot, and fails if the new run exceeds the stored frame-time
budgets or falls below the baseline's minimum FPS requirement.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional


def _repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent.parent


def _default_baseline_path() -> pathlib.Path:
    return _repo_root() / "docs" / "perf" / "pixel_noise_baseline.json"


def _default_build_dir() -> pathlib.Path:
    return _repo_root() / "build"


def _locate_binary(build_dir: pathlib.Path, override: Optional[pathlib.Path]) -> pathlib.Path:
    if override:
        return override

    candidates = [
        build_dir / "examples" / "pixel_noise_example",
        build_dir / "pixel_noise_example",
    ]
    for config in ("Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
        candidates.append(build_dir / config / "examples" / "pixel_noise_example")
        candidates.append(build_dir / config / "pixel_noise_example")
    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate

    raise FileNotFoundError(
        f"pixel_noise_example binary not found under {build_dir}; "
        "build the project or pass --binary",
    )


def _load_json(path: pathlib.Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def _format_float(value: float) -> str:
    return f"{value:.6f}"


def _build_command(binary: pathlib.Path, baseline: Dict[str, Any], output_path: pathlib.Path) -> List[str]:
    command_cfg = baseline.get("command", {})

    def _extract_float(key: str) -> Optional[float]:
        value = command_cfg.get(key)
        if value is None:
            return None
        return float(value)

    args: List[str] = [str(binary)]

    if command_cfg.get("headless", False):
        args.append("--headless")
    else:
        args.append("--windowed")

    width = int(command_cfg.get("width", 1280))
    height = int(command_cfg.get("height", 720))
    args.append(f"--width={width}")
    args.append(f"--height={height}")

    max_frames = int(command_cfg.get("maxFrames", 0))
    if max_frames > 0:
        args.append(f"--frames={max_frames}")

    present_refresh = _extract_float("presentRefreshHz")
    if present_refresh is not None:
        args.append(f"--present-refresh={_format_float(present_refresh)}")

    args.append(f"--seed={int(command_cfg.get('seed', 0))}")

    if command_cfg.get("captureFramebuffer", False):
        args.append("--capture-framebuffer")

    runtime_limit = command_cfg.get("runtimeLimitSeconds")
    if runtime_limit:
        minutes = float(runtime_limit) / 60.0
        if minutes > 0.0:
            args.append(f"--runtime-minutes={_format_float(minutes)}")

    budget_present = _extract_float("budgetPresentMs")
    if budget_present and budget_present > 0.0:
        args.append(f"--budget-present-ms={_format_float(budget_present)}")

    budget_render = _extract_float("budgetRenderMs")
    if budget_render and budget_render > 0.0:
        args.append(f"--budget-render-ms={_format_float(budget_render)}")

    min_fps = _extract_float("minFps")
    if min_fps and min_fps > 0.0:
        args.append(f"--min-fps={_format_float(min_fps)}")

    backend_kind = command_cfg.get("backendKind")
    if backend_kind:
        backend_lower = str(backend_kind).lower()
        if backend_lower in ("metal", "metal2d"):
            args.append("--backend=metal")
        elif backend_lower in ("software", "software2d"):
            args.append("--backend=software")

    args.extend((
        "--report-metrics",
        "--present-call-metric",
        "--report-interval=0.5",
        f"--write-baseline={output_path}",
    ))

    return args


def _run_command(args: List[str]) -> None:
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert process.stdout is not None
    for line in process.stdout:
        print(line, end="")
    rc = process.wait()
    if rc != 0:
        raise subprocess.CalledProcessError(rc, args)


def _compare_metrics(
    baseline: Dict[str, Any],
    fresh: Dict[str, Any],
    epsilon_ms: float = 1e-3,
    epsilon_fps: float = 1e-3,
) -> None:
    summary: Dict[str, Any] = fresh.get("summary", {})
    avg_present = float(summary.get("averagePresentMs", math.inf))
    avg_render = float(summary.get("averageRenderMs", math.inf))
    avg_fps = float(summary.get("averageFps", 0.0))

    command_cfg = baseline.get("command", {})
    budget_present = command_cfg.get("budgetPresentMs")
    budget_render = command_cfg.get("budgetRenderMs")
    min_fps = command_cfg.get("minFps")

    failures: List[str] = []

    if budget_present is not None and budget_present > 0:
        if avg_present - float(budget_present) > epsilon_ms:
            failures.append(
                f"average present {avg_present:.3f}ms exceeds budget {float(budget_present):.3f}ms"
            )

    if budget_render is not None and budget_render > 0:
        if avg_render - float(budget_render) > epsilon_ms:
            failures.append(
                f"average render {avg_render:.3f}ms exceeds budget {float(budget_render):.3f}ms"
            )

    if min_fps is not None and min_fps > 0:
        if float(min_fps) - avg_fps > epsilon_fps:
            failures.append(
                f"average fps {avg_fps:.3f} below min-fps {float(min_fps):.3f}"
            )

    if failures:
        raise RuntimeError("PixelNoise baseline comparison failed:\n  - " + "\n  - ".join(failures))


def main() -> int:
    parser = argparse.ArgumentParser(description="Check PixelNoise perf against baseline budgets.")
    parser.add_argument("--baseline", type=pathlib.Path, default=None, help="Path to baseline JSON.")
    parser.add_argument("--build-dir", type=pathlib.Path, default=None, help="Build directory.")
    parser.add_argument("--binary", type=pathlib.Path, default=None, help="Override pixel_noise_example binary.")
    parser.add_argument("--write-temp", type=pathlib.Path, default=None,
                        help="Optional path to store the fresh baseline snapshot.")
    args = parser.parse_args()

    baseline_path = args.baseline or _default_baseline_path()
    if not baseline_path.exists():
        raise FileNotFoundError(f"Baseline file not found: {baseline_path}")

    build_dir = args.build_dir or _default_build_dir()
    binary = _locate_binary(build_dir, args.binary)

    baseline = _load_json(baseline_path)
    backend_kind = (baseline.get("command") or {}).get("backendKind")
    if backend_kind and str(backend_kind).lower() in ("metal", "metal2d"):
        os.environ.setdefault("PATHSPACE_ENABLE_METAL_UPLOADS", "1")

    if args.write_temp:
        output_path = args.write_temp
        if output_path.exists():
            output_path.unlink()
        output_path.parent.mkdir(parents=True, exist_ok=True)
    else:
        temp_file = tempfile.NamedTemporaryFile(prefix="pixel-noise-run-", suffix=".json", delete=False)
        temp_file.close()
        output_path = pathlib.Path(temp_file.name)

    command = _build_command(binary, baseline, output_path)

    print("pixel_noise_baseline_check: running", " ".join(map(str, command)))
    try:
        _run_command(command)
    except subprocess.CalledProcessError as exc:
        print(f"pixel_noise_baseline_check: command failed with exit code {exc.returncode}", file=sys.stderr)
        if output_path.exists() and not args.write_temp:
            output_path.unlink(missing_ok=True)
        return exc.returncode

    fresh = _load_json(output_path)

    try:
        _compare_metrics(baseline, fresh)
    finally:
        if not args.write_temp and output_path.exists():
            output_path.unlink(missing_ok=True)

    print(
        "pixel_noise_baseline_check: budgets met "
        f"(avgPresentMs={fresh['summary']['averagePresentMs']:.3f}, "
        f"avgRenderMs={fresh['summary']['averageRenderMs']:.3f}, "
        f"avgFps={fresh['summary']['averageFps']:.3f})"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("pixel_noise_baseline_check: interrupted", file=sys.stderr)
        sys.exit(130)
    except Exception as error:  # noqa: BLE001  (we want a single consolidated failure path)
        print(f"pixel_noise_baseline_check: {error}", file=sys.stderr)
        sys.exit(1)

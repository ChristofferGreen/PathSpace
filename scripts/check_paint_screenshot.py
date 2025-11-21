#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path


def build_argument_parser(repo_root: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run paint_example in screenshot mode and verify the framebuffer matches the baseline PNG."
    )
    parser.add_argument(
        "--build-dir",
        default=repo_root / "build",
        type=Path,
        help="Path to the CMake build directory (default: %(default)s)",
    )
    parser.add_argument(
        "--baseline",
        default=repo_root / "docs" / "images" / "paint_example_baseline.png",
        type=Path,
        help="Baseline PNG that the captured screenshot must match.",
    )
    parser.add_argument(
        "--width",
        default=1280,
        type=int,
        help="Screenshot width passed to paint_example (default: %(default)s)",
    )
    parser.add_argument(
        "--height",
        default=800,
        type=int,
        help="Screenshot height passed to paint_example (default: %(default)s)",
    )
    parser.add_argument(
        "--tolerance",
        default=0.0015,
        type=float,
        help="Maximum allowed mean absolute error (0-1) before the comparison fails.",
    )
    parser.add_argument(
        "--screenshot-output",
        type=Path,
        help="Optional location to store the captured screenshot (default: build/artifacts/paint_example/screenshot.png)",
    )
    parser.add_argument(
        "--diff-output",
        type=Path,
        help="Optional location to store a visual diff (default: build/artifacts/paint_example/screenshot_diff.png)",
    )
    parser.add_argument(
        "--tag",
        type=str,
        help="Optional artifact tag appended to screenshot/diff filenames (default: <width>x<height>).",
    )
    return parser


def ensure_path_dir(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = build_argument_parser(repo_root)
    args = parser.parse_args()

    build_dir = args.build_dir
    if not build_dir.is_absolute():
        build_dir = (repo_root / build_dir).resolve()
    paint_example = build_dir / "paint_example"
    if not paint_example.exists():
        print(f"[paint-example-screenshot] Missing binary: {paint_example}", file=sys.stderr)
        return 1

    baseline = args.baseline if args.baseline.is_absolute() else (repo_root / args.baseline).resolve()
    if not baseline.exists():
        print(f"[paint-example-screenshot] Baseline PNG not found: {baseline}", file=sys.stderr)
        return 1

    artifact_dir = build_dir / "artifacts" / "paint_example"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    tag = args.tag if args.tag else f"{args.width}x{args.height}"
    sanitized_tag = tag.replace("/", "_")
    base_name = f"paint_example_{sanitized_tag}"
    screenshot_path = (
        args.screenshot_output
        if args.screenshot_output is not None
        else artifact_dir / f"{base_name}_screenshot.png"
    )
    if not screenshot_path.is_absolute():
        screenshot_path = (repo_root / screenshot_path).resolve()
    ensure_path_dir(screenshot_path)

    diff_path = (
        args.diff_output
        if args.diff_output is not None
        else artifact_dir / f"{base_name}_diff.png"
    )
    if not diff_path.is_absolute():
        diff_path = (repo_root / diff_path).resolve()
    ensure_path_dir(diff_path)

    cmd = [
        str(paint_example),
        f"--width={args.width}",
        f"--height={args.height}",
        f"--screenshot={screenshot_path}",
        f"--screenshot-compare={baseline}",
        f"--screenshot-diff={diff_path}",
        f"--screenshot-max-mean-error={args.tolerance}",
        "--screenshot-require-present",
        "--gpu-smoke",
    ]

    print(f"[paint-example-screenshot] Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=repo_root)
    if result.returncode != 0:
        print("[paint-example-screenshot] paint_example reported a failure", file=sys.stderr)
        return result.returncode

    if diff_path.exists():
        # paint_example removes the diff file when screenshots match; if it remains, surface it.
        print(f"[paint-example-screenshot] Diff written to {diff_path}")
    else:
        print("[paint-example-screenshot] Screenshot matched baseline; no diff generated.")
    print(f"[paint-example-screenshot] Screenshot written to {screenshot_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

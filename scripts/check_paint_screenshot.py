#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


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
    parser.add_argument(
        "--manifest",
        default=repo_root / "docs" / "images" / "paint_example_baselines.json",
        type=Path,
        help="Baseline manifest describing expected PNG metadata (default: %(default)s)",
    )
    return parser


def ensure_path_dir(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def compute_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8192), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as handle:
        header = handle.read(24)
    if len(header) < 24 or header[:8] != PNG_SIGNATURE or header[12:16] != b"IHDR":
        raise ValueError(f"{path} is not a valid PNG file")
    width = int.from_bytes(header[16:20], "big")
    height = int.from_bytes(header[20:24], "big")
    return width, height


def load_manifest_entry(repo_root: Path, args) -> tuple[int, dict, str]:
    manifest_path = args.manifest if args.manifest.is_absolute() else (repo_root / args.manifest).resolve()
    if not manifest_path.exists():
        print(f"[paint-example-screenshot] Manifest missing: {manifest_path}", file=sys.stderr)
        print("Run scripts/paint_example_capture.py to create or refresh the manifest.", file=sys.stderr)
        sys.exit(1)

    try:
        manifest = json.loads(manifest_path.read_text())
    except json.JSONDecodeError as exc:
        print(f"[paint-example-screenshot] Failed to parse manifest {manifest_path}: {exc}", file=sys.stderr)
        sys.exit(1)

    revision = manifest.get("manifest_revision")
    if revision is None:
        print(f"[paint-example-screenshot] Manifest {manifest_path} is missing 'manifest_revision'", file=sys.stderr)
        sys.exit(1)

    captures = manifest.get("captures", {})
    entry_key = args.tag if args.tag else f"{args.width}x{args.height}"
    entry = captures.get(entry_key)
    if entry is None:
        print(f"[paint-example-screenshot] No manifest capture entry for tag '{entry_key}'", file=sys.stderr)
        sys.exit(1)

    entry_path = Path(entry["path"])
    resolved_entry_path = entry_path if entry_path.is_absolute() else (repo_root / entry_path).resolve()
    baseline = args.baseline if args.baseline.is_absolute() else (repo_root / args.baseline).resolve()
    if resolved_entry_path != baseline:
        print(
            "[paint-example-screenshot] Baseline argument does not match manifest entry path",
            file=sys.stderr,
        )
        print(f"  manifest path : {resolved_entry_path}", file=sys.stderr)
        print(f"  argument path : {baseline}", file=sys.stderr)
        sys.exit(1)

    manifest_width = entry.get("width")
    manifest_height = entry.get("height")
    if manifest_width is None or manifest_height is None:
        print(
            f"[paint-example-screenshot] Manifest entry '{entry_key}' is missing width/height",
            file=sys.stderr,
        )
        sys.exit(1)

    if manifest_width != args.width or manifest_height != args.height:
        print(
            f"[paint-example-screenshot] Dimension mismatch for tag '{entry_key}'", file=sys.stderr
        )
        print(f"  manifest: {manifest_width}x{manifest_height}", file=sys.stderr)
        print(f"  args    : {args.width}x{args.height}", file=sys.stderr)
        sys.exit(1)

    try:
        actual_width, actual_height = read_png_dimensions(baseline)
    except ValueError as exc:
        print(f"[paint-example-screenshot] {exc}", file=sys.stderr)
        sys.exit(1)

    if (actual_width, actual_height) != (manifest_width, manifest_height):
        print(
            f"[paint-example-screenshot] Baseline {baseline} has unexpected dimensions {actual_width}x{actual_height}",
            file=sys.stderr,
        )
        print("Re-run scripts/paint_example_capture.py to refresh the file.", file=sys.stderr)
        sys.exit(1)

    recorded_sha = entry.get("sha256")
    if recorded_sha is None:
        print(
            f"[paint-example-screenshot] Manifest entry '{entry_key}' is missing sha256",
            file=sys.stderr,
        )
        sys.exit(1)

    actual_sha = compute_sha256(baseline)
    if actual_sha != recorded_sha:
        print(
            f"[paint-example-screenshot] Baseline hash mismatch for tag '{entry_key}'",
            file=sys.stderr,
        )
        print(f"  manifest sha: {recorded_sha}", file=sys.stderr)
        print(f"  actual sha  : {actual_sha}", file=sys.stderr)
        print(
            f"Run scripts/paint_example_capture.py --tags {entry_key} to update the baseline.", file=sys.stderr
        )
        sys.exit(1)

    print(
        f"[paint-example-screenshot] Using manifest revision {revision} for tag '{entry_key}' (sha256={actual_sha[:12]}...)"
    )
    return revision, entry, actual_sha


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

    manifest_revision = None
    manifest_sha = None
    if args.manifest:
        manifest_revision, _entry, manifest_sha = load_manifest_entry(repo_root, args)

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

    env = os.environ.copy()
    if manifest_revision is not None:
        env["PAINT_EXAMPLE_BASELINE_VERSION"] = str(manifest_revision)
    if manifest_sha is not None:
        env["PAINT_EXAMPLE_BASELINE_SHA256"] = manifest_sha
    env["PAINT_EXAMPLE_BASELINE_TAG"] = tag

    def run_capture(attempt: int) -> subprocess.CompletedProcess:
        print(f"[paint-example-screenshot] Running (attempt {attempt}): {' '.join(cmd)}")
        return subprocess.run(cmd, cwd=repo_root, env=env)

    result = run_capture(1)
    if result.returncode != 0:
        print("[paint-example-screenshot] paint_example reported a failure", file=sys.stderr)
        print("[paint-example-screenshot] Retrying once after 0.5s...", file=sys.stderr)
        time.sleep(0.5)
        retry = run_capture(2)
        if retry.returncode != 0:
            print("[paint-example-screenshot] paint_example reported a failure after retry", file=sys.stderr)
            return retry.returncode
        result = retry

    if diff_path.exists():
        # paint_example removes the diff file when screenshots match; if it remains, surface it.
        print(f"[paint-example-screenshot] Diff written to {diff_path}")
    else:
        print("[paint-example-screenshot] Screenshot matched baseline; no diff generated.")
    print(f"[paint-example-screenshot] Screenshot written to {screenshot_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

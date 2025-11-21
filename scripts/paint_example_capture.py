#!/usr/bin/env python3
"""Capture deterministic paint_example screenshots and refresh the baseline manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Tuple

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_png_dimensions(path: Path) -> Tuple[int, int]:
    with path.open("rb") as handle:
        header = handle.read(24)
    if len(header) < 24 or header[:8] != PNG_SIGNATURE or header[12:16] != b"IHDR":
        raise ValueError(f"{path} is not a valid PNG file")
    width = int.from_bytes(header[16:20], "big")
    height = int.from_bytes(header[20:24], "big")
    return width, height


def compute_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8192), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_argument_parser(repo_root: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Capture deterministic paint_example screenshots for baseline maintenance."
    )
    parser.add_argument(
        "--build-dir",
        default=repo_root / "build",
        type=Path,
        help="Path to the build directory containing the paint_example binary.",
    )
    parser.add_argument(
        "--manifest",
        default=repo_root / "docs" / "images" / "paint_example_baselines.json",
        type=Path,
        help="Manifest describing baseline captures (default: %(default)s)",
    )
    parser.add_argument(
        "--tags",
        nargs="*",
        help="Specific manifest capture tags to refresh (default: all captures).",
    )
    parser.add_argument(
        "--notes",
        type=str,
        help="Optional notes string recorded on each updated capture entry.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the capture commands without executing or updating files.",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Additional argument forwarded to paint_example (repeatable).",
    )
    return parser


def load_manifest(path: Path) -> Dict:
    if not path.exists():
        raise FileNotFoundError(f"Manifest not found: {path}")
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def save_manifest(path: Path, data: Dict) -> None:
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    with tmp_path.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")
    tmp_path.replace(path)


def git_commit(repo_root: Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError("git rev-parse HEAD failed")
    return result.stdout.strip()


def ensure_binary(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(f"Missing paint_example binary: {path}")


def capture_once(
    repo_root: Path,
    binary: Path,
    tag: str,
    entry: Dict,
    extra_args: list[str],
    dry_run: bool,
) -> None:
    screenshot_path = (repo_root / entry["path"]).resolve()
    screenshot_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(binary),
        f"--width={entry['width']}",
        f"--height={entry['height']}",
        f"--screenshot={screenshot_path}",
        "--screenshot-require-present",
        "--gpu-smoke",
    ]
    cmd.extend(extra_args)
    env = os.environ.copy()
    env.setdefault("PATHSPACE_ENABLE_METAL_UPLOADS", "1")
    env.setdefault("PATHSPACE_UI_METAL", "ON")
    env["PAINT_EXAMPLE_BASELINE_TAG"] = tag
    print(f"[paint-example-capture] {' '.join(cmd)}")
    if dry_run:
        return
    result = subprocess.run(cmd, cwd=repo_root, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"paint_example exited with code {result.returncode}")


def validate_dimensions(entry: Dict, path: Path) -> None:
    width, height = read_png_dimensions(path)
    if (width, height) != (entry["width"], entry["height"]):
        raise RuntimeError(
            f"Capture for {path} produced {width}x{height}, expected {entry['width']}x{entry['height']}"
        )


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = build_argument_parser(repo_root)
    args = parser.parse_args()

    build_dir = args.build_dir if args.build_dir.is_absolute() else (repo_root / args.build_dir).resolve()
    binary = build_dir / "paint_example"
    try:
        ensure_binary(binary)
    except FileNotFoundError as exc:
        print(exc, file=sys.stderr)
        print("Build the project (cmake --build build -j) before capturing baselines.", file=sys.stderr)
        return 1

    manifest_path = args.manifest if args.manifest.is_absolute() else (repo_root / args.manifest).resolve()
    try:
        manifest = load_manifest(manifest_path)
    except (FileNotFoundError, json.JSONDecodeError) as exc:
        print(f"[paint-example-capture] {exc}", file=sys.stderr)
        return 1

    captures = manifest.get("captures", {})
    if not captures:
        print("[paint-example-capture] Manifest has no capture definitions", file=sys.stderr)
        return 1

    tags = args.tags if args.tags else sorted(captures.keys())
    unknown = [tag for tag in tags if tag not in captures]
    if unknown:
        print(f"[paint-example-capture] Unknown tags: {', '.join(unknown)}", file=sys.stderr)
        return 1

    commit = git_commit(repo_root)
    updated = False
    for tag in tags:
        entry = captures[tag]
        try:
            capture_once(repo_root, binary, tag, entry, args.extra_arg, args.dry_run)
        except RuntimeError as exc:
            print(f"[paint-example-capture] {exc}", file=sys.stderr)
            return 1
        if args.dry_run:
            continue
        screenshot_path = (repo_root / entry["path"]).resolve()
        try:
            validate_dimensions(entry, screenshot_path)
        except RuntimeError as exc:
            print(f"[paint-example-capture] {exc}", file=sys.stderr)
            return 1
        sha256 = compute_sha256(screenshot_path)
        entry["sha256"] = sha256
        entry["captured_at"] = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
        entry["commit"] = commit
        entry["revision"] = int(entry.get("revision", 0)) + 1
        if args.notes:
            entry["notes"] = args.notes
        updated = True
        print(
            f"[paint-example-capture] Updated tag '{tag}' sha256={sha256[:12]}… width={entry['width']} height={entry['height']}"
        )

    if args.dry_run or not updated:
        return 0

    manifest["manifest_revision"] = int(manifest.get("manifest_revision", 0)) + 1
    save_manifest(manifest_path, manifest)
    print(f"[paint-example-capture] Manifest written to {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

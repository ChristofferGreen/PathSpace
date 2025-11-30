#!/usr/bin/env python3
"""Summarize paint_example screenshot captures and enforce manifest revision guardrails."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Optional

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


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


def sanitize_tag(tag: str) -> str:
    return tag.replace("/", "_")


def default_artifact_path(artifacts_dir: Path, tag: str, suffix: str) -> Path:
    sanitized = sanitize_tag(tag)
    return artifacts_dir / f"paint_example_{sanitized}{suffix}"


def resolve_baseline_path(manifest_path: Path, relative_path: str) -> Path:
    rel = Path(relative_path)
    if rel.is_absolute():
        return rel
    search = manifest_path.parent
    while True:
        candidate = (search / rel).resolve()
        if candidate.exists():
            return candidate
        parent = search.parent
        if parent == search:
            break
        search = parent
    return (manifest_path.parent / rel).resolve()


def load_manifest(path: Path) -> Dict:
    if not path.exists():
        raise FileNotFoundError(f"Manifest not found: {path}")
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def load_expected_revision(path: Path) -> Optional[int]:
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        return None
    return int(text, 10)


def build_parser(repo_root: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate paint_example screenshot artifacts against the manifest and emit a report."
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=repo_root / "docs" / "images" / "paint_example_baselines.json",
        help="Path to the baseline manifest JSON (default: %(default)s)",
    )
    parser.add_argument(
        "--artifacts-dir",
        type=Path,
        default=repo_root / "build" / "artifacts" / "paint_example",
        help="Directory containing screenshot artifacts from pathspace_screenshot_cli (default: %(default)s)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repo_root / "build" / "test-logs" / "paint_example" / "monitor_report.json",
        help="Where to write the aggregated JSON report (default: %(default)s)",
    )
    parser.add_argument(
        "--tags",
        nargs="*",
        help="Subset of manifest capture tags to require (default: all)",
    )
    parser.add_argument(
        "--expected-revision-file",
        type=Path,
        default=repo_root / "docs" / "images" / "paint_example_manifest_revision.txt",
        help="File containing the expected manifest revision (default: %(default)s)",
    )
    parser.add_argument(
        "--palette-log",
        type=Path,
        default=repo_root / "docs" / "images" / "paint_example_palette_log.md",
        help="Markdown log recording each baseline revision (default: %(default)s)",
    )
    return parser


def ensure_exists(path: Path, description: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"Missing {description}: {path}")


def gather_capture(
    tag: str,
    manifest_entry: Dict,
    manifest_path: Path,
    artifacts_dir: Path,
) -> Dict:
    screenshot_path = default_artifact_path(artifacts_dir, tag, "_screenshot.png")
    metrics_path = default_artifact_path(artifacts_dir, tag, "_metrics.json")
    diff_path = default_artifact_path(artifacts_dir, tag, "_diff.png")
    ensure_exists(screenshot_path, f"screenshot for tag '{tag}'")
    ensure_exists(metrics_path, f"metrics JSON for tag '{tag}'")

    width, height = read_png_dimensions(screenshot_path)
    expected_width = manifest_entry.get("width")
    expected_height = manifest_entry.get("height")
    if expected_width != width or expected_height != height:
        raise RuntimeError(
            "Screenshot size mismatch for tag '{}' (actual {}x{}, manifest {}x{})".format(
                tag, width, height, expected_width, expected_height
            )
        )

    screenshot_sha = compute_sha256(screenshot_path)
    with metrics_path.open("r", encoding="utf-8") as handle:
        metrics = json.load(handle)

    run_info = metrics.get("run", {})
    baseline_info = metrics.get("baseline", {})
    status = run_info.get("status")
    if status not in ("match", "captured"):
        raise RuntimeError(f"Metrics for tag '{tag}' report unexpected status: {status}")

    baseline_path = resolve_baseline_path(manifest_path, manifest_entry.get("path", ""))
    baseline_sha256 = manifest_entry.get("sha256")

    summary = {
        "tag": tag,
        "width": expected_width,
        "height": expected_height,
        "renderer": manifest_entry.get("renderer"),
        "baseline_path": str(baseline_path),
        "baseline_sha256": baseline_sha256,
        "manifest_notes": manifest_entry.get("notes"),
        "screenshot_path": str(screenshot_path),
        "screenshot_sha256": screenshot_sha,
        "metrics_path": str(metrics_path),
        "diff_path": str(diff_path),
        "mean_error": run_info.get("mean_error"),
        "max_channel_delta": run_info.get("max_channel_delta"),
        "hardware_capture": run_info.get("hardware_capture"),
        "status": status,
        "timestamp_ns": run_info.get("timestamp_ns"),
    }

    baseline_revision = baseline_info.get("manifest_revision")
    if baseline_revision is not None and manifest_entry.get("revision"):
        summary["metrics_manifest_revision"] = baseline_revision
    if baseline_sha256 and screenshot_sha != baseline_sha256:
        raise RuntimeError(
            "Screenshot SHA mismatch for tag '{}' (actual {}, manifest {})".format(
                tag, screenshot_sha, baseline_sha256
            )
        )
    summary["sha256_match"] = screenshot_sha == baseline_sha256
    return summary


def ensure_palette_log_entry(path: Path, revision: int) -> None:
    if revision is None:
        return
    if not path.exists():
        raise FileNotFoundError(f"Palette log not found: {path}")
    pattern = re.compile(r"^##\s+Revision\s+(\d+)")
    found = False
    with path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            match = pattern.match(line)
            if match and int(match.group(1)) == revision:
                found = True
                break
    if not found:
        raise RuntimeError(
            "Palette log {} lacks an entry for manifest revision {}".format(path, revision)
        )


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = build_parser(repo_root)
    args = parser.parse_args()

    manifest_path = args.manifest if args.manifest.is_absolute() else (repo_root / args.manifest).resolve()
    artifacts_dir = args.artifacts_dir if args.artifacts_dir.is_absolute() else (repo_root / args.artifacts_dir).resolve()
    output_path = args.output if args.output.is_absolute() else (repo_root / args.output).resolve()
    palette_log_path = args.palette_log if args.palette_log.is_absolute() else (repo_root / args.palette_log).resolve()
    expected_revision_path = (
        args.expected_revision_file
        if args.expected_revision_file.is_absolute()
        else (repo_root / args.expected_revision_file).resolve()
    )

    try:
        manifest = load_manifest(manifest_path)
    except (FileNotFoundError, json.JSONDecodeError) as exc:
        print(f"[paint-example-monitor] {exc}", file=sys.stderr)
        return 1

    manifest_revision = manifest.get("manifest_revision")
    if manifest_revision is None:
        print("[paint-example-monitor] Manifest missing 'manifest_revision'", file=sys.stderr)
        return 1

    expected_revision = None
    try:
        expected_revision = load_expected_revision(expected_revision_path)
    except ValueError as exc:
        print(f"[paint-example-monitor] Failed to parse {expected_revision_path}: {exc}", file=sys.stderr)
        return 1

    if expected_revision is not None and expected_revision != manifest_revision:
        print(
            "[paint-example-monitor] Manifest revision {} does not match expected {} ({}).".format(
                manifest_revision, expected_revision, expected_revision_path
            ),
            file=sys.stderr,
        )
        print("Update the manifest and expected revision together via scripts/paint_example_capture.py.", file=sys.stderr)
        return 1

    try:
        ensure_palette_log_entry(palette_log_path, manifest_revision)
    except (FileNotFoundError, RuntimeError) as exc:
        print(f"[paint-example-monitor] {exc}", file=sys.stderr)
        print("Update docs/images/paint_example_palette_log.md when refreshing baselines.", file=sys.stderr)
        return 1

    captures = manifest.get("captures") or {}
    if not captures:
        print("[paint-example-monitor] Manifest has no captures", file=sys.stderr)
        return 1

    required_tags = args.tags if args.tags else sorted(captures.keys())
    missing = [tag for tag in required_tags if tag not in captures]
    if missing:
        print(f"[paint-example-monitor] Unknown manifest tags requested: {', '.join(missing)}", file=sys.stderr)
        return 1

    artifacts_dir.mkdir(parents=True, exist_ok=True)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    report: Dict[str, object] = {
        "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "manifest_path": str(manifest_path),
        "manifest_revision": manifest_revision,
        "expected_revision": expected_revision,
        "artifacts_dir": str(artifacts_dir),
        "captures": {},
    }

    for tag in required_tags:
        entry = captures[tag]
        try:
            summary = gather_capture(tag, entry, manifest_path, artifacts_dir)
        except (FileNotFoundError, RuntimeError, ValueError) as exc:
            print(f"[paint-example-monitor] {exc}", file=sys.stderr)
            return 1
        report["captures"][tag] = summary

    with output_path.open("w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2)
        handle.write("\n")

    print(f"[paint-example-monitor] Wrote report for {len(required_tags)} tags to {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

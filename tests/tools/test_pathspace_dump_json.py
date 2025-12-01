#!/usr/bin/env python3

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate pathspace_dump_json demo output")
    parser.add_argument("--repo-root", required=True, help="Repository root path")
    parser.add_argument("--cli", required=True, help="pathspace_dump_json executable")
    args = parser.parse_args()

    repo_root = Path(args.repo_root)
    cli_path = Path(args.cli)

    fixture_path = repo_root / "tests" / "fixtures" / "pathspace_dump_json_demo.json"
    if not fixture_path.is_file():
        print(f"Fixture not found: {fixture_path}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as temp_dir:
        output_file = Path(temp_dir) / "demo.json"
        cmd = [
            str(cli_path),
            "--demo",
            "--root",
            "/demo",
            "--max-depth",
            "3",
            "--max-children",
            "4",
            "--max-queue-entries",
            "2",
            "--indent",
            "2",
            "--output",
            str(output_file),
        ]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode != 0:
            print("pathspace_dump_json failed:", file=sys.stderr)
            print(result.stdout, file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            return result.returncode or 1

        actual = output_file.read_text(encoding="utf-8").strip()
        expected = fixture_path.read_text(encoding="utf-8").strip()

        if actual != expected:
            print("pathspace_dump_json output did not match fixture", file=sys.stderr)
            print("--- expected", file=sys.stderr)
            print(expected, file=sys.stderr)
            print("--- actual", file=sys.stderr)
            print(actual, file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())


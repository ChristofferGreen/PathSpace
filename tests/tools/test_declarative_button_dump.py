#!/usr/bin/env python3
"""
Regression test for declarative_button_example --dump_json.

Ensures the example exits quickly and exports the expected scene nodes.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        required=True,
        help="Path to the declarative_button_example binary",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Maximum allowed runtime in seconds (default: 5)",
    )
    return parser.parse_args()


def load_json(text: str) -> dict:
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        snippet = text[:4000]
        raise AssertionError(f"JSON parse failed: {exc}\nstdout snippet:\n{snippet}")


def split_path(path: str) -> list[str]:
    return [part for part in path.split("/") if part]


def descend(tree: dict, root_path: str, target: str) -> dict:
    root_parts = split_path(root_path)
    target_parts = split_path(target)
    if len(target_parts) < len(root_parts) or target_parts[: len(root_parts)] != root_parts:
        raise AssertionError(f"Target path {target!r} is outside root {root_path!r}")

    if root_path not in tree or not isinstance(tree[root_path], dict):
        raise AssertionError(f"Root node {root_path!r} missing from export")

    node = tree[root_path]
    for part in target_parts[len(root_parts) :]:
        children = node.get("children", {})
        if not isinstance(children, dict) or part not in children:
            raise AssertionError(f"Missing child {part!r} under {target!r}")
        node = children[part]
    return node


def require_path(tree: dict, root_path: str, target: str, label: str, allow_truncated: bool = False) -> dict:
    node = descend(tree, root_path, target)
    if not allow_truncated and node.get("children_truncated"):
        raise AssertionError(f"{label} node is truncated in export")
    return node


def first_string_value(node: dict, label: str) -> str:
    values = node.get("values", [])
    for entry in values:
        value = entry.get("value")
        if isinstance(value, str):
            return value
    raise AssertionError(f"{label} missing string value")


def main() -> int:
    args = parse_args()
    binary = args.binary
    if not binary.exists():
        print(f"Binary not found: {binary}", file=sys.stderr)
        return 2

    start = time.monotonic()
    try:
        result = subprocess.run(
            [str(binary), "--dump_json"],
            capture_output=True,
            text=True,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        print(f"declarative_button_example timed out after {args.timeout:.2f}s", file=sys.stderr)
        return 1

    elapsed = time.monotonic() - start
    if result.returncode != 0:
        print(
            f"declarative_button_example exited {result.returncode} in {elapsed:.2f}s\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}",
            file=sys.stderr,
        )
        return 1

    if elapsed >= args.timeout:
        print(f"declarative_button_example took {elapsed:.2f}s (>= {args.timeout:.2f}s)", file=sys.stderr)
        return 1
    max_lines = 1200
    if len(result.stdout.splitlines()) >= max_lines:
        print(f"Dump exceeded expected line budget (<{max_lines} lines)", file=sys.stderr)
        return 1

    payload = load_json(result.stdout)
    root_path = "/system/applications/declarative_button_example"
    if root_path not in payload:
        print(f"Exporter output missing root node {root_path!r}", file=sys.stderr)
        return 1

    try:
        app_node = require_path(
        payload,
        root_path,
        "/system/applications/declarative_button_example",
        "application",
    )
        app_children = app_node.get("children", {})
        for required in ("config", "windows"):
            if required not in app_children:
                raise AssertionError(f"missing expected child {required!r} under app root")

        config_node = require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/config",
            "config",
            allow_truncated=True,
        )
        theme_children = config_node.get("children", {}).get("theme", {}).get("children", {})
        active_node = require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/config/theme/active",
            "active theme",
        )
        active_values = active_node.get("values", [])
        if not active_values or active_values[0].get("value") != "sunset":
            raise AssertionError("active theme missing sunset value")
        if "sunset" in theme_children and theme_children["sunset"].get("children"):
            raise AssertionError("theme blob was not stripped from config/theme/sunset")

        require_path(payload, root_path, "/system/applications/declarative_button_example/windows/declarative_button", "window")
        stack_node = require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/windows/declarative_button/views/main/widgets/button_column",
            "stack",
        )
        view_children = require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/windows/declarative_button/views/main",
            "view",
        ).get("children", {})
        for expected_child in ("scene", "surface", "widgets"):
            if expected_child not in view_children:
                raise AssertionError(f"view missing expected child {expected_child!r}")
        require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/windows/declarative_button/views/main/widgets/button_column/children/hello_button",
            "hello button",
        )
        require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/windows/declarative_button/views/main/widgets/button_column/children/goodbye_button",
            "goodbye button",
        )
        hello_label = require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/windows/declarative_button/views/main/widgets/button_column/children/hello_button/meta/label",
            "hello label",
        )
        if first_string_value(hello_label, "hello label") != "Say Hello":
            raise AssertionError("hello button label missing or incorrect")
        goodbye_label = require_path(
            payload,
            root_path,
            "/system/applications/declarative_button_example/windows/declarative_button/views/main/widgets/button_column/children/goodbye_button/meta/label",
            "goodbye label",
        )
        if first_string_value(goodbye_label, "goodbye label") != "Say Goodbye":
            raise AssertionError("goodbye button label missing or incorrect")
    except AssertionError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env bash
# Convenience wrapper that builds the widgets example with UI flags enabled.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

export PATHSPACE_CMAKE_ARGS="${PATHSPACE_CMAKE_ARGS:-} \
-DPATHSPACE_ENABLE_UI=ON \
-DPATHSPACE_UI_SOFTWARE=ON \
-DPATHSPACE_UI_METAL=ON \
-DBUILD_PATHSPACE_EXAMPLES=ON"

exec "${ROOT_DIR}/scripts/compile.sh" --release --target widgets_example "$@"

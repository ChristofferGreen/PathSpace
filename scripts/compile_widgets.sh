#!/usr/bin/env bash
# Convenience wrapper that builds the widgets demos with UI flags enabled.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

export PATHSPACE_CMAKE_ARGS="${PATHSPACE_CMAKE_ARGS:-} \
-DPATHSPACE_ENABLE_UI=ON \
-DPATHSPACE_UI_SOFTWARE=ON \
-DPATHSPACE_UI_METAL=ON \
-DBUILD_PATHSPACE_EXAMPLES=ON"

TARGETS=(
  widgets_example
  widgets_example_minimal
)

ARGS=("$@")

for index in "${!TARGETS[@]}"; do
  TARGET="${TARGETS[$index]}"
  FORWARD_ARGS=("${ARGS[@]}")

  if [[ "$index" -gt 0 ]]; then
    # Only perform a clean configure on the first build to avoid wiping the
    # freshly generated artifacts for subsequent targets.
    FILTERED=()
    for arg in "${FORWARD_ARGS[@]}"; do
      case "$arg" in
        -c|--clean)
          continue
          ;;
        *)
          FILTERED+=("$arg")
          ;;
      esac
    done
    FORWARD_ARGS=("${FILTERED[@]}")
  fi

  "${ROOT_DIR}/scripts/compile.sh" --release --target "$TARGET" "${FORWARD_ARGS[@]}"
done

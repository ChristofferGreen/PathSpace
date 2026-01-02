#!/usr/bin/env bash
# Convenience wrapper that builds the pixel noise example with the UI flags enabled.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

export PATHSPACE_CMAKE_ARGS="${PATHSPACE_CMAKE_ARGS:-} \
-DBUILD_PATHSPACE_EXAMPLES=ON"

exec "${ROOT_DIR}/scripts/compile.sh" --release --target pixel_noise_example "$@"

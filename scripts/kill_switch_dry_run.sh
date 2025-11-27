#!/usr/bin/env bash
# Run the mandated Release + looped test suite with the legacy widget builders
# kill switch enabled. This wrapper extends scripts/compile.sh by injecting
# -DPATHSPACE_DISABLE_LEGACY_BUILDERS=ON and defaulting to a dedicated build
# directory so the dry run never clobbers the primary build tree.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_BUILD_DIR="$ROOT_DIR/build-kill-switch"

ARGS=("--clean" "--release" "--test" "--loop=5" "--per-test-timeout" "20")
PASSTHRU=()
saw_build_dir=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -B|--build-dir)
      if [[ $# -lt 2 ]]; then
        echo "Error: Missing value for $1" >&2
        exit 1
      fi
      saw_build_dir=1
      PASSTHRU+=("$1" "$2")
      shift 2
      continue
      ;;
    --build-dir=*)
      saw_build_dir=1
      value="${1#*=}"
      PASSTHRU+=("--build-dir" "$value")
      shift 1
      continue
      ;;
    --)
      PASSTHRU+=("$@")
      break
      ;;
    *)
      PASSTHRU+=("$1")
      shift 1
      continue
      ;;
  esac
done

if [[ $saw_build_dir -eq 0 ]]; then
  PASSTHRU+=("--build-dir" "$DEFAULT_BUILD_DIR")
fi

export PATHSPACE_CMAKE_ARGS="${PATHSPACE_CMAKE_ARGS:-} -DPATHSPACE_DISABLE_LEGACY_BUILDERS=ON"
export PATHSPACE_KILL_SWITCH_DRY_RUN=1

exec "$SCRIPT_DIR/compile.sh" "${ARGS[@]}" "${PASSTHRU[@]}"

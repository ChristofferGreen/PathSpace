#!/usr/bin/env bash
# Generate the PathSpace Doxygen HTML documentation using the CMake docs target.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
JOBS=""
GENERATOR=""

usage() {
  cat <<'USAGE'
Usage: ./scripts/generate_doxygen.sh [options]

Options:
  -B, --build-dir DIR   Build directory to use (default: ./build)
  -j, --jobs N          Parallel build jobs (defaults to detected CPU count)
  -G, --generator NAME  CMake generator to use (e.g. "Ninja")
  -h, --help            Show this help and exit
USAGE
}

die() {
  echo "Error: $*" >&2
  exit 1
}

info() {
  echo "[doxygen] $*"
}

cpu_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu
  else
    echo 4
  fi
}

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "Required tool not found: $1"
}

# ----------------------------
# Parse arguments
# ----------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -B|--build-dir)
      shift || die "Missing argument for $1"
      BUILD_DIR="$1"
      ;;
    -j|--jobs)
      shift || die "Missing argument for $1"
      JOBS="$1"
      ;;
    -G|--generator)
      shift || die "Missing argument for $1"
      GENERATOR="$1"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown option: $1 (use --help for usage)"
      ;;
  esac
  shift
done

if [[ -z "$BUILD_DIR" ]]; then
  BUILD_DIR="${ROOT_DIR}/build"
fi

if [[ -z "$JOBS" ]]; then
  JOBS="$(cpu_jobs)"
else
  case "$JOBS" in
    ''|*[!0-9]*)
      die "--jobs must be a positive integer"
      ;;
  esac
  if (( JOBS < 1 )); then
    die "--jobs must be >= 1"
  fi
fi

require_tool cmake
require_tool doxygen

mkdir -p "$BUILD_DIR"

CONFIGURE_CMD=( cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DENABLE_DOXYGEN=ON )
if [[ -n "$GENERATOR" ]]; then
  CONFIGURE_CMD+=( -G "$GENERATOR" )
fi
if [[ -n "${PATHSPACE_CMAKE_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_CMAKE_ARGS=( ${PATHSPACE_CMAKE_ARGS} )
  CONFIGURE_CMD+=( "${EXTRA_CMAKE_ARGS[@]}" )
fi

info "Configuring build directory: $BUILD_DIR"
"${CONFIGURE_CMD[@]}"

BUILD_CMD=( cmake --build "$BUILD_DIR" --target docs --parallel "$JOBS" )
info "Building docs target"
"${BUILD_CMD[@]}"

INDEX_PATH="${ROOT_DIR}/docs/doxygen/html/index.html"

if [[ -f "$INDEX_PATH" ]]; then
  info "Doxygen index available at: $INDEX_PATH"
else
  die "Doxygen generation completed but index not found at $INDEX_PATH"
fi

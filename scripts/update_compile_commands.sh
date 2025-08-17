#!/usr/bin/env bash
#
# Refresh ./compile_commands.json by configuring CMake and copying from the build directory.
#
# This ensures editors/LSPs have up-to-date include paths and compile flags after files are added,
# renamed, or removed.
#
# Usage:
#   scripts/update_compile_commands.sh [options]
#
# Options:
#   -B, --build-dir DIR     Build directory (default: ./build)
#       --build-type TYPE   CMake build type (Debug, Release, RelWithDebInfo, MinSizeRel). Default: Debug
#       --debug             Shortcut for --build-type Debug
#       --release           Shortcut for --build-type Release
#   -G, --generator NAME    CMake generator (e.g., "Ninja", "Unix Makefiles")
#       --cmake-args ARGS   Extra args passed to CMake configure (quote the whole string)
#       --no-configure      Skip CMake configure if build/compile_commands.json already exists
#   -c, --clean             Remove the build directory before configuring (full refresh)
#   -v, --verbose           Print commands before running them
#   -h, --help              Show this help and exit
#
# Environment:
#   PATHSPACE_CMAKE_ARGS    Extra args passed to CMake configure (appended after --cmake-args)
#
# Examples:
#   ./scripts/update_compile_commands.sh
#   ./scripts/update_compile_commands.sh --release -G "Ninja"
#   ./scripts/update_compile_commands.sh -B out/build --cmake-args "-DENABLE_PATHIO_MACOS=ON"
#   ./scripts/update_compile_commands.sh --no-configure  # reuse existing build's compilation db
#
# Notes:
# - This script calls `cmake -S . -B <builddir>` with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON.
# - For generators that do not emit compile_commands.json at configure time (e.g., Xcode),
#   prefer "Ninja" or "Unix Makefiles".
# - The top-level CMakeLists also defines a target that copies the file after builds,
#   but this script provides an explicit, fast way to refresh it on demand.

set -euo pipefail

# ----------------------------
# Defaults
# ----------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR_DEFAULT="$ROOT_DIR/build"
BUILD_DIR=""
BUILD_TYPE="Debug"
GENERATOR=""
CMAKE_ARGS="${PATHSPACE_CMAKE_ARGS:-}"
VERBOSE=0
CLEAN=0
DO_CONFIGURE=1

# ----------------------------
# Helpers
# ----------------------------
die() { echo "[update-ccdb] Error: $*" >&2; exit 1; }
say() { echo "[update-ccdb] $*"; }
cmd() { if [[ "$VERBOSE" -eq 1 ]]; then echo "+ $*"; fi; "$@"; }
require_tool() { command -v "$1" >/dev/null 2>&1 || die "Required tool not found: $1"; }

print_help() {
  sed -n '1,120p' "$0" | sed -n '1,80p' | sed -n '1,80p' >/dev/null 2>&1 || true
  cat <<EOF
Usage: $0 [options]

Options:
  -B, --build-dir DIR     Build directory (default: $BUILD_DIR_DEFAULT)
      --build-type TYPE   CMake build type (Debug, Release, RelWithDebInfo, MinSizeRel). Default: Debug
      --debug             Shortcut for --build-type Debug
      --release           Shortcut for --build-type Release
  -G, --generator NAME    CMake generator (e.g., "Ninja", "Unix Makefiles")
      --cmake-args ARGS   Extra args passed to CMake configure (quote the whole string)
      --no-configure      Skip CMake configure if build/compile_commands.json already exists
  -c, --clean             Remove the build directory before configuring (full refresh)
  -v, --verbose           Print commands before running them
  -h, --help              Show this help and exit

Environment:
  PATHSPACE_CMAKE_ARGS    Extra args passed to CMake configure (appended after --cmake-args)

Examples:
  $0
  $0 --release -G "Ninja"
  $0 -B out/build --cmake-args "-DENABLE_PATHIO_MACOS=ON"
  $0 --no-configure
EOF
}

# ----------------------------
# Parse args
# ----------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -B|--build-dir) shift || die "Missing argument for $1"; BUILD_DIR="$1" ;;
    --build-type) shift || die "Missing argument for $1"; BUILD_TYPE="$1" ;;
    --debug) BUILD_TYPE="Debug" ;;
    --release) BUILD_TYPE="Release" ;;
    -G|--generator) shift || die "Missing argument for $1"; GENERATOR="$1" ;;
    --cmake-args) shift || die "Missing argument for $1"; CMAKE_ARGS="${CMAKE_ARGS:+$CMAKE_ARGS }$1" ;;
    --no-configure) DO_CONFIGURE=0 ;;
    -c|--clean) CLEAN=1 ;;
    -v|--verbose) VERBOSE=1 ;;
    -h|--help) print_help; exit 0 ;;
    *) die "Unknown option: $1 (use --help for usage)" ;;
  esac
  shift
done

# ----------------------------
# Validations and setup
# ----------------------------
require_tool cmake

if [[ -z "$BUILD_DIR" ]]; then
  BUILD_DIR="$BUILD_DIR_DEFAULT"
fi

CCDB_SRC="$BUILD_DIR/compile_commands.json"
CCDB_DST="$ROOT_DIR/compile_commands.json"

# Clean if requested
if [[ "$CLEAN" -eq 1 ]]; then
  if [[ -d "$BUILD_DIR" ]]; then
    say "Removing build directory: $BUILD_DIR"
    cmd rm -rf "$BUILD_DIR"
  else
    say "Clean requested but build directory does not exist: $BUILD_DIR"
  fi
fi

# Configure (unless skipped and file exists)
if [[ "$DO_CONFIGURE" -eq 1 || ! -f "$CCDB_SRC" ]]; then
  mkdir -p "$BUILD_DIR"
  CONFIGURE_CMD=( cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON )
  if [[ -n "$GENERATOR" ]]; then
    CONFIGURE_CMD+=( -G "$GENERATOR" )
  fi
  if [[ -n "$CMAKE_ARGS" ]]; then
    # shellcheck disable=SC2206
    EXTRA_ARGS=( $CMAKE_ARGS )
    CONFIGURE_CMD+=( "${EXTRA_ARGS[@]}" )
  fi

  if [[ "$VERBOSE" -eq 1 ]]; then
    echo "Configure: ${CONFIGURE_CMD[*]}"
  fi
  "${CONFIGURE_CMD[@]}"
else
  say "Skipping configure as requested (--no-configure) and $CCDB_SRC exists."
fi

# Verify compile_commands.json was generated
if [[ ! -f "$CCDB_SRC" ]]; then
  echo "[update-ccdb] Warning: $CCDB_SRC was not generated by CMake."
  echo "[update-ccdb] Hint: Use 'Ninja' or 'Unix Makefiles' generator; Xcode may not emit compile_commands.json at configure time."
  die "compile_commands.json not found in build directory"
fi

# Copy to repo root (only if different)
if cmp -s "$CCDB_SRC" "$CCDB_DST"; then
  say "compile_commands.json is already up to date at repo root."
else
  say "Updating $CCDB_DST from $CCDB_SRC"
  cmd cp -f "$CCDB_SRC" "$CCDB_DST"
fi

# Final info
SIZE=$(wc -c < "$CCDB_DST" 2>/dev/null || echo 0)
say "Done. ./compile_commands.json (${SIZE} bytes) is up to date."

exit 0

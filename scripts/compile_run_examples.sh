#!/usr/bin/env bash
#
# Compile and run the minimal PathSpace example.
#
# This helper configures CMake with -DBUILD_PATHSPACE_EXAMPLES=ON,
# builds the minimal_button_example target, and runs it.
#
# Usage:
#   scripts/compile_run_examples.sh [options]
#
# Options:
#   -c, --clean                 Remove the build directory before configuring.
#   -B, --build-dir DIR         Build directory (default: <repo>/build).
#       --build-type TYPE       CMake build type (Debug, Release, RelWithDebInfo, MinSizeRel). Default: Debug
#       --debug                 Shortcut for --build-type Debug.
#       --release               Shortcut for --build-type Release.
#   -j, --jobs N                Parallel build jobs (default: CPU count).
#   -G, --generator NAME        CMake generator (e.g., "Ninja", "Unix Makefiles", "Xcode").
#       --macos-backend         Enable macOS PathIO backends (-DENABLE_PATHIO_MACOS=ON).
#       --run-seconds N         Run the example for N seconds and then terminate it.
#   -v, --verbose               Print commands before running them.
#   -h, --help                  Show this help and exit.
#
# Examples:
#   scripts/compile_run_examples.sh
#   scripts/compile_run_examples.sh --release -j 8
#   scripts/compile_run_examples.sh --macos-backend --run-seconds 5
#   scripts/compile_run_examples.sh -G "Ninja" -B build-ninja
#
# Note:
# - The minimal_button_example runs until Ctrl-C by default. Use --run-seconds to auto-stop.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Defaults
BUILD_DIR_DEFAULT="$ROOT_DIR/build"
BUILD_DIR=""
CLEAN=0
BUILD_TYPE="Debug"
GENERATOR=""
JOBS=""
VERBOSE=0
ENABLE_MACOS_BACKEND=0
RUN_SECONDS=""
TARGET_EXE_NAME="minimal_button_example"

# Helpers
die() {
  echo "Error: $*" >&2
  exit 1
}
info() {
  echo "[examples] $*"
}
verbose() {
  if [[ "$VERBOSE" -eq 1 ]]; then
    echo "+ $*"
  fi
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
print_help() {
  sed -n '1,100p' "$0" | sed -n '1,/^set -euo pipefail/p' | sed 's/^# \{0,1\}//' | sed '1,2d'
}

# Parse args
while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--clean) CLEAN=1 ;;
    -B|--build-dir) shift || die "Missing argument for $1"; BUILD_DIR="$1" ;;
    --build-type) shift || die "Missing argument for $1"; BUILD_TYPE="$1" ;;
    --debug) BUILD_TYPE="Debug" ;;
    --release) BUILD_TYPE="Release" ;;
    -j|--jobs) shift || die "Missing argument for $1"; JOBS="$1" ;;
    -G|--generator) shift || die "Missing argument for $1"; GENERATOR="$1" ;;
    --macos-backend) ENABLE_MACOS_BACKEND=1 ;;
    --run-seconds) shift || die "Missing argument for $1"; RUN_SECONDS="$1" ;;
    -v|--verbose) VERBOSE=1 ;;
    -h|--help) print_help; exit 0 ;;
    *) die "Unknown option: $1 (use --help)" ;;
  esac
  shift
done

# Validate inputs
if [[ -z "$BUILD_DIR" ]]; then
  BUILD_DIR="$BUILD_DIR_DEFAULT"
fi
if [[ -z "$JOBS" ]]; then
  JOBS="$(cpu_jobs)"
else
  [[ "$JOBS" =~ ^[0-9]+$ ]] || die "--jobs must be a positive integer"
  [[ "$JOBS" -ge 1 ]] || die "--jobs must be >= 1"
fi
if [[ -n "$RUN_SECONDS" ]]; then
  [[ "$RUN_SECONDS" =~ ^[0-9]+$ ]] || die "--run-seconds must be a positive integer"
  [[ "$RUN_SECONDS" -ge 1 ]] || die "--run-seconds must be >= 1"
fi

command -v cmake >/dev/null 2>&1 || die "cmake not found in PATH"

# Clean if requested
if [[ "$CLEAN" -eq 1 ]]; then
  if [[ -d "$BUILD_DIR" ]]; then
    info "Removing build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
  fi
fi
mkdir -p "$BUILD_DIR"

# Configure CMake
CONFIGURE_CMD=( cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DBUILD_PATHSPACE_EXAMPLES=ON -DCMAKE_BUILD_TYPE="$BUILD_TYPE" )
# On macOS, prefer Apple clang for C++/ObjC++ so Cocoa/ObjC++ sources compile correctly.
if [[ "$(uname -s)" == "Darwin" ]]; then
  if command -v clang++ >/dev/null 2>&1; then
    CONFIGURE_CMD+=( -DCMAKE_CXX_COMPILER="$(command -v clang++)" )
  fi
  if command -v clang++ >/dev/null 2>&1; then
    CONFIGURE_CMD+=( -DCMAKE_OBJCXX_COMPILER="$(command -v clang++)" )
  fi
fi

# Prefer Ninja if generator not specified
if [[ -z "$GENERATOR" ]]; then
  if command -v ninja >/dev/null 2>&1 || command -v ninja-build >/dev/null 2>&1; then
    GENERATOR="Ninja"
  fi
fi
if [[ -n "$GENERATOR" ]]; then
  CONFIGURE_CMD+=( -G "$GENERATOR" )
fi
if [[ "$ENABLE_MACOS_BACKEND" -eq 1 ]]; then
  CONFIGURE_CMD+=( -DENABLE_PATHIO_MACOS=ON )
fi

verbose "${CONFIGURE_CMD[*]}"
"${CONFIGURE_CMD[@]}"

# Build target
BUILD_CMD=( cmake --build "$BUILD_DIR" --target "$TARGET_EXE_NAME" --parallel "$JOBS" )
# Pass --config for multi-config generators (ignored by single-config)
BUILD_CMD+=( --config "$BUILD_TYPE" )
verbose "${BUILD_CMD[*]}"
"${BUILD_CMD[@]}"

# Locate executable
exe_candidates=()
exe_candidates+=( "$BUILD_DIR/$TARGET_EXE_NAME" )
exe_candidates+=( "$BUILD_DIR/$BUILD_TYPE/$TARGET_EXE_NAME" )
exe_path=""
for p in "${exe_candidates[@]}"; do
  if [[ -x "$p" ]]; then
    exe_path="$p"
    break
  fi
done
if [[ -z "$exe_path" ]]; then
  # Fallback: scan build dir for the target name
  # Fallback: scan build dir for the target name (portable find -perm)
  exe_path="$(find "$BUILD_DIR" -type f \( -perm -u+x -o -perm -g+x -o -perm -o+x \) -name "$TARGET_EXE_NAME" -print -quit 2>/dev/null || true)"
fi
[[ -n "$exe_path" ]] || die "Could not locate built executable '$TARGET_EXE_NAME' under $BUILD_DIR"

info "Running: $exe_path"
if [[ -n "$RUN_SECONDS" ]]; then
  # Run with a timeout-like behavior (portable)
  set +e
  "$exe_path" &
  app_pid=$!
  trap 'kill -TERM "$app_pid" 2>/dev/null || true' INT TERM
  slept=0
  while kill -0 "$app_pid" 2>/dev/null; do
    if [[ "$slept" -ge "$RUN_SECONDS" ]]; then
      info "Time limit ($RUN_SECONDS s) reached; terminating example..."
      kill -TERM "$app_pid" 2>/dev/null || true
      wait "$app_pid" 2>/dev/null
      break
    fi
    sleep 1
    slept=$((slept+1))
  done
  set -e
else
  # Foreground (Ctrl-C to stop)
  "$exe_path"
fi

info "Done."

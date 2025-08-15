#!/usr/bin/env bash
#
# Simple compile helper for the PathSpace project.
#
# By default it performs an incremental build in ./build.
# Use --clean for a full rebuild (delete and reconfigure the build directory).
#
# Examples:
#   ./scripts/compile.sh
#   ./scripts/compile.sh --clean
#   ./scripts/compile.sh -j 8 --release
#   ./scripts/compile.sh --target PathSpaceTests
#   ./scripts/compile.sh -G "Ninja"
#
# Note: Compatible with macOS' default Bash (3.2).

set -euo pipefail

# ----------------------------
# Defaults
# ----------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR_DEFAULT="$ROOT_DIR/build"
CLEAN=0
BUILD_DIR=""
BUILD_TYPE="Debug"
JOBS=""
GENERATOR=""
TARGET=""
VERBOSE=0
SANITIZER=""  # "asan" | "tsan" | "usan"

# ----------------------------
# Helpers
# ----------------------------
die() {
  echo "Error: $*" >&2
  exit 1
}

info() {
  echo "[compile] $*"
}

print_help() {
  cat <<EOF
Usage: $0 [options]

Options:
  -c, --clean                Remove the build directory and do a full reconfigure.
  -B, --build-dir DIR        Build directory (default: $BUILD_DIR_DEFAULT)
      --build-type TYPE      CMake build type (Debug, Release, RelWithDebInfo, MinSizeRel).
      --debug                Shortcut for --build-type Debug
      --release              Shortcut for --build-type Release
  -j, --jobs N               Parallel build jobs (passes to cmake --build --parallel N).
  -G, --generator NAME       CMake generator (e.g., "Ninja", "Unix Makefiles").
  -t, --target NAME          Build a specific target (default: all).
  -v, --verbose              Print commands before running them.
  -h, --help                 Show this help and exit.

Sanitizers (mutually exclusive, maps to CMake options in this repo):
      --asan                 Enable AddressSanitizer      (-DENABLE_ADDRESS_SANITIZER=ON)
      --tsan                 Enable ThreadSanitizer       (-DENABLE_THREAD_SANITIZER=ON)
      --usan | --ubsan       Enable UndefinedSanitizer    (-DENABLE_UNDEFINED_SANITIZER=ON)

Examples:
  $0
  $0 --clean -j 8 --release
  $0 -G "Ninja" --target PathSpaceTests
EOF
}

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "Required tool not found: $1"
}

# ----------------------------
# Parse args
# ----------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--clean)
      CLEAN=1
      ;;
    -B|--build-dir)
      shift || die "Missing argument for $1"
      BUILD_DIR="$1"
      ;;
    --build-type)
      shift || die "Missing argument for $1"
      BUILD_TYPE="$1"
      ;;
    --debug)
      BUILD_TYPE="Debug"
      ;;
    --release)
      BUILD_TYPE="Release"
      ;;
    -j|--jobs)
      shift || die "Missing argument for $1"
      JOBS="$1"
      ;;
    -G|--generator)
      shift || die "Missing argument for $1"
      GENERATOR="$1"
      ;;
    -t|--target)
      shift || die "Missing argument for $1"
      TARGET="$1"
      ;;
    --asan)
      if [[ -n "$SANITIZER" && "$SANITIZER" != "asan" ]]; then
        die "Only one sanitizer flag may be used at a time."
      fi
      SANITIZER="asan"
      ;;
    --tsan)
      if [[ -n "$SANITIZER" && "$SANITIZER" != "tsan" ]]; then
        die "Only one sanitizer flag may be used at a time."
      fi
      SANITIZER="tsan"
      ;;
    --usan|--ubsan)
      if [[ -n "$SANITIZER" && "$SANITIZER" != "usan" ]]; then
        die "Only one sanitizer flag may be used at a time."
      fi
      SANITIZER="usan"
      ;;
    -v|--verbose)
      VERBOSE=1
      ;;
    -h|--help)
      print_help
      exit 0
      ;;
    *)
      die "Unknown option: $1 (use --help for usage)"
      ;;
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

# Jobs sanity (optional)
if [[ -n "${JOBS}" ]]; then
  case "$JOBS" in
    ''|*[!0-9]*)
      die "--jobs must be a positive integer"
      ;;
    *)
      if [[ "$JOBS" -lt 1 ]]; then
        die "--jobs must be >= 1"
      fi
      ;;
  esac
fi

# Sanitizer flags -> CMake cache options (from this project's CMakeLists.txt)
CMAKE_FLAGS=()
case "$SANITIZER" in
  "asan")
    CMAKE_FLAGS+=("-DENABLE_ADDRESS_SANITIZER=ON")
    ;;
  "tsan")
    CMAKE_FLAGS+=("-DENABLE_THREAD_SANITIZER=ON")
    ;;
  "usan")
    CMAKE_FLAGS+=("-DENABLE_UNDEFINED_SANITIZER=ON")
    ;;
  *)
    ;;
esac

# Always export compile_commands.json (redundant with project default, but safe)
CMAKE_FLAGS+=("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")

# ----------------------------
# Clean if requested
# ----------------------------
if [[ "$CLEAN" -eq 1 ]]; then
  if [[ -d "$BUILD_DIR" ]]; then
    info "Removing build directory for full rebuild: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
  else
    info "Clean requested, but build directory does not exist: $BUILD_DIR"
  fi
fi

# Ensure build directory exists
mkdir -p "$BUILD_DIR"

# ----------------------------
# Configure
# ----------------------------
CONFIGURE_CMD=( cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" )
if [[ -n "$GENERATOR" ]]; then
  CONFIGURE_CMD+=( -G "$GENERATOR" )
fi
CONFIGURE_CMD+=( "${CMAKE_FLAGS[@]}" )

if [[ "$VERBOSE" -eq 1 ]]; then
  echo "Configure: ${CONFIGURE_CMD[*]}"
fi
"${CONFIGURE_CMD[@]}"

# ----------------------------
# Build
# ----------------------------
BUILD_CMD=( cmake --build "$BUILD_DIR" )
if [[ -n "$TARGET" ]]; then
  BUILD_CMD+=( --target "$TARGET" )
fi
if [[ -n "$JOBS" ]]; then
  BUILD_CMD+=( --parallel "$JOBS" )
fi

if [[ "$VERBOSE" -eq 1 ]]; then
  echo "Build: ${BUILD_CMD[*]}"
fi
"${BUILD_CMD[@]}"

info "Build completed."
info "Build directory: $BUILD_DIR"
if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
  info "compile_commands.json generated."
fi

# Friendly pointers
if [[ -d "$BUILD_DIR/tests" ]]; then
  TEST_EXE="$BUILD_DIR/tests/PathSpaceTests"
  if [[ -x "$TEST_EXE" ]]; then
    info "Test executable: $TEST_EXE"
  fi
fi

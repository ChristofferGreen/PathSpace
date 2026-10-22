#!/usr/bin/env bash
#
# Configure, build and (optionally) test PathSpace.
#
#   ./scripts/compile.sh                      incremental Debug build in ./build
#   ./scripts/compile.sh --release --test     Release build, run the tests once
#   ./scripts/compile.sh --clean --release --test --loop=5   full pre-push check
#   ./scripts/compile.sh --tsan --test        ThreadSanitizer build in ./build-tsan
#
# Extra CMake arguments can be passed through PATHSPACE_CMAKE_ARGS.
# Compatible with macOS' default Bash (3.2).

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR=""
BUILD_TYPE="Debug"
SANITIZER=""
JOBS=""
TARGET=""
TIMEOUT=
LOOP=1
CLEAN=0
TEST=0
COVERAGE=0
DOCS=0
TIDY=0

usage() {
  cat <<EOF
Usage: $0 [options]
  -c, --clean          Delete the build directory first
  -B, --build-dir DIR  Build directory (default: build, or build-<sanitizer>)
      --debug          Debug build (default)
      --release        Release build (ASan off by default)
      --build-type T   Any CMake build type (Debug, Release, RelWithDebInfo, MinSizeRel)
  -j, --jobs N         Parallel jobs (default: all cores)
  -t, --target NAME    Build only this target
      --test           Run the tests after building
      --loop[=N]       Repeat the tests up to N times, stopping at the first failure (implies --test; default 5)
      --timeout SECS   Per-run test timeout (default: 20; 1800 with a sanitizer)
      --asan|--tsan|--ubsan  Build with a sanitizer
      --coverage       Build with coverage instrumentation
      --docs           Build the Doxygen docs (configures with -DENABLE_DOXYGEN=ON)
      --tidy           Build the library with clang-tidy (build dir: build-tidy; needs clang-tidy)
  -h, --help           Show this help
EOF
}

die() { echo "Error: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--clean) CLEAN=1 ;;
    -B|--build-dir) [[ $# -ge 2 ]] || die "$1 needs a value"; BUILD_DIR="$2"; shift ;;
    --debug) BUILD_TYPE="Debug" ;;
    --release) BUILD_TYPE="Release" ;;
    --build-type) [[ $# -ge 2 ]] || die "$1 needs a value"; BUILD_TYPE="$2"; shift ;;
    -j|--jobs) [[ $# -ge 2 ]] || die "$1 needs a value"; JOBS="$2"; shift ;;
    -t|--target) [[ $# -ge 2 ]] || die "$1 needs a value"; TARGET="$2"; shift ;;
    --test) TEST=1 ;;
    --loop) TEST=1; LOOP=5 ;;
    --loop=*) TEST=1; LOOP="${1#*=}" ;;
    --timeout|--per-test-timeout) [[ $# -ge 2 ]] || die "$1 needs a value"; TIMEOUT="$2"; shift ;;
    --timeout=*) TIMEOUT="${1#*=}" ;;
    --asan) SANITIZER="ADDRESS" ;;
    --tsan) SANITIZER="THREAD" ;;
    --ubsan|--usan) SANITIZER="UNDEFINED" ;;
    --coverage) COVERAGE=1 ;;
    --docs) DOCS=1 ;;
    --tidy) TIDY=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
  shift
done

if [[ -z "$TIMEOUT" ]]; then
  if [[ -n "$SANITIZER" ]]; then TIMEOUT=1800; else TIMEOUT=20; fi
fi
for n in "$LOOP" "$TIMEOUT" ${JOBS:+"$JOBS"}; do
  case "$n" in ''|*[!0-9]*) die "expected a positive integer, got '$n'" ;; esac
done
[[ -n "$JOBS" ]] || JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

if [[ -z "$BUILD_DIR" ]]; then
  case "$SANITIZER" in
    ADDRESS) BUILD_DIR="$ROOT_DIR/build-asan" ;;
    THREAD) BUILD_DIR="$ROOT_DIR/build-tsan" ;;
    UNDEFINED) BUILD_DIR="$ROOT_DIR/build-ubsan" ;;
    *) BUILD_DIR="$ROOT_DIR/build" ;;
  esac
  [[ "$TIDY" -eq 0 ]] || BUILD_DIR="$ROOT_DIR/build-tidy"
fi

CMAKE_ARGS=(-S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE")
# An explicit sanitizer choice replaces the project default (ASan on for non-Release builds).
if [[ -n "$SANITIZER" ]]; then
  for s in ADDRESS THREAD UNDEFINED; do
    if [[ "$s" == "$SANITIZER" ]]; then CMAKE_ARGS+=("-DENABLE_${s}_SANITIZER=ON"); else CMAKE_ARGS+=("-DENABLE_${s}_SANITIZER=OFF"); fi
  done
fi
[[ "$COVERAGE" -eq 0 ]] || CMAKE_ARGS+=(-DENABLE_CODE_COVERAGE=ON)
# Sanitizer builds run several times slower; keep the test runner's own limit above ctest's.
[[ -z "$SANITIZER" ]] || CMAKE_ARGS+=(-DPATHSPACE_TEST_RUNNER_TIMEOUT="$TIMEOUT")
[[ "$DOCS" -eq 0 ]] || CMAKE_ARGS+=(-DENABLE_DOXYGEN=ON)
[[ "$TIDY" -eq 0 ]] || CMAKE_ARGS+=(-DPATHSPACE_CLANG_TIDY=ON)

if [[ "$CLEAN" -eq 1 ]]; then rm -rf "$BUILD_DIR"; fi
# Pick Ninja for new build directories only; existing ones keep their generator.
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then CMAKE_ARGS+=(-G Ninja); fi

# shellcheck disable=SC2086
cmake "${CMAKE_ARGS[@]}" ${PATHSPACE_CMAKE_ARGS:-}

BUILD_ARGS=(--build "$BUILD_DIR" --parallel "$JOBS")
if [[ "$DOCS" -eq 1 ]]; then BUILD_ARGS+=(--target docs); elif [[ -n "$TARGET" ]]; then BUILD_ARGS+=(--target "$TARGET"); fi
cmake "${BUILD_ARGS[@]}"

if [[ "$TEST" -eq 1 ]]; then
  [[ "$SANITIZER" != THREAD ]] || export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=0:report_thread_leaks=0}"
  ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout "$TIMEOUT" --repeat-until-fail "$LOOP"
fi

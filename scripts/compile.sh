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
TEST=0        # run tests after build if 1
LOOP=0        # run tests in a loop N times (default 15 if provided without value)
PER_TEST_TIMEOUT=""  # override seconds per test; default 60 (single), 120 (when --loop is used)
EXTRA_ARGS=""        # extra args passed to test executable (doctest)
DOCS=0               # generate Doxygen docs if 1
ENABLE_METAL_TESTS=1 # Metal presenter tests run by default

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
      --test                 Build and run tests (executes build/tests/PathSpaceTests).
      --loop[=N]            Run tests in a loop N times (default: 15). Implies --test.
      --per-test-timeout SECS  Override per-test timeout (default: 60; 120 when --loop is used).
      --docs                 Generate Doxygen docs into build/docs/html (requires doxygen).
      --args "..."           Extra arguments passed to the test runner (doctest)
      --enable-metal-tests   (default) Build with PATHSPACE_UI_METAL and run Metal presenter tests.
      --disable-metal-tests  Skip building/running the Metal presenter tests.
  -h, --help                 Show this help and exit.

Sanitizers (mutually exclusive, maps to CMake options in this repo):
      --asan                 Enable AddressSanitizer      (-DENABLE_ADDRESS_SANITIZER=ON)
      --tsan                 Enable ThreadSanitizer       (-DENABLE_THREAD_SANITIZER=ON)
      --usan | --ubsan       Enable UndefinedSanitizer    (-DENABLE_UNDEFINED_SANITIZER=ON)

Examples:
  $0
  $0 --clean -j 8 --release
  $0 -G "Ninja" --target PathSpaceTests
  $0 --test
  $0 --docs
EOF
}

# Determine CPU jobs (fallback to 4 if detection is unavailable)
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
    --test)
      TEST=1
      ;;
    --docs)
      DOCS=1
      ;;
    --loop)
      TEST=1
      LOOP=15
      ;;
    --loop=*)
      TEST=1
      LOOP="${1#*=}"
      case "$LOOP" in
        ''|*[!0-9]*)
          die "--loop requires a positive integer (e.g., --loop=15)"
          ;;
        *)
          if [[ "$LOOP" -lt 1 ]]; then
            die "--loop must be >= 1"
          fi
          ;;
      esac
      ;;
    --per-test-timeout)
      shift || die "Missing argument for $1"
      PER_TEST_TIMEOUT="$1"
      case "$PER_TEST_TIMEOUT" in
        ''|*[!0-9]*)
          die "--per-test-timeout requires a positive integer"
          ;;
        *)
          if [[ "$PER_TEST_TIMEOUT" -lt 1 ]]; then
            die "--per-test-timeout must be >= 1"
          fi
          ;;
      esac
      ;;
    --args)
      shift || die "Missing argument for $1"
      EXTRA_ARGS="$1"
      ;;
    --args=*)
      EXTRA_ARGS="${1#*=}"
      ;;
    --enable-metal-tests)
      ENABLE_METAL_TESTS=1
      ;;
    --disable-metal-tests)
      ENABLE_METAL_TESTS=0
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
if [[ "$ENABLE_METAL_TESTS" -eq 1 ]]; then
  if [[ "$(uname)" != "Darwin" ]]; then
    die "--enable-metal-tests is only supported on macOS hosts"
  fi
fi

if [[ -z "$BUILD_DIR" ]]; then
  BUILD_DIR="$BUILD_DIR_DEFAULT"
fi

# Jobs sanity and default parallelism
if [[ -z "${JOBS}" ]]; then
  JOBS="$(cpu_jobs)"
else
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
# Always enable UI tests when building via this helper so CI/push runs cover them.
CMAKE_FLAGS+=("-DPATHSPACE_ENABLE_UI=ON" "-DPATHSPACE_UI_SOFTWARE=ON")
if [[ "$ENABLE_METAL_TESTS" -eq 1 ]]; then
  CMAKE_FLAGS+=("-DPATHSPACE_UI_METAL=ON")
fi

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
CONFIGURE_CMD=( cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" ${PATHSPACE_CMAKE_ARGS:-} )
# Prefer Ninja by default if no generator was specified and it's available
if [[ -z "$GENERATOR" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  elif command -v ninja-build >/dev/null 2>&1; then
    GENERATOR="Ninja"
  fi
fi
if [[ -n "$GENERATOR" ]]; then
  CONFIGURE_CMD+=( -G "$GENERATOR" )
fi
CONFIGURE_CMD+=( "${CMAKE_FLAGS[@]}" )
if [[ "$ENABLE_METAL_TESTS" -eq 1 ]]; then
  info "Metal presenter tests enabled (PATHSPACE_UI_METAL=ON, PATHSPACE_ENABLE_METAL_UPLOADS=1 for test runs)"
fi

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
BUILD_CMD+=( --parallel "$JOBS" )

if [[ "$VERBOSE" -eq 1 ]]; then
  echo "Build: ${BUILD_CMD[*]}"
fi
"${BUILD_CMD[@]}"

info "Build completed."
info "Build directory: $BUILD_DIR"
if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
  info "compile_commands.json generated."
fi
if [[ "$DOCS" -eq 1 ]]; then
  require_tool doxygen
  DOXY_DIR="$BUILD_DIR/docs"
  info "Generating Doxygen docs in: $DOXY_DIR/html"
  mkdir -p "$DOXY_DIR"
  DOXYFILE="$BUILD_DIR/Doxyfile"
  cat > "$DOXYFILE" <<EOF
PROJECT_NAME           = PathSpace
OUTPUT_DIRECTORY       = $DOXY_DIR
GENERATE_HTML          = YES
HTML_OUTPUT            = html
GENERATE_LATEX         = NO
QUIET                  = YES
WARN_AS_ERROR          = NO
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = NO
INPUT                  = $ROOT_DIR/src/pathspace $ROOT_DIR/README.md $ROOT_DIR/docs
RECURSIVE              = YES
FILE_PATTERNS          = *.hpp *.h *.cpp *.md
USE_MDFILE_AS_MAINPAGE = $ROOT_DIR/README.md
EOF
  ( cd "$BUILD_DIR" && doxygen "$DOXYFILE" )
  if [[ -f "$DOXY_DIR/html/index.html" ]]; then
    info "Doxygen: $DOXY_DIR/html/index.html"
  else
    die "Doxygen generation failed (index.html not found)"
  fi
fi

# Determine per-test timeout default (if not provided)
if [[ -z "${PER_TEST_TIMEOUT}" ]]; then
  if [[ "$LOOP" -gt 0 ]]; then
    PER_TEST_TIMEOUT=120
  else
    PER_TEST_TIMEOUT=60
  fi
fi

# Friendly pointers
if [[ -d "$BUILD_DIR/tests" ]]; then
  TEST_RUNNER="$ROOT_DIR/scripts/run-test-with-logs.sh"
  CORE_TEST_EXE="$BUILD_DIR/tests/PathSpaceTests"
  UI_TEST_EXE="$BUILD_DIR/tests/PathSpaceUITests"

  if [[ -x "$CORE_TEST_EXE" ]]; then
    info "Test executable: $CORE_TEST_EXE"
  fi
  if [[ -x "$UI_TEST_EXE" ]]; then
    info "UI test executable: $UI_TEST_EXE"
  else
    info "UI test executable: not found (will skip PathSpaceUITests unless built)."
  fi

  if [[ "$TEST" -eq 1 ]]; then
    if [[ ! -x "$TEST_RUNNER" ]]; then
      die "Test runner helper not executable: $TEST_RUNNER"
    fi
    if [[ ! -x "$CORE_TEST_EXE" ]]; then
      die "Test executable not found or not executable: $CORE_TEST_EXE"
    fi

    TEST_PATHS=("$CORE_TEST_EXE")
    TEST_NAMES=("PathSpaceTests")
    if [[ -x "$UI_TEST_EXE" ]]; then
      TEST_PATHS+=("$UI_TEST_EXE")
      TEST_NAMES+=("PathSpaceUITests")
    fi

    TEST_LOG_DIR="$BUILD_DIR/test-logs"
    mkdir -p "$TEST_LOG_DIR"

    if [[ -n "$EXTRA_ARGS" ]]; then
      IFS=' ' read -r -a EXTRA_ARGS_ARRAY <<< "$EXTRA_ARGS"
    else
      EXTRA_ARGS_ARRAY=()
    fi

    PATHSPACE_TEST_TIMEOUT_VALUE="${PATHSPACE_TEST_TIMEOUT:-1}"
    MALLOC_NANO_ZONE_VALUE="${MallocNanoZone:-0}"
    PATHSPACE_LOG_VALUE="${PATHSPACE_LOG:-1}"

    TEST_ENV_FLAGS=(
      "--env" "PATHSPACE_LOG=${PATHSPACE_LOG_VALUE}"
      "--env" "PATHSPACE_TEST_TIMEOUT=${PATHSPACE_TEST_TIMEOUT_VALUE}"
      "--env" "MallocNanoZone=${MALLOC_NANO_ZONE_VALUE}"
    )
    if [[ "$ENABLE_METAL_TESTS" -eq 1 ]]; then
      TEST_ENV_FLAGS+=("--env" "PATHSPACE_ENABLE_METAL_UPLOADS=1")
    fi

    # Optional: enable core dumps/backtraces for debugging crashes
    if [[ "${PATHSPACE_ENABLE_CORES:-0}" == "1" ]]; then
      (ulimit -c unlimited) 2>/dev/null || true
      export ASAN_OPTIONS="${ASAN_OPTIONS:-} detect_leaks=0 fast_unwind_on_malloc=0 symbolize=1"
      export UBSAN_OPTIONS="${UBSAN_OPTIONS:-} print_stacktrace=1 halt_on_error=1"
    fi

    export USE_CLI_TIMEOUT="${USE_CLI_TIMEOUT:-0}"

    run_test_binary() {
      local exe_path="$1"
      local display_name="$2"
      local iteration="$3"
      local total="$4"

      local args=("$TEST_RUNNER" "--label" "$display_name" "--log-dir" "$TEST_LOG_DIR" "--timeout" "$PER_TEST_TIMEOUT")
      if [[ -n "$iteration" && -n "$total" && "$total" -gt 0 ]]; then
        args+=("--iteration" "$iteration" "--iterations" "$total")
      fi
      args+=("${TEST_ENV_FLAGS[@]}")
      args+=("--")
      args+=("$exe_path")
      if [[ ${#EXTRA_ARGS_ARRAY[@]} -gt 0 ]]; then
        args+=("${EXTRA_ARGS_ARRAY[@]}")
      fi

      "${args[@]}"
      return $?
    }

    if [[ "$LOOP" -gt 0 ]]; then
      COUNT="$LOOP"
      info "Running tests in a loop ($COUNT iterations)..."
      for i in $(seq 1 "$COUNT"); do
        info "Loop $i/$COUNT: starting"
        for idx in "${!TEST_PATHS[@]}"; do
          name="${TEST_NAMES[$idx]}"
          path="${TEST_PATHS[$idx]}"
          info "  Running ${name}..."
          if ! run_test_binary "$path" "$name" "$i" "$COUNT"; then
            RC=$?
            if [[ $RC -eq 124 ]]; then
              die "Loop $i failed (${name} timed out after ${PER_TEST_TIMEOUT} seconds)"
            else
              die "Loop $i failed (${name} exit code $RC)"
            fi
          fi
        done
        info "Loop $i/$COUNT: passed"
      done
      info "All $COUNT iterations passed."
    else
      for idx in "${!TEST_PATHS[@]}"; do
        name="${TEST_NAMES[$idx]}"
        path="${TEST_PATHS[$idx]}"
        info "Running ${name}..."
        if ! run_test_binary "$path" "$name" "" ""; then
          RC=$?
          if [[ $RC -eq 124 ]]; then
            die "${name} timed out after ${PER_TEST_TIMEOUT} seconds"
          else
            die "${name} failed with exit code $RC"
          fi
        fi
      done
      info "Tests completed successfully."
    fi
  fi
elif [[ "$TEST" -eq 1 ]]; then
  die "Tests directory not found: $BUILD_DIR/tests"
fi

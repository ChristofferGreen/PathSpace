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
SANITIZER_TEST_MODE=0
TEST=0        # run tests after build if 1
LOOP=0        # run tests in a loop N times (default 15 if provided without value)
PER_TEST_TIMEOUT=""  # override seconds per test; default 60 (single), 120 (when --loop is used)
EXTRA_ARGS=""        # extra args passed to test executable (doctest)
UI_TEST_EXTRA_ARGS="${PATHSPACE_UI_TEST_EXTRA_ARGS:-}" # extra args passed only to PathSpaceUITests
DOCS=0               # generate Doxygen docs if 1
ENABLE_METAL_TESTS=1 # Metal presenter tests run by default
SIZE_REPORT=0
SIZE_BASELINE=""
SIZE_WRITE_BASELINE=""
SIZE_BASELINE_DEFAULT="$ROOT_DIR/docs/perf/example_size_baseline.json"
SIZE_PRINT_DONE=0
PERF_REPORT=0
PERF_BASELINE=""
PERF_WRITE_BASELINE=""
PERF_HISTORY_DIR=""
PERF_BASELINE_DEFAULT="$ROOT_DIR/docs/perf/performance_baseline.json"
PERF_PRINT=0
METAL_FLAG_EXPLICIT=0
RUNTIME_FLAG_REPORT=0
RUNTIME_FLAG_REPORT_PATH=""
LOOP_KEEP_LOGS="${PATHSPACE_LOOP_KEEP_LOGS:-}"
LOOP_LABEL_FILTER="${PATHSPACE_LOOP_LABEL_FILTER:-}"

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
      --ui-test-extra-args "..."
                             Extra arguments passed only to PathSpaceUITests (e.g. "--success")
      --enable-metal-tests   (default) Build with PATHSPACE_UI_METAL and run Metal presenter tests.
      --disable-metal-tests  Skip building/running the Metal presenter tests.
      --loop-keep-logs LABELS  Comma/space separated labels whose logs are preserved even on success
                             (default when --loop is used: PathSpaceUITests). Use "none" to disable.
      --loop-label LABEL       Restrict --loop runs to one or more labels (repeat or comma-separate, supports globs)
      --size-report[=PATH]    Generate a binary size report for demo binaries and check against the
                             guardrail baseline (default baseline path: $SIZE_BASELINE_DEFAULT).
      --size-write-baseline[=PATH]
                             Record the current binary sizes as the new guardrail baseline.
      --perf-report[=PATH]     Run the performance guardrail checks (default baseline path:
                             $PERF_BASELINE_DEFAULT).
      --perf-write-baseline[=PATH]
                             Record current renderer/presenter metrics as the new performance baseline.
      --perf-history-dir PATH  Append performance run outputs to JSONL files under PATH.
  -h, --help                 Show this help and exit.

Sanitizers (mutually exclusive, maps to CMake options in this repo):
      --asan                 Enable AddressSanitizer      (-DENABLE_ADDRESS_SANITIZER=ON)
      --asan-test            Enable ASan and run the test suite once (defaults to build-dir ./build-asan).
      --tsan                 Enable ThreadSanitizer       (-DENABLE_THREAD_SANITIZER=ON)
      --tsan-test            Enable TSan and run the test suite once (defaults to build-dir ./build-tsan).
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

run_size_guardrail() {
  require_tool python3
  local args=("--build-dir" "$BUILD_DIR")
  if [[ "$SIZE_PRINT_DONE" -eq 0 ]]; then
    args+=("--print")
    SIZE_PRINT_DONE=1
  fi
  while [[ $# -gt 0 ]]; do
    args+=("$1")
    shift
  done
  python3 "$SCRIPT_DIR/size_guardrail.py" "${args[@]}"
}

run_perf_guardrail() {
  require_tool python3
  local args=("--build-dir" "$BUILD_DIR" "--build-type" "$BUILD_TYPE" "--jobs" "$JOBS")
  if [[ -n "$PERF_BASELINE" ]]; then
    args+=("--baseline" "$PERF_BASELINE")
  else
    args+=("--baseline" "$PERF_BASELINE_DEFAULT")
  fi
  if [[ -n "$PERF_HISTORY_DIR" ]]; then
    args+=("--history-dir" "$PERF_HISTORY_DIR")
  fi
  if [[ "$PERF_PRINT" -eq 1 ]]; then
    args+=("--print")
  fi
  if [[ "$VERBOSE" -eq 1 ]]; then
    args+=("--verbose")
  fi
  if [[ "$1" == "write" ]]; then
    args+=("--write-baseline")
  fi
  python3 "$SCRIPT_DIR/perf_guardrail.py" "${args[@]}"
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
    --asan-test)
      if [[ -n "$SANITIZER" && "$SANITIZER" != "asan" ]]; then
        die "Only one sanitizer flag may be used at a time."
      fi
      SANITIZER="asan"
      SANITIZER_TEST_MODE=1
      TEST=1
      if [[ "$LOOP" -eq 0 ]]; then
        LOOP=1
      fi
      ;;
    --tsan)
      if [[ -n "$SANITIZER" && "$SANITIZER" != "tsan" ]]; then
        die "Only one sanitizer flag may be used at a time."
      fi
      SANITIZER="tsan"
      ;;
    --tsan-test)
      if [[ -n "$SANITIZER" && "$SANITIZER" != "tsan" ]]; then
        die "Only one sanitizer flag may be used at a time."
      fi
      SANITIZER="tsan"
      SANITIZER_TEST_MODE=1
      TEST=1
      if [[ "$LOOP" -eq 0 ]]; then
        LOOP=1
      fi
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
    --ui-test-extra-args)
      shift || die "Missing argument for $1"
      if [[ -z "$UI_TEST_EXTRA_ARGS" ]]; then
        UI_TEST_EXTRA_ARGS="$1"
      else
        UI_TEST_EXTRA_ARGS+=" $1"
      fi
      ;;
    --ui-test-extra-args=*)
      value="${1#*=}"
      if [[ -z "$UI_TEST_EXTRA_ARGS" ]]; then
        UI_TEST_EXTRA_ARGS="$value"
      else
        UI_TEST_EXTRA_ARGS+=" $value"
      fi
      ;;
    --enable-metal-tests)
      ENABLE_METAL_TESTS=1
      METAL_FLAG_EXPLICIT=1
      ;;
    --disable-metal-tests)
      ENABLE_METAL_TESTS=0
      METAL_FLAG_EXPLICIT=1
      ;;
    --loop-keep-logs)
      shift || die "Missing argument for $1"
      LOOP_KEEP_LOGS="$1"
      ;;
    --loop-keep-logs=*)
      LOOP_KEEP_LOGS="${1#*=}"
      ;;
    --loop-label)
      shift || die "Missing argument for $1"
      if [[ -z "$LOOP_LABEL_FILTER" ]]; then
        LOOP_LABEL_FILTER="$1"
      else
        LOOP_LABEL_FILTER+=" $1"
      fi
      ;;
    --loop-label=*)
      value="${1#*=}"
      if [[ -z "$LOOP_LABEL_FILTER" ]]; then
        LOOP_LABEL_FILTER="$value"
      else
        LOOP_LABEL_FILTER+=" $value"
      fi
      ;;
    --size-report)
      SIZE_REPORT=1
      ;;
    --size-report=*)
      SIZE_REPORT=1
      SIZE_BASELINE="${1#*=}"
      ;;
    --size-write-baseline)
      SIZE_WRITE_BASELINE="$SIZE_BASELINE_DEFAULT"
      ;;
    --size-write-baseline=*)
      SIZE_WRITE_BASELINE="${1#*=}"
      ;;
    --perf-report)
      PERF_REPORT=1
      ;;
    --perf-report=*)
      PERF_REPORT=1
      PERF_BASELINE="${1#*=}"
      ;;
    --perf-write-baseline)
      PERF_WRITE_BASELINE=1
      ;;
    --perf-write-baseline=*)
      PERF_WRITE_BASELINE=1
      PERF_BASELINE="${1#*=}"
      ;;
    --perf-history-dir)
      shift || die "Missing argument for $1"
      PERF_HISTORY_DIR="$1"
      ;;
    --perf-history-dir=*)
      PERF_HISTORY_DIR="${1#*=}"
      ;;
    --runtime-flag-report)
      RUNTIME_FLAG_REPORT=1
      TEST=1
      if [[ "$LOOP" -eq 0 ]]; then
        LOOP=15
      fi
      ;;
    --runtime-flag-report=*)
      RUNTIME_FLAG_REPORT=1
      RUNTIME_FLAG_REPORT_PATH="${1#*=}"
      TEST=1
      if [[ "$LOOP" -eq 0 ]]; then
        LOOP=15
      fi
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

if [[ "$SIZE_REPORT" -eq 1 && -z "$SIZE_BASELINE" ]]; then
  SIZE_BASELINE="$SIZE_BASELINE_DEFAULT"
fi
if [[ "$PERF_REPORT" -eq 1 || "$PERF_WRITE_BASELINE" -eq 1 ]]; then
  if [[ -z "$PERF_BASELINE" ]]; then
    PERF_BASELINE="$PERF_BASELINE_DEFAULT"
  fi
  PERF_PRINT=1
fi

if [[ -n "$SANITIZER" && -z "$BUILD_DIR" ]]; then
  BUILD_DIR="$ROOT_DIR/build-$SANITIZER"
fi

if [[ -n "$SANITIZER" && "$METAL_FLAG_EXPLICIT" -eq 0 ]]; then
  ENABLE_METAL_TESTS=0
fi

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

if [[ "$RUNTIME_FLAG_REPORT" -eq 1 && -z "$RUNTIME_FLAG_REPORT_PATH" ]]; then
  RUNTIME_FLAG_REPORT_PATH="$BUILD_DIR/trellis_flag_bench.json"
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
if [[ "$SIZE_REPORT" -eq 1 || -n "$SIZE_WRITE_BASELINE" || "$PERF_REPORT" -eq 1 || "$PERF_WRITE_BASELINE" -eq 1 || "$TEST" -eq 1 ]]; then
  CMAKE_FLAGS+=("-DBUILD_PATHSPACE_EXAMPLES=ON")
fi
if [[ "$PERF_REPORT" -eq 1 || "$PERF_WRITE_BASELINE" -eq 1 ]]; then
  CMAKE_FLAGS+=("-DBUILD_PATHSPACE_BENCHMARKS=ON")
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

if [[ -n "$SIZE_WRITE_BASELINE" ]]; then
  run_size_guardrail "--baseline" "$SIZE_WRITE_BASELINE" "--record-baseline"
fi
if [[ "$SIZE_REPORT" -eq 1 ]]; then
  run_size_guardrail "--baseline" "$SIZE_BASELINE"
fi
if [[ "$PERF_WRITE_BASELINE" -eq 1 ]]; then
  run_perf_guardrail "write"
fi
if [[ "$PERF_REPORT" -eq 1 ]]; then
  run_perf_guardrail "check"
fi

# Determine per-test timeout default (if not provided)
if [[ -z "${PER_TEST_TIMEOUT}" ]]; then
  if [[ "$LOOP" -gt 0 ]]; then
    PER_TEST_TIMEOUT=20
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

    TEST_LOG_DIR="$BUILD_DIR/test-logs"
    mkdir -p "$TEST_LOG_DIR"
    TEST_LOG_MANIFEST=""
    if [[ "$LOOP" -gt 0 ]]; then
      TEST_LOG_MANIFEST="$TEST_LOG_DIR/loop_manifest.tsv"
      : > "$TEST_LOG_MANIFEST"
    fi

    if [[ -n "$EXTRA_ARGS" ]]; then
      IFS=' ' read -r -a EXTRA_ARGS_ARRAY <<< "$EXTRA_ARGS"
    else
      EXTRA_ARGS_ARRAY=()
    fi

    if [[ -n "$UI_TEST_EXTRA_ARGS" ]]; then
      IFS=' ' read -r -a UI_TEST_EXTRA_ARGS_ARRAY <<< "$UI_TEST_EXTRA_ARGS"
    else
      UI_TEST_EXTRA_ARGS_ARRAY=()
    fi

    PATHSPACE_TEST_TIMEOUT_VALUE="${PATHSPACE_TEST_TIMEOUT:-1}"
    MALLOC_NANO_ZONE_VALUE="${MallocNanoZone:-0}"
    PATHSPACE_LOG_VALUE="${PATHSPACE_LOG:-1}"

    TEST_ENV_FLAGS=(
      "--env" "PATHSPACE_LOG=${PATHSPACE_LOG_VALUE}"
      "--env" "PATHSPACE_TEST_TIMEOUT=${PATHSPACE_TEST_TIMEOUT_VALUE}"
      "--env" "MallocNanoZone=${MALLOC_NANO_ZONE_VALUE}"
    )
    if [[ -n "$TEST_LOG_MANIFEST" ]]; then
      export PATHSPACE_TEST_LOG_MANIFEST="$TEST_LOG_MANIFEST"
      TEST_ENV_FLAGS+=("--env" "PATHSPACE_TEST_LOG_MANIFEST=${TEST_LOG_MANIFEST}")
    fi
    if [[ "$ENABLE_METAL_TESTS" -eq 1 ]]; then
      TEST_ENV_FLAGS+=("--env" "PATHSPACE_ENABLE_METAL_UPLOADS=1")
    fi
    if [[ "$SANITIZER" == "asan" ]]; then
      export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:strict_init_order=1:color=always}"
      export LSAN_OPTIONS="${LSAN_OPTIONS:-}"
    elif [[ "$SANITIZER" == "tsan" ]]; then
      export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:report_thread_leaks=0}"
    fi

    if [[ "${PATHSPACE_ENABLE_CORES:-0}" == "1" ]]; then
      (ulimit -c unlimited) 2>/dev/null || true
      export ASAN_OPTIONS="${ASAN_OPTIONS:-} detect_leaks=0 fast_unwind_on_malloc=0 symbolize=1"
      export UBSAN_OPTIONS="${UBSAN_OPTIONS:-} print_stacktrace=1 halt_on_error=1"
    fi

    export USE_CLI_TIMEOUT="${USE_CLI_TIMEOUT:-0}"

    TEST_LABELS=()
    TEST_COMMAND_STRINGS=()

    add_test_command() {
      local label="$1"
      shift
      local idx="${#TEST_LABELS[@]}"
      TEST_LABELS+=("$label")
      local sep=$'\x1f'
      local command_str=""
      while [[ "$#" -gt 0 ]]; do
        if [[ -n "$command_str" ]]; then
          command_str+="$sep"
        fi
        command_str+="$1"
        shift
      done
      TEST_COMMAND_STRINGS+=("$command_str")
    }

    KEEP_SUCCESS_LABELS_RAW="$LOOP_KEEP_LOGS"
    if [[ -z "$KEEP_SUCCESS_LABELS_RAW" && "$LOOP" -gt 0 ]]; then
      KEEP_SUCCESS_LABELS_RAW="PathSpaceUITests"
    fi

    KEEP_SUCCESS_LABELS=()
    if [[ -n "$KEEP_SUCCESS_LABELS_RAW" ]]; then
      if [[ "$KEEP_SUCCESS_LABELS_RAW" == "none" || "$KEEP_SUCCESS_LABELS_RAW" == "NONE" ]]; then
        KEEP_SUCCESS_LABELS=()
      else
        KEEP_SUCCESS_LABELS_RAW="${KEEP_SUCCESS_LABELS_RAW//,/ }"
        for entry in $KEEP_SUCCESS_LABELS_RAW; do
          [[ -z "$entry" ]] && continue
          KEEP_SUCCESS_LABELS+=("$entry")
        done
      fi
    fi

    LOOP_FILTER_LABELS=()
    if [[ -n "$LOOP_LABEL_FILTER" ]]; then
      LOOP_LABEL_FILTER="${LOOP_LABEL_FILTER//,/ }"
      for entry in $LOOP_LABEL_FILTER; do
        [[ -z "$entry" ]] && continue
        LOOP_FILTER_LABELS+=("$entry")
      done
    fi

    should_keep_success_log() {
      local label="$1"
      if [[ "$LOOP" -le 0 ]]; then
        return 1
      fi
      if [[ ${#KEEP_SUCCESS_LABELS[@]} -eq 0 ]]; then
        return 1
      fi
      for entry in "${KEEP_SUCCESS_LABELS[@]}"; do
        case "$label" in
          $entry)
            return 0
            ;;
        esac
      done
      return 1
    }

    report_log_manifest() {
      if [[ -n "$TEST_LOG_MANIFEST" && -s "$TEST_LOG_MANIFEST" ]]; then
        info "Loop log manifest: $TEST_LOG_MANIFEST"
      fi
    }

    archive_loop_failure() {
      local label="$1"
      local iteration="$2"
      local exit_code="$3"
      local reason="$4"
      local failure_root="$ROOT_DIR/test-logs/loop_failures"
      mkdir -p "$failure_root"
      local timestamp
      timestamp="$(date +"%Y%m%d-%H%M%S")"
      local safe_label="${label//[^A-Za-z0-9._-]/_}"
      local iteration_slug="loop${iteration:-unknown}"
      local failure_dir="$failure_root/${timestamp}_${safe_label}_${iteration_slug}"
      mkdir -p "$failure_dir"

      if [[ -n "${TEST_LOG_MANIFEST:-}" && -s "$TEST_LOG_MANIFEST" ]]; then
        cp "$TEST_LOG_MANIFEST" "$failure_dir/loop_manifest.tsv" >/dev/null 2>&1 || true
      fi

      local log_path=""
      local artifact_dir=""
      if command -v python3 >/dev/null 2>&1 && [[ -n "${TEST_LOG_MANIFEST:-}" && -s "$TEST_LOG_MANIFEST" ]]; then
        local -a manifest_lookup=()
        if mapfile -t manifest_lookup < <(python3 - "$TEST_LOG_MANIFEST" "$label" "${iteration:-}" <<'PY'
import csv
import sys

manifest_path = sys.argv[1]
label = sys.argv[2]
iteration = sys.argv[3]
rows = []
with open(manifest_path, newline='') as fh:
    reader = csv.reader(fh, delimiter='\t')
    for row in reader:
        if len(row) < 5:
            continue
        if row[0] != label:
            continue
        if iteration and row[1] != iteration:
            continue
        rows.append(row)
if not rows:
    sys.exit(1)
row = rows[-1]
print(row[3])
print(row[4])
PY
); then
          if [[ ${#manifest_lookup[@]} -ge 1 ]]; then
            log_path="${manifest_lookup[0]}"
          fi
          if [[ ${#manifest_lookup[@]} -ge 2 ]]; then
            artifact_dir="${manifest_lookup[1]}"
          fi
        fi
      fi

      if [[ -n "$log_path" && -f "$log_path" ]]; then
        cp "$log_path" "$failure_dir/" >/dev/null 2>&1 || true
      fi
      if [[ -n "$artifact_dir" && -d "$artifact_dir" ]]; then
        cp -R "$artifact_dir" "$failure_dir/" >/dev/null 2>&1 || true
      fi

      {
        echo "label: $label"
        echo "iteration: ${iteration:-unknown}"
        echo "exit_code: $exit_code"
        if [[ -n "$reason" ]]; then
          echo "reason: $reason"
        fi
        if [[ -n "$log_path" ]]; then
          echo "log: $log_path"
        fi
        if [[ -n "$artifact_dir" ]]; then
          echo "artifacts: $artifact_dir"
        fi
      } >"$failure_dir/summary.txt"

      info "Archived ${label} loop ${iteration:-?} logs to $failure_dir"
    }

    loop_should_run_label() {
      local label="$1"
      if [[ "$LOOP" -le 0 ]]; then
        return 0
      fi
      if [[ ${#LOOP_FILTER_LABELS[@]} -eq 0 ]]; then
        return 0
      fi
      for entry in "${LOOP_FILTER_LABELS[@]}"; do
        case "$label" in
          $entry)
            return 0
            ;;
        esac
      done
      return 1
    }

    add_test_command "PathSpaceTests" "$CORE_TEST_EXE" "${EXTRA_ARGS_ARRAY[@]}"
    if [[ -x "$UI_TEST_EXE" ]]; then
      add_test_command "PathSpaceUITests" "$UI_TEST_EXE" "${EXTRA_ARGS_ARRAY[@]}" "${UI_TEST_EXTRA_ARGS_ARRAY[@]}"
    fi

    if [[ "$LOOP" -gt 0 && ${#LOOP_FILTER_LABELS[@]} -gt 0 ]]; then
      for entry in "${LOOP_FILTER_LABELS[@]}"; do
        found_match=0
        for existing in "${TEST_LABELS[@]}"; do
          case "$existing" in
            $entry)
              found_match=1
              break
              ;;
          esac
        done
        if [[ "$found_match" -eq 0 ]]; then
          die "Loop label filter '$entry' did not match any configured tests"
        fi
      done
    fi

    if [[ "$(uname)" == "Darwin" ]]; then
      pixel_script="$ROOT_DIR/scripts/check_pixel_noise_baseline.py"
      pixel_baseline="$ROOT_DIR/docs/perf/pixel_noise_baseline.json"
      if command -v python3 >/dev/null 2>&1 && [[ -f "$pixel_script" ]]; then
        if [[ -f "$pixel_baseline" ]]; then
          add_test_command "PixelNoisePerfHarness" python3 "$pixel_script" --build-dir "$BUILD_DIR"
        else
          info "PixelNoisePerfHarness baseline missing at $pixel_baseline; skipping."
        fi
        if [[ "$ENABLE_METAL_TESTS" -eq 1 ]]; then
          pixel_metal_baseline="$ROOT_DIR/docs/perf/pixel_noise_metal_baseline.json"
          if [[ -f "$pixel_metal_baseline" ]]; then
            add_test_command "PixelNoisePerfHarnessMetal" python3 "$pixel_script" --build-dir "$BUILD_DIR" --baseline "$pixel_metal_baseline"
          else
            info "PixelNoisePerfHarnessMetal baseline missing at $pixel_metal_baseline; skipping."
          fi
        fi
      else
        info "python3 or pixel noise harness script unavailable; skipping PixelNoise perf harness tests."
      fi
    fi

    paint_screenshot_script="$ROOT_DIR/scripts/check_paint_screenshot.py"
    paint_manifest="$ROOT_DIR/docs/images/paint_example_baselines.json"
    if command -v python3 >/dev/null 2>&1 && [[ -f "$paint_screenshot_script" ]]; then
      add_test_command "PaintExampleScreenshot" python3 "$paint_screenshot_script" --build-dir "$BUILD_DIR" --manifest "$paint_manifest"
      add_test_command "PaintExampleScreenshot720" python3 "$paint_screenshot_script" --build-dir "$BUILD_DIR" --baseline "$ROOT_DIR/docs/images/paint_example_720_baseline.png" --height 720 --tag paint_720 --manifest "$paint_manifest"
    else
      info "PaintExampleScreenshot harness unavailable; skipping (python3 or script missing)."
    fi

    if [[ ${#TEST_LABELS[@]} -eq 0 ]]; then
      die "No tests configured to run."
    fi

    if [[ "$RUNTIME_FLAG_REPORT" -eq 1 ]]; then
      require_tool python3
    fi

    run_test_command() {
      local display_name="$1"
      local iteration="$2"
      local total="$3"
      shift 3

      local args=("$TEST_RUNNER" "--label" "$display_name" "--log-dir" "$TEST_LOG_DIR" "--timeout" "$PER_TEST_TIMEOUT")
      if [[ -n "$iteration" && -n "$total" && "$total" -gt 0 ]]; then
        args+=("--iteration" "$iteration" "--iterations" "$total")
      fi
      args+=("${TEST_ENV_FLAGS[@]}")
      if should_keep_success_log "$display_name"; then
        args+=("--keep-success-log")
      fi
      args+=("--")
      args+=("$@")

      "${args[@]}"
      return $?
    }

    if [[ "$LOOP" -gt 0 ]]; then
      COUNT="$LOOP"
      if [[ "$RUNTIME_FLAG_REPORT" -eq 1 ]]; then
        info "Runtime flag report enabled; writing results to $RUNTIME_FLAG_REPORT_PATH"
        RUNTIME_FLAG_OUTPUT_LINES=()

        runtime_flag_loop() {
          local flag="$1"
          local label="$2"
          local count="$COUNT"
          local original_env_flags=("${TEST_ENV_FLAGS[@]}")
          TEST_ENV_FLAGS+=("--env" "PATHSPACE_TRELLIS_INTERNAL_RUNTIME=${flag}")

          local start_time
          start_time=$(python3 - <<'PY'
import time
print(f"{time.time():.6f}")
PY
)

          info "[$label] Running tests in a loop ($count iterations) with PATHSPACE_TRELLIS_INTERNAL_RUNTIME=${flag}"
          for i in $(seq 1 "$count"); do
            info "[$label] Loop $i/$count: starting"
            for idx in "${!TEST_LABELS[@]}"; do
              name="${TEST_LABELS[$idx]}"
              command_str="${TEST_COMMAND_STRINGS[$idx]}"
              if ! loop_should_run_label "$name"; then
                info "  [$label] Skipping ${name} (loop filter)"
                continue
              fi
              IFS=$'\x1f' read -r -a COMMAND <<< "$command_str"
              info "  [$label] Running ${name}..."
              if ! run_test_command "$name" "$i" "$count" "${COMMAND[@]}"; then
                RC=$?
                if [[ $RC -eq 124 ]]; then
                  die "[$label] Loop $i failed (${name} timed out after ${PER_TEST_TIMEOUT} seconds)"
                else
                  die "[$label] Loop $i failed (${name} exit code $RC)"
                fi
              fi
            done
            info "[$label] Loop $i/$count: passed"
          done
          info "[$label] All $count iterations passed."

          local end_time
          end_time=$(python3 - <<'PY'
import time
print(f"{time.time():.6f}")
PY
)

          local summary
          summary=$(COUNT="$count" START="$start_time" END="$end_time" python3 - <<'PY'
import os
total = float(os.environ["END"]) - float(os.environ["START"])
count = int(os.environ["COUNT"])
print(f"{total:.6f}|{total/count:.6f}")
PY
)
          local total_seconds="${summary%%|*}"
          local average_seconds="${summary##*|}"
          info "[$label] Total loop time: ${total_seconds}s (avg ${average_seconds}s/iter)."

          RUNTIME_FLAG_OUTPUT_LINES+=("${flag}|${total_seconds}|${average_seconds}")
          if [[ ${#original_env_flags[@]} -gt 0 ]]; then
            TEST_ENV_FLAGS=("${original_env_flags[@]}")
          else
            TEST_ENV_FLAGS=()
          fi
        }

        runtime_flag_loop 1 "runtime-on"
        runtime_flag_loop 0 "runtime-off"

        if [[ ${#RUNTIME_FLAG_OUTPUT_LINES[@]} -gt 0 ]]; then
          run_lines=$(printf '%s\n' "${RUNTIME_FLAG_OUTPUT_LINES[@]}")
          RUN_LINES="$run_lines" python3 - "$RUNTIME_FLAG_REPORT_PATH" "$COUNT" "$PER_TEST_TIMEOUT" "$BUILD_TYPE" <<'PY'
import json, os, sys, datetime
path = sys.argv[1]
loop = int(sys.argv[2])
timeout = int(sys.argv[3])
build_type = sys.argv[4]
runs = []
for line in os.environ.get("RUN_LINES", "").splitlines():
    if not line:
        continue
    flag, total, average = line.split("|")
    runs.append({
        "flag": int(flag),
        "total_seconds": float(total),
        "average_seconds": float(average)
    })
report = {
    "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "loop_iterations": loop,
    "per_test_timeout": timeout,
    "build_type": build_type,
    "runs": runs
}
with open(path, "w", encoding="ascii") as handle:
    json.dump(report, handle, indent=2)
PY
          info "Runtime flag report written to $RUNTIME_FLAG_REPORT_PATH"
        fi

        report_log_manifest
      else
        info "Running tests in a loop ($COUNT iterations)..."
        for i in $(seq 1 "$COUNT"); do
          info "Loop $i/$COUNT: starting"
        for idx in "${!TEST_LABELS[@]}"; do
          name="${TEST_LABELS[$idx]}"
          command_str="${TEST_COMMAND_STRINGS[$idx]}"
          if ! loop_should_run_label "$name"; then
            info "  Skipping ${name} (loop filter)"
            continue
          fi
          IFS=$'\x1f' read -r -a COMMAND <<< "$command_str"
          info "  Running ${name}..."
          set +e
          run_test_command "$name" "$i" "$COUNT" "${COMMAND[@]}"
          RC=$?
          set -e
          if [[ $RC -ne 0 ]]; then
            failure_reason="exit code ${RC}"
            if [[ $RC -eq 124 ]]; then
              failure_reason="timeout after ${PER_TEST_TIMEOUT} seconds"
            fi
            archive_loop_failure "$name" "$i" "$RC" "$failure_reason"
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
        report_log_manifest
      fi
    else
      for idx in "${!TEST_LABELS[@]}"; do
        name="${TEST_LABELS[$idx]}"
        command_str="${TEST_COMMAND_STRINGS[$idx]}"
        IFS=$'\x1f' read -r -a COMMAND <<< "$command_str"
        info "Running ${name}..."
        set +e
        run_test_command "$name" "" "" "${COMMAND[@]}"
        RC=$?
        set -e
        if [[ $RC -ne 0 ]]; then
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
else
  if [[ "$TEST" -eq 1 ]]; then
    die "Tests directory not found: $BUILD_DIR/tests"
  fi
fi

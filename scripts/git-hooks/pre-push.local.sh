#!/usr/bin/env bash
# Local pre-push helper
# Build, run tests in a loop, and smoke-test the minimal_button_example app locally.
#
# How to install:
#   ln -sf ../../scripts/git-hooks/pre-push.local.sh .git/hooks/pre-push
#
# Env toggles:
#   SKIP_LOOP_TESTS=1       -> skip the test loop
#   SKIP_EXAMPLE=1          -> skip the example app smoke test
#   BUILD_TYPE=Release      -> override build type (Default: Release)
#   JOBS=N                  -> parallel build jobs (Default: nproc/sysctl)
#   PATHSPACE_CMAKE_ARGS=.. -> extra CMake args (quoted)
#   ENABLE_PATHIO_MACOS=ON  -> on macOS, enable PathIO macOS backends in the example build
#   DISABLE_METAL_TESTS=1   -> skip Metal presenter coverage even on macOS
#   RUN_ASAN=1              -> run an additional AddressSanitizer build/test pass
#   RUN_TSAN=1              -> run an additional ThreadSanitizer build/test pass
#   ASAN_LOOP=N             -> override ASan test loop iterations (default 1)
#   TSAN_LOOP=N             -> override TSan test loop iterations (default 1)
#   SANITIZER_CLEAN=1       -> force sanitized passes to clean their build directories before running
#   SANITIZER_BUILD_TYPE=TYPE -> override sanitized build type (default Debug)

set -euo pipefail

PREPUSH_RUN_STAMP="$(date -u +"%Y%m%d-%H%M%S")"
PREPUSH_START_EPOCH="$(date +%s)"
PREPUSH_START_ISO="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
PREPUSH_HOSTNAME="$(hostname 2>/dev/null || echo unknown)"

# ----- Utils -----

say()  { printf "\033[1;34m[pre-push]\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32m[pre-push]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[pre-push]\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31m[pre-push]\033[0m %s\n" "$*" >&2; }

json_escape() {
  local input="${1:-}"
  input="${input//\\/\\\\}"
  input="${input//\"/\\\"}"
  input="${input//$'\n'/\\n}"
  input="${input//$'\r'/\\r}"
  printf '%s' "$input"
}

write_prepush_summary() {
  local exit_code="$1"
  local end_epoch="$(date +%s)"
  local end_iso="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  local duration=$((end_epoch - PREPUSH_START_EPOCH))
  local status="failure"
  if [[ "$exit_code" -eq 0 ]]; then
    status="success"
  fi

  local summary_dir="${PREPUSH_SUMMARY_DIR:-}"
  local summary_path="${PREPUSH_SUMMARY_PATH:-}"
  if [[ -z "$summary_dir" || -z "$summary_path" ]]; then
    return 0
  fi
  if ! mkdir -p "$summary_dir"; then
    warn "Unable to create $summary_dir for pre-push summary"
    return 0
  fi

  cat >"$summary_path" <<EOF
{
  "status": "$(json_escape "$status")",
  "exit_code": $exit_code,
  "start_iso": "$(json_escape "$PREPUSH_START_ISO")",
  "end_iso": "$(json_escape "$end_iso")",
  "duration_seconds": $duration,
  "build_type": "$(json_escape "${BUILD_TYPE:-}")",
  "jobs": "$(json_escape "${JOBS:-}")",
  "host": "$(json_escape "$PREPUSH_HOSTNAME")",
  "skip_loop_tests": "$(json_escape "${SKIP_LOOP_TESTS:-0}")",
  "skip_example": "$(json_escape "${SKIP_EXAMPLE:-0}")",
  "run_asan": "$(json_escape "${RUN_ASAN:-0}")",
  "run_tsan": "$(json_escape "${RUN_TSAN:-0}")",
  "asan_loop": "$(json_escape "${ASAN_LOOP:-}")",
  "tsan_loop": "$(json_escape "${TSAN_LOOP:-}")",
  "skip_history_cli": "$(json_escape "${SKIP_HISTORY_CLI:-0}")",
  "metal_tests_requested": "$(json_escape "${METAL_TESTS_REQUESTED:-0}")",
  "cmake_args": "$(json_escape "${PATHSPACE_CMAKE_ARGS:-}")"
}
EOF

  if [[ "$status" == "success" ]]; then
    say "Pre-push summary saved to $summary_path"
  else
    warn "Pre-push summary saved to $summary_path"
  fi
}

repo_root() {
  git rev-parse --show-toplevel 2>/dev/null || {
    err "Not inside a git repository."
    exit 1
  }
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

have_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    echo "timeout"
  elif command -v gtimeout >/dev/null 2>&1; then
    echo "gtimeout"
  else
    echo ""
  fi
}

# Run a command with a soft timeout; if timeout tool is unavailable, emulate with bg/kill.
# Args: seconds, command...
run_with_timeout() {
  local secs="$1"; shift
  local tbin
  tbin="$(have_timeout)"
  if [[ -n "$tbin" ]]; then
    # Use GNU/BSD timeout if available
    if "$tbin" "${secs}s" "$@"; then
      return 0
    else
      local rc=$?
      # Treat timeout exit (usually 124) as success for a smoke run
      if [[ $rc -eq 124 ]]; then
        warn "Command timed out after ${secs}s (treated as success for smoke test): $*"
        return 0
      fi
      return $rc
    fi
  fi

  # Fallback: naive bg+sleep+kill orchestration
  "$@" &
  local pid=$!
  local finished=0
  (
    wait "$pid" && finished=1 || finished=$?
    echo "$finished" > /tmp/prepush_wait.$$ 2>/dev/null || true
  ) &

  local waited=0
  while [[ $waited -lt $secs ]]; do
    if [[ -f /tmp/prepush_wait.$$ ]]; then
      local rc
      rc="$(cat /tmp/prepush_wait.$$ 2>/dev/null || echo 1)"
      rm -f /tmp/prepush_wait.$$ || true
      return "$rc"
    fi
    sleep 1
    waited=$((waited + 1))
  done
  warn "Timeout (${secs}s) reached; terminating: $*"
  kill -TERM "$pid" 2>/dev/null || true
  sleep 1
  kill -KILL "$pid" 2>/dev/null || true
  rm -f /tmp/prepush_wait.$$ || true
  # Treat timeout as success for smoke test
  return 0
}

PREPUSH_COMMAND_TIMEOUT="${PREPUSH_COMMAND_TIMEOUT:-520}"

run_with_hard_timeout() {
  local label="$1"; shift
  local secs="$PREPUSH_COMMAND_TIMEOUT"
  local env_args=()
  while [[ $# -gt 0 && "$1" == *=* ]]; do
    env_args+=("$1")
    shift
  done
  if [[ $# -eq 0 ]]; then
    err "$label missing command"
    exit 1
  fi
  local cmd=("$@")
  local exec_cmd=()
  if [[ ${#env_args[@]} -gt 0 ]]; then
    exec_cmd=(env "${env_args[@]}" "${cmd[@]}")
  else
    exec_cmd=("${cmd[@]}")
  fi

  local tbin
  tbin="$(have_timeout)"
  if [[ -n "$tbin" ]]; then
    if "$tbin" "${secs}s" "${exec_cmd[@]}"; then
      return 0
    fi
    local rc=$?
    if [[ $rc -eq 124 ]]; then
      err "$label timed out after ${secs}s: ${cmd[*]}"
    else
      err "$label failed (exit $rc): ${cmd[*]}"
    fi
    exit 1
  fi

  if "${exec_cmd[@]}"; then
    return 0
  fi
  local rc=$?
  err "$label failed (exit $rc): ${cmd[*]}"
  exit 1
}

# ----- Main -----

ROOT="$(repo_root)"
cd "$ROOT"

PREPUSH_SUMMARY_DIR="$ROOT/build/test-logs/pre-push"
PREPUSH_SUMMARY_PATH="$PREPUSH_SUMMARY_DIR/pre-push_${PREPUSH_RUN_STAMP}_pid$$.json"

if [[ -z "${PATHSPACE_TEST_ARTIFACT_DIR:-}" ]]; then
  export PATHSPACE_TEST_ARTIFACT_DIR="$ROOT/build/test-logs/pre-push_artifacts"
fi
mkdir -p "$PATHSPACE_TEST_ARTIFACT_DIR"
FONT_ATLAS_ARTIFACT="$PATHSPACE_TEST_ARTIFACT_DIR/widget_gallery_font_assets.bin"
# When skipping the main test loop, leave any provided artifact in place (or
# seed an empty placeholder) so the later guard does not fail unnecessarily.
if [[ "${SKIP_LOOP_TESTS:-0}" == "1" ]]; then
  : >"$FONT_ATLAS_ARTIFACT"
else
  rm -f "$FONT_ATLAS_ARTIFACT"
fi

if [[ -z "${PATHSPACE_LEGACY_WIDGET_BUILDERS:-}" ]]; then
  export PATHSPACE_LEGACY_WIDGET_BUILDERS="error"
fi
if [[ -z "${PATHSPACE_LEGACY_WIDGET_BUILDERS_REPORT:-}" ]]; then
  export PATHSPACE_LEGACY_WIDGET_BUILDERS_REPORT="$ROOT/build/prepush_legacy_builders.jsonl"
fi
LEGACY_REPORT_DIR="$(dirname "$PATHSPACE_LEGACY_WIDGET_BUILDERS_REPORT")"
mkdir -p "$LEGACY_REPORT_DIR"
: >"$PATHSPACE_LEGACY_WIDGET_BUILDERS_REPORT"

JOBS="${JOBS:-$(cpu_jobs)}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_TYPE_LOWER="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
case "$BUILD_TYPE_LOWER" in
  release|debug|relwithdebinfo|minsizerel)
    ;;
  *)
    warn "Unknown build type '$BUILD_TYPE'; defaulting to Release"
    BUILD_TYPE_LOWER="release"
    ;;
esac
BUILD_TYPE_FLAG="--$BUILD_TYPE_LOWER"

trap 'rc=$?; write_prepush_summary "$rc"' EXIT

CLANGXX_BIN="$(command -v clang++ || true)"
PATHSPACE_CMAKE_ARGS="${PATHSPACE_CMAKE_ARGS:-}"
PATHSPACE_CMAKE_ARGS+=" -DCMAKE_CXX_COMPILER=${CLANGXX_BIN}"
PATHSPACE_CMAKE_ARGS+=" -DCMAKE_OBJCXX_COMPILER=${CLANGXX_BIN}"

METAL_TEST_ARGS=()
METAL_TESTS_REQUESTED=0
BASE_PATHSPACE_CMAKE_ARGS="$PATHSPACE_CMAKE_ARGS"

say "Repository root: $ROOT"
say "Jobs: $JOBS  Build type: $BUILD_TYPE"

# 1) Clean build + test loop (loop=5) unless skipped
if [[ "${SKIP_LOOP_TESTS:-0}" != "1" ]]; then
  say "Building and running tests with loop=5"
  # scripts/compile.sh takes care of configure+build+tests
  # Prefer Apple clang for both C++ and ObjC++ to support ObjC headers/blocks
  run_with_hard_timeout "compile/test loop" PATHSPACE_CMAKE_ARGS="$PATHSPACE_CMAKE_ARGS" ./scripts/compile.sh --clean --test --loop=5 "$BUILD_TYPE_FLAG" --jobs "$JOBS"
  ok "Test loop completed successfully"
else
  warn "Skipping test loop (SKIP_LOOP_TESTS=1)"
fi

# Optional sanitizer passes
SANITIZER_RUNS=()
if [[ "${RUN_ASAN:-0}" == "1" ]]; then
  SANITIZER_RUNS+=("asan")
fi
if [[ "${RUN_TSAN:-0}" == "1" ]]; then
  SANITIZER_RUNS+=("tsan")
fi

if [[ ${#SANITIZER_RUNS[@]} -gt 0 ]]; then
  say "Running optional sanitizer passes: ${SANITIZER_RUNS[*]}"

  for sanitizer in "${SANITIZER_RUNS[@]}"; do
    upper_sanitizer="$(echo "$sanitizer" | tr '[:lower:]' '[:upper:]')"
    local_loop=""
    case "$sanitizer" in
      "asan") local_loop="${ASAN_LOOP:-}";;
      "tsan") local_loop="${TSAN_LOOP:-}";;
    esac
    sanitize_args=()
    if [[ "${SANITIZER_CLEAN:-0}" == "1" ]]; then
      sanitize_args+=("--clean")
    fi
    sanitize_args+=("--build-dir" "build-${sanitizer}")
    sanitize_args+=("--build-type" "${SANITIZER_BUILD_TYPE:-Debug}")
    sanitize_args+=("--jobs" "$JOBS")
    sanitize_args+=("--disable-metal-tests")
    sanitize_args+=("--${sanitizer}-test")
    if [[ -n "$local_loop" ]]; then
      sanitize_args+=("--loop=$local_loop")
    fi
    say "Running ${upper_sanitizer} sanitizer build/test pass"
    run_with_hard_timeout "${upper_sanitizer} sanitizer compile/test" PATHSPACE_CMAKE_ARGS="$BASE_PATHSPACE_CMAKE_ARGS" \
      ./scripts/compile.sh "${sanitize_args[@]}"
    ok "${upper_sanitizer} sanitizer pass succeeded"
  done
fi

warn "Skipping font atlas artifact guard (UI/atlas artifacts removed)."

warn "Skipping history savefile CLI roundtrip (harness removed)."

# Example app build/smoke test removed (minimal_button_example not required).

ok "Local pre-push checks passed"
exit 0

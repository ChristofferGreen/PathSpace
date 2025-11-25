#!/usr/bin/env bash
# Local pre-push helper
# Build, run tests in a loop, and smoke-test the devices_example app locally.
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
#   SKIP_PERF_GUARDRAIL=1   -> skip performance guardrail metrics check
#   RUN_ASAN=1              -> run an additional AddressSanitizer build/test pass
#   RUN_TSAN=1              -> run an additional ThreadSanitizer build/test pass
#   ASAN_LOOP=N             -> override ASan test loop iterations (default 1)
#   TSAN_LOOP=N             -> override TSan test loop iterations (default 1)
#   SANITIZER_CLEAN=1       -> force sanitized passes to clean their build directories before running
#   SANITIZER_BUILD_TYPE=TYPE -> override sanitized build type (default Debug)

set -euo pipefail

# ----- Utils -----

say()  { printf "\033[1;34m[pre-push]\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32m[pre-push]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[pre-push]\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31m[pre-push]\033[0m %s\n" "$*" >&2; }

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

# ----- Main -----

ROOT="$(repo_root)"
cd "$ROOT"

JOBS="${JOBS:-$(cpu_jobs)}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

CLANGXX_BIN="$(command -v clang++ || true)"
PATHSPACE_CMAKE_ARGS="${PATHSPACE_CMAKE_ARGS:-}"
PATHSPACE_CMAKE_ARGS+=" -DCMAKE_CXX_COMPILER=${CLANGXX_BIN}"
PATHSPACE_CMAKE_ARGS+=" -DCMAKE_OBJCXX_COMPILER=${CLANGXX_BIN}"

METAL_TEST_ARGS=()
if [[ "$(uname)" == "Darwin" ]] && [[ "${DISABLE_METAL_TESTS:-0}" != "1" ]]; then
  PATHSPACE_CMAKE_ARGS+=" -DPATHSPACE_UI_METAL=ON"
  METAL_TEST_ARGS+=(--enable-metal-tests)
fi
BASE_PATHSPACE_CMAKE_ARGS="$PATHSPACE_CMAKE_ARGS"

say "Repository root: $ROOT"
say "Jobs: $JOBS  Build type: $BUILD_TYPE"

# 1) Clean build + test loop (loop=5) unless skipped
if [[ "${SKIP_LOOP_TESTS:-0}" != "1" ]]; then
  say "Building and running tests with loop=5"
  # scripts/compile.sh takes care of configure+build+tests
  # Prefer Apple clang for both C++ and ObjC++ to support ObjC headers/blocks
  PATHSPACE_CMAKE_ARGS="$PATHSPACE_CMAKE_ARGS" ./scripts/compile.sh --clean --test --loop=5 --${BUILD_TYPE,,} --jobs "$JOBS" "${METAL_TEST_ARGS[@]}"
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
fi

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
  PATHSPACE_CMAKE_ARGS="$BASE_PATHSPACE_CMAKE_ARGS -DPATHSPACE_UI_METAL=OFF" \
    ./scripts/compile.sh "${sanitize_args[@]}"
  ok "${upper_sanitizer} sanitizer pass succeeded"
done

if [[ "${SKIP_HISTORY_CLI:-0}" != "1" ]]; then
  say "Running history savefile CLI roundtrip harness"
  if [[ ! -x ./build/pathspace_history_cli_roundtrip ]]; then
    err "Missing ./build/pathspace_history_cli_roundtrip (build step did not produce harness)"
    exit 1
  fi
  archive_stamp="$(date +"%Y%m%d-%H%M%S")"
  archive_dir="./build/test-logs/history_cli_roundtrip/pre-push_${archive_stamp}"
  mkdir -p "$archive_dir"
  if PATHSPACE_CLI_ROUNDTRIP_ARCHIVE_DIR="$archive_dir" ./build/pathspace_history_cli_roundtrip; then
    ok "History savefile CLI roundtrip succeeded"
    scripts/history_cli_roundtrip_ingest.py \
      --artifacts-root "build/test-logs" \
      --output "build/test-logs/history_cli_roundtrip/index.json" \
      --relative-base "${PWD}/build" \
      --html-output "build/test-logs/history_cli_roundtrip/dashboard.html" \
      --quiet || warn "history telemetry aggregation failed"
  else
    err "History savefile CLI roundtrip failed"
    exit 1
  fi
else
  warn "Skipping history savefile CLI roundtrip (SKIP_HISTORY_CLI=1)"
fi

# 2) Build the example app (non-sim) unless skipped
if [[ "${SKIP_EXAMPLE:-0}" != "1" ]]; then
  say "Configuring example app (devices_example)"
  # Configure example via CMake cache; respect PATHSPACE_CMAKE_ARGS and optional macOS backend flag
  cmake -S . -B build \
    -DBUILD_PATHSPACE_EXAMPLES=ON \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    ${PATHSPACE_CMAKE_ARGS:-}

  say "Building example app"
  cmake --build build -j "$JOBS" --target devices_example

  # 3) Smoke run the example briefly to ensure it starts (no simulation; expects real input)
  say "Running devices_example for a brief smoke test (3s)..."
  if run_with_timeout 3 ./build/devices_example; then
    ok "devices_example smoke test OK"
  else
    err "devices_example failed to start cleanly"
    exit 1
  fi
else
  warn "Skipping example app smoke test (SKIP_EXAMPLE=1)"
fi

if [[ "${SKIP_PERF_GUARDRAIL:-0}" != "1" ]]; then
  say "Running performance guardrail checks"
  if python3 ./scripts/perf_guardrail.py \
      --build-dir build \
      --build-type "$BUILD_TYPE" \
      --jobs "$JOBS" \
      --baseline docs/perf/performance_baseline.json \
      --history-dir build/perf/history \
      --print; then
    ok "Performance guardrail checks passed"
  else
    err "Performance guardrail failed"
    exit 1
  fi
else
  warn "Skipping performance guardrail (SKIP_PERF_GUARDRAIL=1)"
fi

ok "Local pre-push checks passed"
exit 0

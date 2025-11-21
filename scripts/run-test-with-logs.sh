#!/usr/bin/env bash
#
# Run a single test executable, capture its output, and preserve logs when it
# fails or times out. Intended to be shared by compile.sh and CTest entries so
# failures automatically surface diagnostics.
#
# Required arguments:
#   --label NAME        Logical test name used for log filenames.
#   --log-dir DIR       Directory where log files should be written.
#   --timeout SECS      Timeout in seconds (0 disables).
#   --env KEY=VALUE     Environment variables to pass (can repeat).
#   --iteration N       Optional loop iteration (1-based) for naming.
#   --iterations N      Optional total iteration count for naming.
#   -- command [...]    Command to execute (after `--`).
#
# Environment:
#   USE_CLI_TIMEOUT=1   Prefer GNU timeout if available.
#
# Exit codes mirror the wrapped command (124 on timeout).

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run-test-with-logs.sh [options] -- command [args...]
  --label NAME          Logical test name (required)
  --log-dir DIR         Directory to store log files (required)
  --timeout SECS        Timeout seconds (default 0 = no timeout)
  --env KEY=VALUE       Environment variable to pass to the test (repeatable)
  --iteration N         Loop iteration (for log naming)
  --iterations N        Loop iteration count (for log naming)
  --keep-success-log    Preserve the log even when the command succeeds
  -h, --help            Show this help
EOF
}

# ----------------------------
# Parse options
# ----------------------------
LABEL=""
LOG_DIR=""
TIMEOUT=0
ENV_VARS=()
ITERATION=""
ITERATIONS=""
KEEP_SUCCESS_LOG=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --label)
      shift || { usage >&2; exit 2; }
      LABEL="$1"
      ;;
    --log-dir)
      shift || { usage >&2; exit 2; }
      LOG_DIR="$1"
      ;;
    --timeout)
      shift || { usage >&2; exit 2; }
      TIMEOUT="$1"
      ;;
    --env)
      shift || { usage >&2; exit 2; }
      ENV_VARS+=("$1")
      ;;
    --iteration)
      shift || { usage >&2; exit 2; }
      ITERATION="$1"
      ;;
    --iterations)
      shift || { usage >&2; exit 2; }
      ITERATIONS="$1"
      ;;
    --keep-success-log)
      KEEP_SUCCESS_LOG=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ -z "${LABEL}" ]] || [[ -z "${LOG_DIR}" ]]; then
  echo "Missing required --label or --log-dir" >&2
  usage >&2
  exit 2
fi

if [[ $# -eq 0 ]]; then
  echo "Missing command to execute (expected after --)" >&2
  usage >&2
  exit 2
fi

if [[ -n "$TIMEOUT" ]]; then
  case "$TIMEOUT" in
    ''|*[!0-9]*)
      echo "--timeout must be an integer" >&2
      exit 2
      ;;
    *)
      if [[ "$TIMEOUT" -lt 0 ]]; then
        echo "--timeout must be >= 0" >&2
        exit 2
      fi
      ;;
  esac
else
  TIMEOUT=0
fi

mkdir -p "$LOG_DIR"

# Sanitise label for filenames.
SAFE_LABEL="${LABEL//[^A-Za-z0-9._-]/_}"
timestamp="$(date +"%Y%m%d-%H%M%S")"
if [[ -n "$ITERATION" ]] && [[ -n "$ITERATIONS" ]]; then
  SAFE_LABEL="${SAFE_LABEL}_loop${ITERATION}of${ITERATIONS}"
elif [[ -n "$ITERATION" ]]; then
  SAFE_LABEL="${SAFE_LABEL}_loop${ITERATION}"
fi

ARTIFACT_DIR="${LOG_DIR}/${SAFE_LABEL}_${timestamp}.artifacts"
mkdir -p "$ARTIFACT_DIR"
ENV_VARS+=("PATHSPACE_TEST_ARTIFACT_DIR=${ARTIFACT_DIR}")

TMP_TEMPLATE="${LOG_DIR}/${SAFE_LABEL}.XXXXXX.log"
if TMP_LOG="$(mktemp "${TMP_TEMPLATE}" 2>/dev/null)"; then
  :
else
  if command -v python3 >/dev/null 2>&1; then
    TMP_LOG="$(python3 - "$LOG_DIR" "$SAFE_LABEL" <<'PY'
import os
import random
import string
import sys

log_dir = sys.argv[1]
label = sys.argv[2]
alphabet = string.ascii_lowercase + string.ascii_uppercase + string.digits

for _ in range(1024):
    suffix = ''.join(random.choice(alphabet) for _ in range(8))
    candidate = os.path.join(log_dir, f"{label}.{suffix}.log")
    try:
        fd = os.open(candidate, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        continue
    else:
        os.close(fd)
        print(candidate)
        sys.exit(0)

print("ERROR: could not allocate temp log file", file=sys.stderr)
sys.exit(1)
PY
)"
    if [[ -z "${TMP_LOG}" || "${TMP_LOG}" == ERROR:* ]]; then
      echo "Failed to allocate temporary log file in ${LOG_DIR}" >&2
      exit 1
    fi
  else
    suffix="$(date +"%s")_${RANDOM}_${RANDOM}"
    TMP_LOG="${LOG_DIR}/${SAFE_LABEL}.${suffix}.log"
    if ! (set -o noclobber; : >"${TMP_LOG}") 2>/dev/null; then
      echo "Failed to allocate temporary log file in ${LOG_DIR}" >&2
      exit 1
    fi
  fi
fi

FINAL_LOG="${LOG_DIR}/${SAFE_LABEL}_${timestamp}.log"

# Build command array with optional env.
CMD=("$@")
BASE_CMD=()
if [[ ${#ENV_VARS[@]} -gt 0 ]]; then
  BASE_CMD=(env "${ENV_VARS[@]}" "${CMD[@]}")
else
  BASE_CMD=("${CMD[@]}")
fi

TIMEOUT_CMD=""
if [[ "${USE_CLI_TIMEOUT:-0}" == "1" ]] && command -v timeout >/dev/null 2>&1; then
  TIMEOUT_CMD="timeout"
fi

run_with_manual_timeout() {
  local rc=0
  "${BASE_CMD[@]}" >"$TMP_LOG" 2>&1 &
  local pid=$!
  local elapsed=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    elapsed=$((elapsed + 1))
    if [[ "$TIMEOUT" -gt 0 && "$elapsed" -ge "$TIMEOUT" ]]; then
      echo "Timeout (${TIMEOUT}s) reached; terminating ${LABEL}" >>"$TMP_LOG"
      kill -TERM "$pid" 2>/dev/null || true
      sleep 1
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      return 124
    fi
  done
  wait "$pid" && rc=$? || rc=$?
  return "$rc"
}

append_log_manifest() {
  local status="$1"
  local log_path="$2"
  if [[ -z "${PATHSPACE_TEST_LOG_MANIFEST:-}" ]]; then
    return
  fi
  if [[ -z "$log_path" ]]; then
    return
  fi
  local iteration_label="${ITERATION:-}"
  if [[ -z "$iteration_label" ]]; then
    iteration_label="-"
  fi
  {
    printf '%s\t%s\t%s\t%s\t%s\n' \
      "$LABEL" \
      "$iteration_label" \
      "$status" \
      "$log_path" \
      "$ARTIFACT_DIR"
  } >>"$PATHSPACE_TEST_LOG_MANIFEST" || true
}

append_exit_banner() {
  local log_path="$1"
  local rc="$2"
  local timestamp
  timestamp="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  local detail=""
  if [[ "$rc" -eq 124 ]]; then
    detail="timeout after ${TIMEOUT}s (rc=${rc})"
  elif [[ "$rc" -ge 128 ]]; then
    local signal=$((rc - 128))
    detail="signal ${signal} (rc=${rc})"
  else
    detail="exit code ${rc}"
  fi
  {
    echo ""
    printf "[test-runner] EXIT %s at %s\n" "$detail" "$timestamp"
  } >>"$log_path"
}

RC=0
if [[ "$TIMEOUT" -gt 0 && -n "$TIMEOUT_CMD" ]]; then
  if ! "${TIMEOUT_CMD}" "${TIMEOUT}s" "${BASE_CMD[@]}" >"$TMP_LOG" 2>&1; then
    RC=$?
  fi
else
  run_with_manual_timeout || RC=$?
fi

if [[ "$RC" -eq 0 ]]; then
  if [[ "$KEEP_SUCCESS_LOG" -eq 1 ]]; then
    mv "$TMP_LOG" "$FINAL_LOG"
    append_log_manifest "success" "$FINAL_LOG"
    echo "[test-runner] ${LABEL} succeeded; log saved to: ${FINAL_LOG}" >&2
  else
    rm -f "$TMP_LOG"
  fi
  exit 0
fi

mv "$TMP_LOG" "$FINAL_LOG"
append_exit_banner "$FINAL_LOG" "$RC"
append_log_manifest "failure" "$FINAL_LOG"
echo "[test-runner] ${LABEL} failed (exit ${RC}). Log saved to: ${FINAL_LOG}" >&2
if command -v tail >/dev/null 2>&1; then
  echo "[test-runner] Last 40 log lines:" >&2
  tail -n 40 "$FINAL_LOG" >&2
fi

exit "$RC"

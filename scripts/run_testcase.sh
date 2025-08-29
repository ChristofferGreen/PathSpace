#!/usr/bin/env bash
#
# Run the project's tests with convenient defaults and macOS/bash-3.2 compatibility.
#
# Features:
# - Fuzzy substring test selection (-t/--test) using --list-test-cases
# - Optional subcase filter (-s/--subcase)
# - Run all matches (-a/--all) or interactively choose one when TTY
# - Per-run timeout (-T/--timeout SECS or PATHSPACE_TEST_TIMEOUT env)
# - Loop runs (--loop[=N]) for flakiness checks
# - Verbose (-v) and dry-run (-n) modes
#
# Usage:
#   ./scripts/run_testcase.sh                       # run all tests (default)
#   ./scripts/run_testcase.sh -t Multithreading    # run test cases containing "Multithreading"
#   ./scripts/run_testcase.sh -t Multithreading -s "Dining Philosophers"
#   ./scripts/run_testcase.sh -a -t PathIO         # run all matches
#   ./scripts/run_testcase.sh -T 120               # set timeout to 120 seconds
#
set -uo pipefail

# ---- Defaults ----
: "${PATHSPACE_TEST_TIMEOUT:=60}"                 # Default timeout seconds via env (overridden by -T/--timeout)
TEST_TIMEOUT_SECS="${PATHSPACE_TEST_TIMEOUT}"
EXECUTABLE="./build/tests/PathSpaceTests"
LOG_FILE="./log.txt"
TEST_QUERY=""
SUBCASE=""
VERBOSE=0
DRY_RUN=0
RUN_ALL_MATCHES=0
LOOP=0  # 0 = run once; --loop (no value) => 15; --loop=N => N

# ---- Helpers ----
die() { echo "$*" >&2; exit 2; }
is_tty() { [[ -t 0 && -t 1 ]]; }
timestamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

print_help() {
  cat <<EOF
Usage: $0 [options]

Options:
  -e, --executable PATH   Path to the test executable (default: $EXECUTABLE)
  -t, --test NAME         Fuzzy test case name (substring, case-insensitive).
                          If omitted, runs the entire test executable.
  -s, --sub NAME          Subcase name to pass to the test executable.
  -l, --log PATH          Path to write combined stdout/stderr log (default: $LOG_FILE)
  -a, --all               When fuzzy query matches multiple cases, run all matches.
  -T, --timeout SECS      Per-invocation timeout in seconds (default: ${TEST_TIMEOUT_SECS})
  -v, --verbose           Print commands that will be run.
  -n, --dry-run           Show what would be run but don't execute.
      --loop[=N]          Repeat the selected run N times (default: 15).
  -h, --help              Show this help and exit.

Environment:
  PATHSPACE_TEST_TIMEOUT  Default timeout in seconds if --timeout not provided (current: ${TEST_TIMEOUT_SECS})
EOF
}

# Wrap a command with a timeout; returns the command's exit code or 124 on timeout.
# Usage: run_command cmd arg1 arg2 ...
run_command() {
  local rc=0
  local cmd=( "$@" )

  if [[ $VERBOSE -eq 1 || $DRY_RUN -eq 1 ]]; then
    echo "Command: ${cmd[*]}"
    echo "Timeout: ${TEST_TIMEOUT_SECS}s"
    echo "Log file: $LOG_FILE"
  fi

  if [[ $DRY_RUN -eq 1 ]]; then
    return 0
  fi

  {
    echo "=== RUNNING: $(timestamp) ==="
    echo "Command: ${cmd[*]}"
    echo

    if command -v timeout >/dev/null 2>&1; then
      timeout "${TEST_TIMEOUT_SECS}s" "${cmd[@]}"
      rc=$?
    else
      # Manual timeout fallback (portable)
      "${cmd[@]}" &
      local pid=$!
      local waited=0
      while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        waited=$((waited+1))
        if [[ $waited -ge $TEST_TIMEOUT_SECS ]]; then
          echo "Error: tests timed out after ${TEST_TIMEOUT_SECS} seconds"
          kill -TERM "$pid" 2>/dev/null || true
          sleep 1
          kill -KILL "$pid" 2>/dev/null || true
          wait "$pid" 2>/dev/null
          rc=124
          break
        fi
      done
      if [[ -z "${rc:-}" || $rc -eq 0 ]]; then
        wait "$pid"
        rc=$?
      fi
    fi

    echo
    echo "=== EXIT CODE: $rc ==="
    echo "=== FINISHED: $(timestamp) ==="
  } >> "$LOG_FILE" 2>&1

  if [[ $rc -eq 124 ]]; then
    echo "Error: test run timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE" >&2
  fi

  return $rc
}

# ---- Arg parsing ----
while [[ $# -gt 0 ]]; do
  case "$1" in
    -e|--executable)
      shift; EXECUTABLE="${1:-}"; [[ -z "$EXECUTABLE" ]] && die "Error: --executable requires a path"
      ;;
    -t|--test|--test-case)
      shift; TEST_QUERY="${1:-}"; [[ -z "$TEST_QUERY" ]] && die "Error: --test requires a name"
      ;;
    -s|--sub|--subcase)
      shift; SUBCASE="${1:-}"; [[ -z "$SUBCASE" ]] && die "Error: --subcase requires a name"
      ;;
    -l|--log)
      shift; LOG_FILE="${1:-}"; [[ -z "$LOG_FILE" ]] && die "Error: --log requires a path"
      ;;
    -a|--all)
      RUN_ALL_MATCHES=1
      ;;
    -T|--timeout)
      shift
      local_to="${1:-}"
      case "$local_to" in
        ''|*[!0-9]*)
          die "Error: --timeout requires a positive integer"
          ;;
        *)
          if [[ "$local_to" -lt 1 ]]; then die "Error: --timeout must be >= 1"; fi
          TEST_TIMEOUT_SECS="$local_to"
          PATHSPACE_TEST_TIMEOUT="$TEST_TIMEOUT_SECS"
          ;;
      esac
      ;;
    -v|--verbose)
      VERBOSE=1
      ;;
    -n|--dry-run|--no-run)
      DRY_RUN=1
      ;;
    --loop)
      LOOP=15
      ;;
    --loop=*)
      LOOP="${1#*=}"
      case "$LOOP" in
        ''|*[!0-9]*)
          die "Error: --loop requires a positive integer"
          ;;
        *)
          if [[ "$LOOP" -lt 1 ]]; then die "Error: --loop must be >= 1"; fi
          ;;
      esac
      ;;
    -h|--help)
      print_help; exit 0
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
  shift
done

# ---- Validate executable & prepare log ----
if [[ ! -x "$EXECUTABLE" ]]; then
  die "Error: executable not found or not executable: $EXECUTABLE
Build the tests first (e.g., from project root: cmake .. && make) or pass -e / --executable."
fi

: > "$LOG_FILE" || die "Unable to write to log file: $LOG_FILE"
export PATHSPACE_TEST_TIMEOUT="$TEST_TIMEOUT_SECS"

# ---- No query => run entire executable ----
if [[ -z "$TEST_QUERY" ]]; then
  CMD=( "$EXECUTABLE" )
  if [[ -n "$SUBCASE" ]]; then
    CMD+=( --subcase="$SUBCASE" )
  fi

  if [[ $VERBOSE -eq 1 ]]; then
    echo "Will run: ${CMD[*]}"
    echo "Log file: $LOG_FILE"
    echo "Timeout: ${TEST_TIMEOUT_SECS}s"
  fi

  if [[ "$LOOP" -gt 0 ]]; then
    echo "Looping entire test execution $LOOP time(s)..."
    i=1
    while [[ $i -le $LOOP ]]; do
      echo "Iteration $i/$LOOP"
      run_command "${CMD[@]}"
      rc=$?
      if [[ $rc -eq 0 ]]; then
        echo "Iteration $i passed."
      elif [[ $rc -eq 124 ]]; then
        echo "Iteration $i timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
        exit 124
      else
        echo "Iteration $i failed with exit code $rc. See log: $LOG_FILE"
        exit $rc
      fi
      i=$((i+1))
    done
    echo "All $LOOP iterations passed."
    exit 0
  else
    run_command "${CMD[@]}"
    EXIT_CODE=$?
    if [[ $EXIT_CODE -eq 0 ]]; then
      echo "Run finished (exit code 0). Log: $LOG_FILE"
    elif [[ $EXIT_CODE -eq 124 ]]; then
      echo "Run timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
    else
      echo "Run finished with exit code $EXIT_CODE. See log: $LOG_FILE"
    fi
    exit $EXIT_CODE
  fi
fi

# ---- Query mode: fuzzy match test cases ----
ALL_TESTS_RAW="$("$EXECUTABLE" --list-test-cases 2>&1)" || true

ALL_TESTS=()
while IFS= read -r line; do
  # Trim leading/trailing whitespace
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  if [[ -n "$line" && "${line:0:1}" != "[" ]]; then
    ALL_TESTS+=("$line")
  fi
done < <(printf '%s\n' "$ALL_TESTS_RAW")

if [[ ${#ALL_TESTS[@]} -eq 0 ]]; then
  echo "Warning: no test cases found from --list-test-cases. Falling back to running the executable directly."
  CMD=( "$EXECUTABLE" --test-case="$TEST_QUERY" )
  if [[ -n "$SUBCASE" ]]; then
    CMD+=( --subcase="$SUBCASE" )
  fi
  run_command "${CMD[@]}"
  exit $?
fi

MATCHES=()
for t in "${ALL_TESTS[@]}"; do
  if printf '%s\n' "$t" | grep -qiF -- "$TEST_QUERY"; then
    MATCHES+=("$t")
  fi
done

if [[ ${#MATCHES[@]} -eq 0 ]]; then
  echo "No matches for query: '$TEST_QUERY' (as test-case). Trying to run the executable using --subcase='$TEST_QUERY'..."
  CMD=( "$EXECUTABLE" --subcase="$TEST_QUERY" )
  if [[ $VERBOSE -eq 1 ]]; then
    echo "Attempting: ${CMD[*]}"
    echo "Log file: $LOG_FILE"
    echo "Timeout: ${TEST_TIMEOUT_SECS}s"
  fi

  if [[ "$LOOP" -gt 0 ]]; then
    echo "Looping subcase run $LOOP time(s)..."
    i=1
    while [[ $i -le $LOOP ]]; do
      echo "Iteration $i/$LOOP"
      run_command "${CMD[@]}"
      rc=$?
      if [[ $rc -eq 0 ]]; then
        echo "Iteration $i passed."
      elif [[ $rc -eq 124 ]]; then
        echo "Iteration $i timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
        exit 124
      else
        echo "Iteration $i failed with exit code $rc. See log: $LOG_FILE"
        exit $rc
      fi
      i=$((i+1))
    done
    echo "All $LOOP iterations passed."
    exit 0
  else
    run_command "${CMD[@]}"
    rc=$?
    if [[ $rc -eq 0 ]]; then
      echo "Run succeeded using --subcase filter with query '$TEST_QUERY'. See log: $LOG_FILE"
      exit 0
    elif [[ $rc -eq 124 ]]; then
      echo "Run timed out after ${TEST_TIMEOUT_SECS} seconds using --subcase filter with query '$TEST_QUERY'. See log: $LOG_FILE"
      exit 124
    else
      echo "Run failed with exit code $rc using --subcase filter with query '$TEST_QUERY'. See log: $LOG_FILE"
      exit $rc
    fi
  fi
fi

# If single match, run it directly
if [[ ${#MATCHES[@]} -eq 1 ]]; then
  SELECTED="${MATCHES[0]}"
  echo "Single match: '$SELECTED' -> running"
  CMD=( "$EXECUTABLE" --test-case="$SELECTED" )
  if [[ -n "$SUBCASE" ]]; then
    CMD+=( --subcase="$SUBCASE" )
  fi

  if [[ "$LOOP" -gt 0 ]]; then
    echo "Looping test case '$SELECTED' $LOOP time(s)..."
    i=1
    while [[ $i -le $LOOP ]]; do
      echo "Iteration $i/$LOOP"
      run_command "${CMD[@]}"
      rc=$?
      if [[ $rc -eq 0 ]]; then
        echo "Iteration $i passed."
      elif [[ $rc -eq 124 ]]; then
        echo "Iteration $i timed out after ${TEST_TIMEOUT_SECS} seconds. Log: $LOG_FILE"
        exit 124
      else
        echo "Iteration $i failed (exit code $rc). Log: $LOG_FILE"
        exit $rc
      fi
      i=$((i+1))
    done
    echo "All $LOOP iterations passed."
    exit 0
  else
    run_command "${CMD[@]}"
    rc=$?
    if [[ $rc -eq 0 ]]; then
      echo "Test case '$SELECTED' passed (exit code 0). Log: $LOG_FILE"
    elif [[ $rc -eq 124 ]]; then
      echo "Test case '$SELECTED' timed out after ${TEST_TIMEOUT_SECS} seconds. Log: $LOG_FILE"
    else
      echo "Test case '$SELECTED' failed (exit code $rc). Log: $LOG_FILE"
    fi
    exit $rc
  fi
fi

# Multiple matches
echo "Multiple matches (${#MATCHES[@]}) for query '$TEST_QUERY':"
i=1
for t in "${MATCHES[@]}"; do
  printf "  %2d) %s\n" "$i" "$t"
  i=$((i+1))
done

if [[ $RUN_ALL_MATCHES -eq 1 ]]; then
  echo "--all specified: running all ${#MATCHES[@]} matches sequentially."
  EXIT_CODE=0
  for t in "${MATCHES[@]}"; do
    echo "==== Running test-case: '$t' ===="
    CMD=( "$EXECUTABLE" --test-case="$t" )
    if [[ -n "$SUBCASE" ]]; then
      CMD+=( --subcase="$SUBCASE" )
    fi
    run_command "${CMD[@]}"
    rc=$?
    if [[ $rc -ne 0 ]]; then
      if [[ $rc -eq 124 ]]; then
        echo "Test case '$t' timed out after ${TEST_TIMEOUT_SECS} seconds (continuing)."
      else
        echo "Test case '$t' failed with exit code $rc (continuing)."
      fi
      EXIT_CODE=$rc
    fi
  done
  if [[ $EXIT_CODE -eq 0 ]]; then
    echo "All matched test cases completed successfully. Log: $LOG_FILE"
  else
    echo "One or more matched test cases failed (last non-zero exit code: $EXIT_CODE). See log: $LOG_FILE"
  fi
  exit $EXIT_CODE
fi

if is_tty; then
  echo "Enter number of test to run (1-${#MATCHES[@]}) or 'a' to run all. Default: 1"
  printf "> "
  read -r selection || selection=""
  if [[ -z "$selection" ]]; then
    selection=1
  fi

  if [[ "$selection" == "a" || "$selection" == "A" ]]; then
    echo "Running all matches..."
    EXIT_CODE=0
    for t in "${MATCHES[@]}"; do
      CMD=( "$EXECUTABLE" --test-case="$t" )
      if [[ -n "$SUBCASE" ]]; then
        CMD+=( --subcase="$SUBCASE" )
      fi

      if [[ "$LOOP" -gt 0 ]]; then
        echo "Looping test case '$t' $LOOP time(s)..."
        i=1
        while [[ $i -le $LOOP ]]; do
          echo "Iteration $i/$LOOP"
          run_command "${CMD[@]}"
          rc=$?
          if [[ $rc -ne 0 ]]; then
            EXIT_CODE=$rc
            echo "Iteration $i failed (exit code $rc). Log: $LOG_FILE"
            exit $EXIT_CODE
          fi
          i=$((i+1))
        done
        echo "All $LOOP iterations for '$t' passed."
      else
        run_command "${CMD[@]}"
        rc=$?
        if [[ $rc -ne 0 ]]; then
          EXIT_CODE=$rc
        fi
      fi
    done
    exit $EXIT_CODE
  fi

  if ! [[ "$selection" =~ ^[0-9]+$ ]]; then
    echo "Invalid selection: $selection" >&2
    exit 5
  fi

  sel_index=$((selection - 1))
  if (( sel_index < 0 || sel_index >= ${#MATCHES[@]} )); then
    echo "Selection out of range." >&2
    exit 6
  fi

  SELECTED="${MATCHES[$sel_index]}"
  echo "Running selected: '$SELECTED'"
  CMD=( "$EXECUTABLE" --test-case="$SELECTED" )
  if [[ -n "$SUBCASE" ]]; then
    CMD+=( --subcase="$SUBCASE" )
  fi

  if [[ "$LOOP" -gt 0 ]]; then
    echo "Looping selected test case $LOOP time(s)..."
    i=1
    while [[ $i -le $LOOP ]]; do
      echo "Iteration $i/$LOOP"
      run_command "${CMD[@]}"
      rc=$?
      if [[ $rc -ne 0 ]]; then
        if [[ $rc -eq 124 ]]; then
          echo "Iteration $i timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
        else
          echo "Iteration $i failed with exit code $rc. See log: $LOG_FILE"
        fi
        exit $rc
      fi
      i=$((i+1))
    done
    echo "All $LOOP iterations passed."
    exit 0
  else
    run_command "${CMD[@]}"
    rc=$?
    if [[ $rc -eq 124 ]]; then
      echo "Selected test case timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
    fi
    exit $rc
  fi
else
  # Non-interactive: run the first match
  SELECTED="${MATCHES[0]}"
  echo "Non-interactive session: running first match: '$SELECTED'"
  CMD=( "$EXECUTABLE" --test-case="$SELECTED" )
  if [[ -n "$SUBCASE" ]]; then
    CMD+=( --subcase="$SUBCASE" )
  fi

  if [[ "$LOOP" -gt 0 ]]; then
    echo "Looping first matched test case '$SELECTED' $LOOP time(s)..."
    i=1
    while [[ $i -le $LOOP ]]; do
      echo "Iteration $i/$LOOP"
      run_command "${CMD[@]}"
      rc=$?
      if [[ $rc -ne 0 ]]; then
        if [[ $rc -eq 124 ]]; then
          echo "Iteration $i timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
        else
          echo "Iteration $i failed with exit code $rc. See log: $LOG_FILE"
        fi
        exit $rc
      fi
      i=$((i+1))
    done
    echo "All $LOOP iterations passed."
    exit 0
  else
    run_command "${CMD[@]}"
    rc=$?
    if [[ $rc -eq 124 ]]; then
      echo "Test case '$SELECTED' timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
    fi
    exit $rc
  fi
fi

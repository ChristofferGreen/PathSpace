#!/usr/bin/env bash
#
# Run the project's tests with convenient defaults and macOS/bash-3.2 compatibility.
# - By default (no -t/--test provided) runs the entire test executable (all tests).
# - If a test case name is provided, performs case-insensitive fuzzy substring
#   matching against the list returned by the executable's --list-test-cases.
#
# Compatibility fixes included:
# - Avoids `mapfile`/`readarray` (not present in macOS's Bash 3.2).
# - Uses a portable date format (compatible with BSD `date`).
# - Fixes selection indexing (proper array indexing).
#
# Usage:
#   ./scripts/run_testcase.sh                # run all tests (default)
#   ./scripts/run_testcase.sh -t Dining     # fuzzy-match test cases containing "Dining"
#   ./scripts/run_testcase.sh -t Dining -a  # run all matches
#   ./scripts/run_testcase.sh -t Dining -s "Dining Philosophers" # pass subcase
#
set -uo pipefail
# Default per-run timeout (seconds)
TEST_TIMEOUT_SECS=60

# Defaults
EXECUTABLE="./build/tests/PathSpaceTests"
LOG_FILE="./log.txt"
TEST_QUERY=""
SUBCASE=""
VERBOSE=0
DRY_RUN=0
RUN_ALL_MATCHES=0
LOOP=0

print_help() {
    cat <<EOF
Usage: $0 [options]

Options:
  -e, --executable PATH   Path to the test executable (default: $EXECUTABLE)
  -t, --test NAME         Fuzzy test case name (substring, case-insensitive).
                          If omitted, the script runs the entire test executable.
  -s, --sub NAME          Subcase name to pass to the test executable.
  -l, --log PATH          Path to write combined stdout/stderr log (default: $LOG_FILE)
  -a, --all               When a fuzzy test query matches multiple cases,
                          run all matches instead of prompting.
  -v, --verbose           Print commands that will be run.
  -n, --dry-run           Show what would be run but don't execute.
      --loop[=N]          Repeat the selected run N times (default: 15). Implies fail-fast on first error/timeout.
  -h, --help              Show this help and exit.
EOF
}

# Parse args (simple)
while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--executable)
            shift
            EXECUTABLE="$1"
            ;;
        -t|--test|--test-case)
            shift
            TEST_QUERY="$1"
            ;;
        -s|--sub|--subcase)
            shift
            SUBCASE="$1"
            ;;
        -l|--log)
            shift
            LOG_FILE="$1"
            ;;
        -a|--all)
            RUN_ALL_MATCHES=1
            ;;
        -v|--verbose)
            VERBOSE=1
            ;;
        -n|--dry-run|--no-run)
            DRY_RUN=1
            ;;
        -h|--help)
            print_help
            exit 0
            ;;
        --loop)
            LOOP=15
            ;;
        --loop=*)
            LOOP="${1#*=}"
            case "$LOOP" in
              ''|*[!0-9]*)
                echo "Error: --loop requires a positive integer" >&2
                exit 2
                ;;
              *)
                if [[ "$LOOP" -lt 1 ]]; then
                  echo "Error: --loop must be >= 1" >&2
                  exit 2
                fi
                ;;
            esac
            ;;
        *)
            echo "Unknown argument: $1" >&2
            print_help
            exit 2
            ;;
    esac
    shift
done

# Helpers
die() { echo "$*" >&2; exit 1; }
is_tty() { [[ -t 0 && -t 1 ]]; }

# Validate executable
if [[ ! -x "$EXECUTABLE" ]]; then
    die "Error: executable not found or not executable: $EXECUTABLE
Build the tests first (e.g. from project root: cmake .. && make) or pass -e / --executable."
fi

# Prepare log (create/truncate)
: > "$LOG_FILE" || die "Unable to write to log file: $LOG_FILE"

# Portable date function (works on macOS and Linux)
timestamp() {
    # Use UTC ISO8601 which works on BSD and GNU date
    date -u +"%Y-%m-%dT%H:%M:%SZ"
}

run_command() {
    # run_command "$@" - runs the command, appends output to the log, returns rc
    local cmd
    cmd=( "$@" )
    if [[ $VERBOSE -eq 1 || $DRY_RUN -eq 1 ]]; then
        echo "Command: ${cmd[*]}"
    fi
    if [[ $DRY_RUN -eq 1 ]]; then
        return 0
    fi

    {
        echo "=== RUNNING: $(timestamp) ==="
        echo "Command: ${cmd[*]}"
        echo
        # Prefer GNU/BSD timeout if available, otherwise emulate a timeout
        if command -v timeout >/dev/null 2>&1; then
            timeout "${TEST_TIMEOUT_SECS}s" "${cmd[@]}"
            rc=$?
        else
            # Fallback manual timeout mechanism
            "${cmd[@]}" &
            pid=$!
            SECS=0
            while kill -0 "$pid" 2>/dev/null; do
                sleep 1
                SECS=$((SECS+1))
                if [[ $SECS -ge $TEST_TIMEOUT_SECS ]]; then
                    echo "Error: tests timed out after ${TEST_TIMEOUT_SECS} seconds"
                    kill -TERM "$pid" 2>/dev/null || true
                    # give it a moment to terminate gracefully
                    sleep 1
                    kill -KILL "$pid" 2>/dev/null || true
                    wait "$pid" 2>/dev/null
                    rc=124
                    break
                fi
            done
            if [[ -z "${rc:-}" ]]; then
                wait "$pid"
                rc=$?
            fi
        fi
        echo
        echo "=== EXIT CODE: $rc ==="
        echo "=== FINISHED: $(timestamp) ==="
    } >> "$LOG_FILE" 2>&1

    # If timed out with rc 124, surface a clear message and fail
    if [[ ${rc:-0} -eq 124 ]]; then
        echo "Error: test run timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE" >&2
    fi

    return ${rc:-0}
}

# If no query provided, run the entire test executable
if [[ -z "$TEST_QUERY" ]]; then
    echo "No test case specified: running entire test executable (default)."
    CMD=( "$EXECUTABLE" )
    if [[ -n "$SUBCASE" ]]; then
        CMD+=( --subcase="$SUBCASE" )
    fi

    if [[ $VERBOSE -eq 1 ]]; then
        echo "Will run: ${CMD[*]}"
        echo "Log file: $LOG_FILE"
    fi

    if [[ "$LOOP" -gt 0 ]]; then
        echo "Looping entire test execution $LOOP time(s)..."
        for i in $(seq 1 "$LOOP"); do
            echo "Iteration $i/$LOOP"
            run_command "${CMD[@]}"
            EXIT_CODE=$?
            if [[ $EXIT_CODE -eq 0 ]]; then
                echo "Iteration $i passed."
            elif [[ $EXIT_CODE -eq 124 ]]; then
                echo "Iteration $i timed out after ${TEST_TIMEOUT_SECS} seconds. See log: $LOG_FILE"
                exit 124
            else
                echo "Iteration $i failed with exit code $EXIT_CODE. See log: $LOG_FILE"
                exit $EXIT_CODE
            fi
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

# Otherwise, try to list test cases and fuzzy-match
ALL_TESTS_RAW="$("$EXECUTABLE" --list-test-cases 2>&1)" || true

# Extract test case lines: drop lines starting with '[' and blank lines.
# Then trim leading/trailing whitespace. Use process substitution and read into array
ALL_TESTS=()
while IFS= read -r line; do
    # Trim leading/trailing whitespace (portable)
    # using sed would be another external call, but keep it simple:
    # remove leading spaces/tabs
    line="${line#"${line%%[![:space:]]*}"}"
    # remove trailing spaces/tabs
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

# Perform case-insensitive substring match
MATCHES=()
for t in "${ALL_TESTS[@]}"; do
    if printf '%s\n' "$t" | grep -qiF -- "$TEST_QUERY"; then
        MATCHES+=("$t")
    fi
done

if [[ ${#MATCHES[@]} -eq 0 ]]; then
    echo "No matches for query: '$TEST_QUERY' (as test-case). Trying to run the executable using --subcase='$TEST_QUERY'..."
    # doctest supports --subcase to filter subcases across test cases.
    # Try running the executable with the query as a subcase filter before giving up.
    CMD=( "$EXECUTABLE" --subcase="$TEST_QUERY" )
    if [[ $VERBOSE -eq 1 ]]; then
        echo "Attempting: ${CMD[*]}"
        echo "Log file: $LOG_FILE"
    fi
    if [[ "$LOOP" -gt 0 ]]; then
        echo "Looping subcase run $LOOP time(s)..."
        for i in $(seq 1 "$LOOP"); do
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
        fi
    fi

    # If that also failed, fall back to listing available test cases for the user.
    echo "No matches for query: '$TEST_QUERY'"
    echo "Available test cases (first 30 shown):"
    local_count=0
    for t in "${ALL_TESTS[@]}"; do
        printf '  %s\n' "$t"
        ((local_count++))
        [[ $local_count -ge 30 ]] && break
    done
    exit 4
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
        for i in $(seq 1 "$LOOP"); do
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

# Multiple matches: list them
echo "Multiple matches (${#MATCHES[@]}) for query '$TEST_QUERY':"
i=1
for t in "${MATCHES[@]}"; do
    printf "  %2d) %s\n" "$i" "$t"
    ((i++))
done

# If --all specified, run all matches sequentially
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
                echo "Test case '$t' timed out after 120 seconds (continuing)."
            else
                echo "Test case '$t' failed with exit code $rc (continuing)."
            }
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

# Interactive selection if possible
if is_tty; then
    echo "Enter number of test to run (1-${#MATCHES[@]}) or 'a' to run all. Default: 1"
    printf "> "
    read -r selection
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
                for i in $(seq 1 "$LOOP"); do
                    echo "Iteration $i/$LOOP"
                    run_command "${CMD[@]}"
                    rc=$?
                    if [[ $rc -ne 0 ]]; then
                        EXIT_CODE=$rc
                        echo "Iteration $i failed (exit code $rc). Log: $LOG_FILE"
                        exit $EXIT_CODE
                    fi
                done
                echo "All $LOOP iterations for '$t' passed."
            else
                run_command "${CMD[@]}"
                rc=$?
                if [[ $rc -ne 0 ]]; then
                    EXIT_CODE=$rc
                }
            fi
        done
        exit $EXIT_CODE
    fi

    if ! [[ "$selection" =~ ^[0-9]+$ ]]; then
        echo "Invalid selection: $selection" >&2
        exit 5
    fi

    # convert to zero-based index (bash arithmetic)
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
        for i in $(seq 1 "$LOOP"); do
            echo "Iteration $i/$LOOP"
            run_command "${CMD[@]}"
            rc=$?
            if [[ $rc -ne 0 ]]; then
                if [[ $rc -eq 124 ]]; then
                    echo "Iteration $i timed out after 120 seconds. See log: $LOG_FILE"
                else
                    echo "Iteration $i failed with exit code $rc. See log: $LOG_FILE"
                fi
                exit $rc
            fi
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
        for i in $(seq 1 "$LOOP"); do
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

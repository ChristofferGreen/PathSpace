#!/bin/bash

# Exit on error. Append "|| true" if you expect an error.
set -e

# Exit on unset variables.
set -u

# Exit if any command in a pipeline fails.
set -o pipefail

# Configuration with defaults - adjusted for running from project root
VALGRIND_EXEC="${VALGRIND_EXEC:-valgrind}"
SUPPRESSIONS_FILE="${SUPPRESSIONS_FILE:-$(pwd)/scripts/suppressions.txt}"
OUTPUT_DIR="${OUTPUT_DIR:-/mnt/shared}"
TEST_EXECUTABLE="${TEST_EXECUTABLE:-$(pwd)/build/tests/PathSpaceTests}"

# Cleanup function
cleanup() {
    [ -f "./valgrind.txt" ] && rm "./valgrind.txt"
}
trap cleanup EXIT

# Check for --save and --suppress flags and remove them from args
save_output=false
use_suppressions=false
args=()
for arg in "$@"; do
    if [ "$arg" == "--save" ]; then
        save_output=true
    elif [ "$arg" == "--suppress" ]; then
        use_suppressions=true
    else
        args+=("$arg")
    fi
done

# Verify the executable exists
if [ ! -x "$TEST_EXECUTABLE" ]; then
    echo "Error: Cannot find or execute $TEST_EXECUTABLE"
    exit 1
fi

# Verify suppressions file if needed
if [ "$use_suppressions" = true ] && [ ! -f "$SUPPRESSIONS_FILE" ]; then
    echo "Error: Cannot find suppressions file $SUPPRESSIONS_FILE"
    exit 1
fi

# Set file descriptor limit
echo "Setting file descriptor limit to 1024..."
ulimit -n 1024

# Build the command
valgrind_cmd=(
    "$VALGRIND_EXEC"
    "--tool=helgrind"
)

if [ "$use_suppressions" = true ]; then
    valgrind_cmd+=("--suppressions=$SUPPRESSIONS_FILE")
fi

valgrind_cmd+=("$TEST_EXECUTABLE")

# Add test case and subcase if provided
if [ ${#args[@]} -eq 1 ]; then
    valgrind_cmd+=("--test-case=${args[0]}")
elif [ ${#args[@]} -eq 2 ]; then
    valgrind_cmd+=("--test-case=${args[0]}" "--subcase=${args[1]}")
fi

# Print the command about to be executed
echo "Executing:"
echo "${valgrind_cmd[@]}"
echo

# Execute with output handling
if [ "$save_output" = true ]; then
    echo "Running with output saved to valgrind.txt..."
    # Use tee to see output and save it
    if ! "${valgrind_cmd[@]}" 2>&1 | tee valgrind.txt; then
        echo "Error: Valgrind command failed"
        exit 1
    fi
    
    echo "Copying to $OUTPUT_DIR..."
    if ! sudo cp valgrind.txt "$OUTPUT_DIR/"; then
        echo "Error: Failed to copy output file"
        exit 1
    fi
    echo "Test output saved to $OUTPUT_DIR/valgrind.txt"
else
    # Run directly with output to terminal
    if ! "${valgrind_cmd[@]}"; then
        echo "Error: Valgrind command failed"
        exit 1
    fi
fi
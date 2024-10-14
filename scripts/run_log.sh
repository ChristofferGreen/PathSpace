#!/bin/bash

# Define macros (functions) for common checks
ARGS_INSUFFICIENT() {
    [ $# -lt 2 ]
}

FILE_EXISTS() {
    [ -f "$1" ]
}

TEST_SUCCEEDED() {
    [ $? -eq 0 ]
}

# Constants
readonly EXECUTABLE="./build/tests/PathSpaceTests"
readonly LOG_FILE="./log.txt"

# Function to check if both test case and subcase names were provided
check_args() {
    if ARGS_INSUFFICIENT "$@"; then
        echo "Error: Insufficient arguments provided."
        echo "Usage: $0 <test_case_name> <subcase_name>"
        exit 1
    fi
}

# Function to check if the executable exists
check_executable() {
    if ! FILE_EXISTS "$EXECUTABLE"; then
        echo "Error: Executable not found at $EXECUTABLE"
        exit 1
    fi
}

# Function to run the test
run_test() {
    local test_case="$1"
    local subcase="$2"
    echo "Running test case: $test_case"
    echo "Subcase: $subcase"
    echo "Log will be written to: $LOG_FILE"

    # Run the test and redirect stderr to log file
    $EXECUTABLE --test-case="$test_case" --subcase="$subcase" 2> "$LOG_FILE"

    if TEST_SUCCEEDED; then
        echo "Test completed successfully."
    else
        echo "Test failed. Check $LOG_FILE for details."
    fi
}

# Function to display log contents
display_log() {
    echo "Log file contents:"
    cat "$LOG_FILE"
}

# Main function
main() {
    local test_case="$1"
    local subcase="$2"

    check_args "$@"
    check_executable

    run_test "$test_case" "$subcase"
    display_log
}

# Run main function
main "$@"

# Ex: ./scripts/run_log.sh "PathSpace Read" "PathSpace Read Block"
#!/bin/bash

# Global variable for verbose mode
VERBOSE=false

# Function to log messages in verbose mode
log_verbose() {
    if [ "$VERBOSE" = true ]; then
        echo "[INFO] $1" >&2
    fi
}

# Function to get relative path
get_relative_path() {
    local file=$1
    local base_dir=$(cd .. && pwd)
    echo "${file#$base_dir/}"
}

# Function to read and write header
write_header() {
    local output_file=$1
    local header_file="header.txt"
    
    if [[ -f "$header_file" ]]; then
        log_verbose "Adding header from $header_file"
        cat "$header_file" >> "$output_file" || return 1
        echo "" >> "$output_file"  # Add a blank line after the header
    else
        log_verbose "Warning: header.txt not found. Skipping header."
    fi
}

# Function to write metadata to the output file
write_metadata() {
    local output_file=$1
    log_verbose "Writing metadata"
    {
        echo "<!-- Project Files -->"
        echo "<!-- Generated on: $(date) -->"
        echo ""
    } >> "$output_file" || return 1
}

# Function to generate and write file structure
write_file_structure() {
    local output_file="$1"
    echo "<project_structure>" >> "$output_file"
    tree -P "*.cpp|*.hpp|*.h|*.md" --prune -I ".*|build|ext|myenv" ../ >> "$output_file"
    echo "</project_structure>" >> "$output_file"
}

# Function to write a file's contents to the output file
write_file_contents() {
    local file=$1
    local output_file=$2
    local relative_path=$(get_relative_path "$file")
    local filename=$(basename "$file")

    log_verbose "Processing file: $relative_path"
    {
        echo "<!-- === $relative_path === -->"
        echo "<$filename>"
        cat "$file"
        echo "</$filename>"
        echo ""
    } >> "$output_file" || return 1
}

# Function to process files
process_files() {
    local output_file=$1
    log_verbose "Searching for C++, HPP, H, and MD files"
    find .. -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "CMakeList.txt" -o -name "*.md" \) | while read -r file; do
        if [[ "$file" == *"/build/"* || "$file" == *"/ext/"* || "$file" == *"/tests/"* ]]; then
            log_verbose "Skipping file in excluded directory: $file"
            continue
        fi
        write_file_contents "$file" "$output_file" || return 1
    done
}

# Function to parse command line arguments
parse_arguments() {
    while [[ "$#" -gt 0 ]]; do
        case $1 in
            --verbose) VERBOSE=true ;;
            *) OUTPUT_FILE="$1" ;;
        esac
        shift
    done
}

# Main function
main() {
    parse_arguments "$@"
    local output_file="${OUTPUT_FILE:-output.txt}"

    log_verbose "Starting script execution"
    log_verbose "Output file: $output_file"

    if ! touch "$output_file"; then
        echo "Error: Unable to create or write to $output_file" >&2
        exit 1
    fi

    > "$output_file"
    
    write_header "$output_file" || { echo "Error: Failed to write header" >&2; exit 1; }
    write_metadata "$output_file" || { echo "Error: Failed to write metadata" >&2; exit 1; }
    write_file_structure "$output_file" || { echo "Error: Failed to write file structure" >&2; exit 1; }
    process_files "$output_file" || { echo "Error: Failed to process files" >&2; exit 1; }

    log_verbose "Script execution completed"
    echo "Project file contents have been written to $output_file"
}

# Run the main function
main "$@"
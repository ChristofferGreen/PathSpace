#!/bin/bash

# Output file
output_file="output.txt"

# Clear the output file if it already exists
> "$output_file"

# Find C++ files in the parent directory and its subdirectories
find .. -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.cc" \) | while read -r file; do
    # Skip files in the build directory
    if [[ "$file" == *"/build/"* ]]; then
        continue
    fi
    
    # Skip files in paths containing "ext" or "tests"
    if [[ "$file" == *"/ext/"* || "$file" == *"/tests/"* ]]; then
        continue
    fi
    
    # Write the file name as a header
    echo "=== $file ===" >> "$output_file"
    
    # Write the contents of the file
    cat "$file" >> "$output_file"
    
    # Add a newline for separation
    echo "" >> "$output_file"
done

echo "C++ file contents have been written to $output_file"
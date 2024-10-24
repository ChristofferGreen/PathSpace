#!/bin/bash

# Global variables
VERBOSE=false
MINIMAL=false
INCLUDE_TESTS=false
DEFAULT_OUTPUT="collected_sources.txt"
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

# Parse command line arguments
output_file="$DEFAULT_OUTPUT"
for arg in "$@"; do
    case "$arg" in
        --verbose) VERBOSE=true ;;
        --minimal) MINIMAL=true ;;
        --add-tests) INCLUDE_TESTS=true ;;
        *) output_file="$arg" ;;
    esac
done

# Create/clear output file
> "$output_file"

# Write header if exists
if [[ -f "scripts/header.txt" ]]; then
    cat "scripts/header.txt" >> "$output_file"
    echo "" >> "$output_file"
fi

# Write project structure if not minimal
if [ "$MINIMAL" = false ]; then
    echo "<project_structure>" >> "$output_file"
    tree -P "*.cpp|*.hpp|*.h|*.md|CMakeLists.txt" --prune -I ".*|build|ext|myenv" . >> "$output_file"
    echo "</project_structure>" >> "$output_file"
fi

# Find all relevant files
files=$(find . -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "CMakeLists.txt" -o -name "*.md" \))

# Process all found files
while IFS= read -r file; do
    [ -z "$file" ] && continue
    
    # Skip files in build and src/ext directories
    if [[ "$file" == *"/build/"* || "$file" == *"/src/ext/"* ]]; then
        continue
    fi
    
    # Check if file is in allowed directories (root, docs, src/pathspace, tests)
    if [[ "$file" != "./"* && "$file" != "./docs/"* && "$file" != "./src/pathspace/"* && "$file" != "./tests/"* ]] || \
       [[ "$file" =~ /\.[^/]+/ ]]; then  # Skip hidden directories
        continue
    fi

    # Skip if not including tests
    if [[ "$INCLUDE_TESTS" = false && "$file" == "./tests/"* ]]; then
        [ "$VERBOSE" = true ] && echo -e "[INFO] ${RED}Excluding${NC}: $file (test file)"
        continue
    fi

    # Skip if minimal mode
    if [ "$MINIMAL" = true ]; then
        if [[ "$file" == "./src/pathspace/path/"* ]]; then
            [ "$VERBOSE" = true ] && echo -e "[INFO] ${RED}Excluding${NC}: $file (pathspace/path in minimal mode)"
            continue
        fi
        if [[ "$file" == *"CMakeLists.txt" ]]; then
            [ "$VERBOSE" = true ] && echo -e "[INFO] ${RED}Excluding${NC}: $file (CMake file in minimal mode)"
            continue
        fi
    fi

    [ "$VERBOSE" = true ] && echo -e "[INFO] ${GREEN}Including${NC}: $file"
    
    # Write file contents
    echo "<!-- === $file === -->" >> "$output_file"
    echo "<$(basename "$file")>" >> "$output_file"
    cat "$file" >> "$output_file"
    echo "</$(basename "$file")>" >> "$output_file"
    echo "" >> "$output_file"
done <<< "$files"

echo "Project file contents have been written to $output_file"
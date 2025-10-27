#!/usr/bin/env bash
# Run the codex command multiple times with the default prompt.

set -euo pipefail

loops="${1:-10}"

if ! [[ "$loops" =~ ^[0-9]+$ ]] || (( loops <= 0 )); then
    echo "usage: $0 [loops>=1]" >&2
    exit 1
fi

for ((i = 1; i <= loops; ++i)); do
    codex "Follow the instructions in docs/AI_Default_Prompts.md"
done

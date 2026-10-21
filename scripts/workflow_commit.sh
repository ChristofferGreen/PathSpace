#!/usr/bin/env bash
# Enforce the full "build → loop tests → commit → push" workflow with required commit metadata.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <commit-title> <commit-message>" >&2
    exit 1
fi

commit_title="$1"
commit_message="$2"

if [[ -z "${commit_title// }" ]]; then
    echo "error: commit title must not be empty" >&2
    exit 1
fi

if [[ -z "${commit_message// }" ]]; then
    echo "error: commit message must not be empty" >&2
    exit 1
fi

cmake --build build -j

./scripts/compile.sh --clean --test --loop=5 --release

if git diff --quiet && git diff --cached --quiet; then
    echo "error: no changes to commit" >&2
    exit 1
fi

# Stage everything so conventional commit lines can succeed even with new files.
git add -A

git commit -m "$commit_title" -m "$commit_message"

git push origin master

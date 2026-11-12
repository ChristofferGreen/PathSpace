#!/usr/bin/env bash
# Run the hierarchy benchmark once per memory resource so results can be compared in one session.
# Usage: ./scripts/bench_allocators.sh [build-dir] [extra PathSpaceBench args...]
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build-release}"
[[ $# -eq 0 ]] || shift
BENCH="$BUILD_DIR/bench/PathSpaceBench"
[[ -x "$BENCH" ]] || { echo "missing $BENCH (build the Release tree first)" >&2; exit 1; }

for resource in default pool monotonic counting; do
  echo "=== resource: $resource ==="
  "$BENCH" --resource "$resource" "$@"
done

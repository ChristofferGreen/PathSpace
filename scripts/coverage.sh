#!/usr/bin/env bash
set -euo pipefail

# Simple coverage helper:
# 1) Configures a coverage-enabled build tree (default: build-coverage)
# 2) Builds the tree
# 3) Runs the test suite
# 4) Emits a filtered coverage summary (src/include only) and writes gcovr.json

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build-coverage}"
JOBS="${CTEST_JOBS:-$(command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu || nproc)}"
TIMEOUT="${CTEST_TIMEOUT:-60}"

cmake_args=(
  -S "${ROOT}"
  -B "${BUILD}"
  -DENABLE_CODE_COVERAGE=ON
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"
)

mkdir -p "${BUILD}"
if [ ! -f "${BUILD}/CMakeCache.txt" ]; then
  cmake "${cmake_args[@]}"
fi

cmake --build "${BUILD}" -j "${JOBS}"

ctest --test-dir "${BUILD}" --output-on-failure -j "${JOBS}" --timeout "${TIMEOUT}"

filter_args=(
  -r "${ROOT}"
  --filter "${ROOT}/src"
  --filter "${ROOT}/include"
  --exclude "${ROOT}/tests"
  --exclude "${ROOT}/third_party"
)

# Text summary to stdout
python3 -m gcovr "${filter_args[@]}" "${GCOVR_FLAGS:-}" || exit 1

# JSON artifact for tooling
python3 -m gcovr "${filter_args[@]}" --json -o "${BUILD}/gcovr.json" "${GCOVR_FLAGS:-}"

echo "Coverage JSON written to ${BUILD}/gcovr.json"

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
OUTPUT_PATH="${OUTPUT_PATH:-${REPO_ROOT}/docs/perf/pixel_noise_baseline.json}"
FRAMES="${FRAMES:-120}"
WIDTH="${WIDTH:-1280}"
HEIGHT="${HEIGHT:-720}"
SEED="${SEED:-123456789}"
PRESENT_REFRESH="${PRESENT_REFRESH:-0}"
MIN_FPS="${MIN_FPS:-50.0}"
BUDGET_PRESENT_MS="${BUDGET_PRESENT_MS:-20.0}"
BUDGET_RENDER_MS="${BUDGET_RENDER_MS:-20.0}"
EXTRA_ARGS=("$@")

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "Configuring build directory at ${BUILD_DIR}"
  cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
fi

echo "Building pixel_noise_example"
cmake --build "${BUILD_DIR}" --target pixel_noise_example

PIXEL_NOISE_BIN="${BUILD_DIR}/examples/pixel_noise_example"
if [[ ! -x "${PIXEL_NOISE_BIN}" ]]; then
  PIXEL_NOISE_BIN="${BUILD_DIR}/pixel_noise_example"
fi

if [[ ! -x "${PIXEL_NOISE_BIN}" ]]; then
  echo "pixel_noise_example binary not found (looked under ${BUILD_DIR}/examples and ${BUILD_DIR})" >&2
  exit 1
fi

mkdir -p "$(dirname "${OUTPUT_PATH}")"

echo "Capturing baseline to ${OUTPUT_PATH}"
"${PIXEL_NOISE_BIN}" \
  --headless \
  --width="${WIDTH}" \
  --height="${HEIGHT}" \
  --frames="${FRAMES}" \
  --present-refresh="${PRESENT_REFRESH}" \
  --report-metrics \
  --report-interval=0.5 \
  --present-call-metric \
  --seed="${SEED}" \
  --budget-present-ms="${BUDGET_PRESENT_MS}" \
  --budget-render-ms="${BUDGET_RENDER_MS}" \
  --min-fps="${MIN_FPS}" \
  --write-baseline="${OUTPUT_PATH}" \
  "${EXTRA_ARGS[@]}"

echo "Baseline capture complete."

#!/usr/bin/env bash
# Replay a previously recorded widgets_example trace without opening the UI window.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
  cat <<EOF
Usage: $(basename "$0") -i <trace_file> [--no-build] [--run-tests] [--] [widgets_example args...]

Options:
  -i, --input PATH   Trace file to replay (required).
      --no-build     Skip invoking scripts/compile_widgets.sh before running.
      --run-tests    After replay, run PathSpaceUITests to ensure regression tests pass.
  -h, --help         Show this help message.

Additional arguments after -- are forwarded to widgets_example.
EOF
}

TRACE_FILE=""
RUN_BUILD=1
RUN_TESTS=0
FORWARD_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -i|--input)
      [[ $# -lt 2 ]] && { echo "Error: --input requires a path." >&2; exit 2; }
      TRACE_FILE="$2"
      shift 2
      ;;
    --no-build)
      RUN_BUILD=0
      shift
      ;;
    --run-tests)
      RUN_TESTS=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      FORWARD_ARGS+=("$@")
      break
      ;;
    *)
      FORWARD_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ -z "${TRACE_FILE}" ]]; then
  echo "Error: trace input path is required." >&2
  echo >&2
  usage
  exit 2
fi

if [[ ! -f "${TRACE_FILE}" ]]; then
  echo "Error: trace file '${TRACE_FILE}' does not exist." >&2
  exit 1
fi

if [[ $RUN_BUILD -eq 1 ]]; then
  "${ROOT_DIR}/scripts/compile_widgets.sh"
fi

BINARY="${ROOT_DIR}/build/widgets_example"
if [[ ! -x "${BINARY}" ]]; then
  echo "Error: widgets_example binary not found at ${BINARY}." >&2
  echo "Run scripts/compile_widgets.sh first or pass --no-build after building manually." >&2
  exit 1
fi

export WIDGETS_EXAMPLE_HEADLESS="${WIDGETS_EXAMPLE_HEADLESS:-1}"
export WIDGETS_EXAMPLE_TRACE_REPLAY="${TRACE_FILE}"

"${BINARY}" "${FORWARD_ARGS[@]}"
RC=$?
if [[ $RC -ne 0 ]]; then
  exit $RC
fi

if [[ $RUN_TESTS -eq 1 ]]; then
  ctest --test-dir "${ROOT_DIR}/build" --output-on-failure -R PathSpaceUITests
  RC=$?
fi

exit $RC

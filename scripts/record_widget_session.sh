#!/usr/bin/env bash
# Capture pointer and keyboard interactions from widgets_example into a trace file.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
  cat <<EOF
Usage: $(basename "$0") -o <trace_file> [--no-build] [--] [widgets_example args...]

Options:
  -o, --output PATH   Trace file to write (required).
      --no-build      Skip invoking scripts/compile_widgets.sh before running.
  -h, --help          Show this help message.

All remaining arguments after -- are forwarded to widgets_example.
EOF
}

TRACE_FILE=""
RUN_BUILD=1
FORWARD_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -o|--output)
      [[ $# -lt 2 ]] && { echo "Error: --output requires a path." >&2; exit 2; }
      TRACE_FILE="$2"
      shift 2
      ;;
    --no-build)
      RUN_BUILD=0
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
  echo "Error: trace output path is required." >&2
  echo >&2
  usage
  exit 2
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

export WIDGETS_EXAMPLE_TRACE_RECORD="${TRACE_FILE}"
exec "${BINARY}" "${FORWARD_ARGS[@]}"

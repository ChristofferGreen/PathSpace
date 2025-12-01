#!/usr/bin/env bash
# Run the codex command multiple times with the default prompt.
# Optional: stop looping as soon as a given file path disappears (e.g., a plan doc
# that gets moved to docs/finished/).

set -euo pipefail

usage() {
    cat <<'USAGE' >&2
usage: loop_codex.sh <current_plan_doc> [loops>=1]

  current_plan_doc  Path to the active plan document. The script stops looping
                    as soon as this file disappears (plan finished/moved).
  loops             Optional number of iterations. When omitted, the script
                    runs until the watched plan doc disappears.
USAGE
}

plan_doc=""
loops=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        *)
            if [[ -z "$plan_doc" ]]; then
                plan_doc="$1"
            elif [[ -z "$loops" ]]; then
                loops="$1"
            else
                usage
                exit 1
            fi
            ;;
    esac
    shift || break
done

if [[ -z "$plan_doc" ]]; then
    usage
    exit 1
fi

if [[ -z "$loops" ]]; then
    loops="0"  # run until the watched plan doc disappears
fi

if ! [[ "$loops" =~ ^[0-9]+$ ]]; then
    usage
    exit 1
fi

if [[ ! -e "$plan_doc" ]]; then
    echo "loop_codex: '$plan_doc' already missing; nothing to do" >&2
    exit 0
fi

iter=1
while :; do
    codex exec --full-auto \
        $'Current_Plan_Doc is '"$plan_doc"$'\nFollow the instructions in docs/AI_Default_Prompts.md'

    if [[ ! -e "$plan_doc" ]]; then
        echo "loop_codex: '$plan_doc' missing; stopping after iteration $iter" >&2
        break
    fi

    if (( loops != 0 && iter >= loops )); then
        break
    fi
    ((iter++))
done

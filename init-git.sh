#!/usr/bin/env bash
# init-git.sh — one-time Git hook setup for this repository.
#
# What it does:
# - Option A (default): symlink .git/hooks/pre-push -> scripts/git-hooks/pre-push (thin wrapper)
# - Option B (--use-core-hooks-path): set git config core.hooksPath to scripts/git-hooks
#
# The hook entrypoint delegates to scripts/git-hooks/pre-push.local.sh which runs looped tests
# and a local smoke test of the example (configurable via env flags).
#
# Usage:
#   ./init-git.sh                   # install using a symlink in .git/hooks/
#   ./init-git.sh --use-core-hooks-path   # use `git config core.hooksPath` instead
#   ./init-git.sh --uninstall       # remove hook and/or unset core.hooksPath if set to repo hooks
#   ./init-git.sh --force           # overwrite existing hook/symlink
#
# Notes:
# - Git intentionally does not auto-install hooks from the repo; this script helps set them up locally.
# - To customize hook behavior on each run: see scripts/git-hooks/pre-push.local.sh (env toggles).
#   Examples:
#     SKIP_EXAMPLE=1 git push
#     SKIP_LOOP_TESTS=1 BUILD_TYPE=Debug JOBS=8 git push
#     ENABLE_PATHIO_MACOS=ON git push   # on macOS, include PathIO backends when building the example

set -euo pipefail

say()  { printf "\033[1;34m[init-git]\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32m[init-git]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[init-git]\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31m[init-git]\033[0m %s\n" "$*" >&2; }

usage() {
  cat <<'EOF'
Usage:
  init-git.sh [--use-core-hooks-path] [--uninstall] [--force]

Options:
  --use-core-hooks-path  Configure `git config core.hooksPath` to point at scripts/git-hooks
  --uninstall            Remove local hook symlink and/or unset core.hooksPath (if pointing at repo hooks)
  --force                Overwrite existing hook/symlink or core.hooksPath

This script sets up the pre-push hook that delegates to scripts/git-hooks/pre-push.local.sh
so you can run looped tests and a local example smoke-test before pushing.

Examples:
  ./init-git.sh
  ./init-git.sh --use-core-hooks-path
  ./init-git.sh --uninstall
EOF
}

# --- Parse args ---
USE_CORE_HOOKS_PATH=0
UNINSTALL=0
FORCE=0
if [[ $# -gt 0 ]]; then
  for arg in "$@"; do
    case "$arg" in
      --use-core-hooks-path) USE_CORE_HOOKS_PATH=1 ;;
      --uninstall)           UNINSTALL=1 ;;
      --force)               FORCE=1 ;;
      -h|--help)             usage; exit 0 ;;
      *)
        err "Unknown option: $arg"
        usage
        exit 1
        ;;
    esac
  done
fi

# --- Ensure inside a git repo ---
if ! command -v git >/dev/null 2>&1; then
  err "git is not installed or not on PATH."
  exit 1
fi

if ! ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  err "Not inside a git repository."
  exit 1
fi
GIT_DIR="$(git rev-parse --git-dir)"
HOOKS_DIR="${GIT_DIR%/}/hooks"

cd "$ROOT"

REPO_HOOK_ENTRY="scripts/git-hooks/pre-push"
REPO_HOOK_HELPER="scripts/git-hooks/pre-push.local.sh"
REPO_HOOKS_DIR="scripts/git-hooks"

# --- Validate files exist in repo ---
if [[ ! -f "$REPO_HOOK_ENTRY" ]]; then
  err "Missing hook entrypoint: $REPO_HOOK_ENTRY"
  exit 1
fi
if [[ ! -f "$REPO_HOOK_HELPER" ]]; then
  err "Missing hook helper: $REPO_HOOK_HELPER"
  exit 1
fi

# Ensure entrypoint and helper are executable
if [[ ! -x "$REPO_HOOK_ENTRY" ]]; then
  warn "Marking $REPO_HOOK_ENTRY as executable"
  chmod +x "$REPO_HOOK_ENTRY"
fi
if [[ ! -x "$REPO_HOOK_HELPER" ]]; then
  warn "Marking $REPO_HOOK_HELPER as executable"
  chmod +x "$REPO_HOOK_HELPER"
fi

# --- Helpers ---
abs_path() (
  cd "$(dirname "$1")" && printf "%s/%s\n" "$(pwd -P)" "$(basename "$1")"
)

backup_file() {
  local path="$1"
  if [[ -e "$path" && $FORCE -eq 1 ]]; then
    local ts
    ts="$(date +%Y%m%d_%H%M%S)"
    local backup="${path}.bak.${ts}"
    warn "Backing up existing: $path -> $backup"
    mv -f "$path" "$backup"
  fi
}

remove_if_symlink_to() {
  local link="$1"
  local target="$2"
  if [[ -L "$link" ]]; then
    local dest
    dest="$(readlink "$link")"
    if [[ "$dest" == "$target" ]]; then
      say "Removing symlink $link -> $target"
      rm -f "$link"
    fi
  fi
}

# --- Uninstall path ---
if [[ $UNINSTALL -eq 1 ]]; then
  say "Uninstalling Git hook setup..."
  # Remove symlink if it points to our entry
  if [[ -d "$HOOKS_DIR" ]]; then
    remove_if_symlink_to "$HOOKS_DIR/pre-push" "../../$REPO_HOOK_ENTRY"
  fi
  # Unset core.hooksPath if points to our repo hooks
  if HOOKS_PATH_VAL="$(git config --get core.hooksPath 2>/dev/null || true)"; then
    # Compare absolute paths
    REPO_HOOKS_ABS="$(abs_path "$REPO_HOOKS_DIR")"
    if [[ -n "$HOOKS_PATH_VAL" ]]; then
      # Normalize HOOKS_PATH_VAL to absolute path
      case "$HOOKS_PATH_VAL" in
        /*) CURRENT_PATH="$HOOKS_PATH_VAL" ;;
        *) CURRENT_PATH="$(cd "$ROOT" && cd "$HOOKS_PATH_VAL" 2>/dev/null && pwd -P || echo "")" ;;
      esac
      if [[ "$CURRENT_PATH" == "$REPO_HOOKS_ABS" ]]; then
        say "Unsetting git config core.hooksPath ($HOOKS_PATH_VAL)"
        git config --unset core.hooksPath || true
      fi
    fi
  fi
  ok "Uninstall complete."
  exit 0
fi

# --- Install path ---
if [[ $USE_CORE_HOOKS_PATH -eq 1 ]]; then
  # Use absolute path for core.hooksPath
  REPO_HOOKS_ABS="$(abs_path "$REPO_HOOKS_DIR")"
  if [[ ! -d "$REPO_HOOKS_ABS" ]]; then
    err "Hooks directory missing: $REPO_HOOKS_ABS"
    exit 1
  fi
  say "Configuring git core.hooksPath -> $REPO_HOOKS_ABS"
  if [[ $FORCE -eq 1 ]]; then
    git config --unset core.hooksPath 2>/dev/null || true
  fi
  git config core.hooksPath "$REPO_HOOKS_ABS"
  ok "Installed via core.hooksPath."
else
  # Symlink .git/hooks/pre-push -> ../../scripts/git-hooks/pre-push
  mkdir -p "$HOOKS_DIR"
  local_link="$HOOKS_DIR/pre-push"
  rel_target="../../$REPO_HOOK_ENTRY"
  if [[ -e "$local_link" && $FORCE -ne 1 ]]; then
    err "Hook already exists at $local_link. Use --force to overwrite, or --use-core-hooks-path to switch mode."
    exit 1
  fi
  backup_file "$local_link"
  say "Creating symlink: $local_link -> $rel_target"
  ln -s "$rel_target" "$local_link"
  chmod +x "$local_link" || true
  ok "Installed via .git/hooks symlink."
fi

# --- Verification ---
say "Verifying installation..."
# Determine which path Git will use
if HOOKS_PATH_VAL="$(git config --get core.hooksPath 2>/dev/null || true)"; then
  if [[ -n "$HOOKS_PATH_VAL" ]]; then
    say "core.hooksPath is set to: $HOOKS_PATH_VAL"
    if [[ ! -x "$REPO_HOOK_ENTRY" ]]; then
      err "Hook entrypoint is not executable: $REPO_HOOK_ENTRY"
      exit 1
    fi
    ok "Hook entrypoint present and executable."
    ok "You can now push; the pre-push hook will run local checks."
    exit 0
  fi
fi

if [[ -x "$HOOKS_DIR/pre-push" ]]; then
  ok "Hook symlink is present and executable at $HOOKS_DIR/pre-push"
  ok "You can now push; the pre-push hook will run local checks."
else
  err "Hook not found or not executable at $HOOKS_DIR/pre-push"
  exit 1
fi

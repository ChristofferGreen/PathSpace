#!/usr/bin/env bash
# PathSpace/scripts/create_pr.sh
#
# Create a GitHub Pull Request for the current branch and open it in your browser.
# - Uses GitHub CLI (gh) when available for the best experience.
# - Falls back to opening the compare page in a browser if gh is not installed.
#
# Examples:
#   ./scripts/create_pr.sh
#   ./scripts/create_pr.sh -b main -t "fix(core): handle iterator bounds" -r your-handle -l perf,tests
#   ./scripts/create_pr.sh --draft --title "refactor(path): unify iterator types"
#
# Prerequisites:
# - A git repository with a remote named "origin" pointing to GitHub.
# - Branch pushed to origin (script will push automatically unless --no-push).
# - Optional: GitHub CLI (`gh`) authenticated (`gh auth login`).

set -euo pipefail

# ----------------------------
# Helpers
# ----------------------------
say()  { printf "\033[1;34m[create-pr]\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32m[create-pr]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[create-pr]\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31m[create-pr]\033[0m %s\n" "$*" >&2; }

open_url() {
  local url="$1"
  if command -v open >/dev/null 2>&1; then
    open "$url" >/dev/null 2>&1 || true
  elif command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$url" >/dev/null 2>&1 || true
  elif command -v start >/dev/null 2>&1; then
    start "$url" >/dev/null 2>&1 || true
  else
    warn "Could not detect a browser opener. Please open this URL manually:"
    echo "$url"
  fi
}

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  -b, --base BRANCH     Base branch for the PR (default: detect 'main' or 'master')
  -t, --title TITLE     PR title (default: last commit subject)
  -B, --body TEXT       PR body (default: bullet list of commit subjects since base)
  -r, --reviewers LIST  Comma-separated GitHub handles to request review from (gh only)
  -a, --assignees LIST  Comma-separated GitHub handles to assign (gh only)
  -l, --labels LIST     Comma-separated labels to apply (gh only)
      --draft           Create PR as draft
      --no-push         Do NOT push the branch (assumes it's already on origin)
  -h, --help            Show this help and exit

Behavior:
- If GitHub CLI (gh) is available and authenticated, this script will run:
    gh pr create ... and open the created PR in your browser.
- Otherwise, it will open the GitHub compare page for the branch so you can create a PR manually.

Commit message tips (Conventional Commits):
  type(scope): imperative subject
  types: feat, fix, perf, refactor, docs, test, chore, build, ci, revert, style
  scopes: core, path, layer, task, type, tests, docs, build, scripts

Examples:
  fix(iterator): rebind views/iterators to local storage for safe copies
  perf(waitmap): reduce notify scan with concrete-path fast path
  docs(overview): document compile_commands.json refresh workflow
  build(scripts): prefer Ninja and auto-parallelize by CPU core count
  refactor(path): unify iterator types and remove string_view end() usage
EOF
}

require_git() {
  command -v git >/dev/null 2>&1 || { err "git not found in PATH"; exit 1; }
}

get_repo_slug() {
  # Parse owner/repo from origin URL (supports https and ssh)
  local url
  url="$(git config --get remote.origin.url || true)"
  if [[ -z "$url" ]]; then
    err "No 'origin' remote configured."
    exit 1
  fi
  case "$url" in
    git@github.com:*)
      echo "${url##git@github.com:}" | sed 's/\.git$//'
      ;;
    https://github.com/*)
      echo "${url#https://github.com/}" | sed 's/\.git$//'
      ;;
    http://github.com/*)
      echo "${url#http://github.com/}" | sed 's/\.git$//'
      ;;
    *)
      err "Unsupported origin URL format: $url"
      exit 1
      ;;
  esac
}

default_base_branch() {
  # Prefer 'main', else 'master', else first remote HEAD target
  if git ls-remote --exit-code --heads origin main >/dev/null 2>&1; then
    echo "main"
  elif git ls-remote --exit-code --heads origin master >/dev/null 2>&1; then
    echo "master"
  else
    # Fallback to local branch named main or master if present
    if git show-ref --verify --quiet refs/heads/main; then
      echo "main"
    elif git show-ref --verify --quiet refs/heads/master; then
      echo "master"
    else
      # Last resort: resolve origin/HEAD target
      git remote show origin 2>/dev/null | awk '/HEAD branch/ {print $NF}' | head -n1
    fi
  fi
}

ensure_branch_pushed() {
  local branch="$1"
  local no_push="$2"
  if git rev-parse --abbrev-ref --symbolic-full-name "@{u}" >/dev/null 2>&1; then
    # Upstream exists
    return 0
  fi
  if [[ "$no_push" == "1" ]]; then
    warn "Branch '$branch' has no upstream and --no-push was set. PR creation may fail."
    return 0
  fi
  say "Pushing current branch '$branch' to origin (setting upstream)..."
  git push -u origin "$branch"
  ok "Pushed '$branch' to origin with upstream."
}

collect_default_title() {
  git log -1 --pretty=%s
}

collect_default_body() {
  local base="$1"
  # Bullet list of commit subjects since base (if range is valid)
  if git merge-base --is-ancestor "$base" HEAD 2>/dev/null; then
    git log --pretty=format:"- %s" "${base}..HEAD"
  else
    # If base is not ancestor (or unknown), fallback to recent commits
    git log -n 10 --pretty=format:"- %s"
  fi
}

# ----------------------------
# Parse arguments
# ----------------------------
BASE=""
TITLE=""
BODY=""
REVIEWERS=""
ASSIGNEES=""
LABELS=""
DRAFT=0
NO_PUSH=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--base) shift; BASE="${1:-}";;
    -t|--title) shift; TITLE="${1:-}";;
    -B|--body) shift; BODY="${1:-}";;
    -r|--reviewers) shift; REVIEWERS="${1:-}";;
    -a|--assignees) shift; ASSIGNEES="${1:-}";;
    -l|--labels) shift; LABELS="${1:-}";;
    --draft) DRAFT=1;;
    --no-push) NO_PUSH=1;;
    -h|--help) usage; exit 0;;
    *) err "Unknown argument: $1"; usage; exit 1;;
  esac
  shift || true
done

# ----------------------------
# Main
# ----------------------------
require_git

# Ensure we are in a git repo
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || { err "Not inside a git repository"; exit 1; }

# Detect branch, repo, base
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [[ -z "${BASE:-}" ]]; then
  BASE="$(default_base_branch)"
  [[ -n "$BASE" ]] || BASE="main"
fi

if [[ "$BRANCH" == "$BASE" ]]; then
  err "You are on '$BRANCH'. Create a topic branch before creating a PR."
  exit 1
fi

REPO_SLUG="$(get_repo_slug)"
say "Repository: $REPO_SLUG"
say "Base branch: $BASE"
say "Head branch: $BRANCH"

# Push branch if needed
ensure_branch_pushed "$BRANCH" "$NO_PUSH"

# Compute defaults for title/body if missing
if [[ -z "${TITLE:-}" ]]; then
  TITLE="$(collect_default_title)"
fi
if [[ -z "${BODY:-}" ]]; then
  BODY="$(collect_default_body "$BASE")"
fi

# Attempt with GitHub CLI (gh)
if command -v gh >/dev/null 2>&1; then
  say "Creating PR using GitHub CLI..."
  # Build gh args
  GH_ARGS=(pr create --repo "$REPO_SLUG" --base "$BASE" --head "$BRANCH" --title "$TITLE" --body "$BODY")
  [[ "$DRAFT" -eq 1 ]] && GH_ARGS+=(--draft)
  [[ -n "$REVIEWERS" ]] && GH_ARGS+=(--reviewer "$REVIEWERS")
  [[ -n "$ASSIGNEES" ]] && GH_ARGS+=(--assignee "$ASSIGNEES")
  [[ -n "$LABELS" ]] && GH_ARGS+=(--label "$LABELS")

  # Create PR and capture URL (gh prints the URL on success)
  if PR_URL="$(gh "${GH_ARGS[@]}" 2>&1 | tee /dev/stderr | grep -Eo 'https://github\.com/[^ ]+/pull/[0-9]+' | tail -n1)"; then
    if [[ -z "$PR_URL" ]]; then
      # Fallback: query the URL explicitly
      PR_URL="$(gh pr view --repo "$REPO_SLUG" --json url -q .url 2>/dev/null || true)"
    fi
    if [[ -n "$PR_URL" ]]; then
      ok "PR created: $PR_URL"
      open_url "$PR_URL"
      exit 0
    fi
  fi
  warn "GitHub CLI could not determine the PR URL automatically."
fi

# Fallback: open the compare page in browser
COMPARE_URL="https://github.com/${REPO_SLUG}/compare/${BASE}...${BRANCH}?expand=1"
warn "Falling back to opening compare page. Complete PR details in the browser."
say "Compare URL (create PR from this page):"
echo "$COMPARE_URL"
open_url "$COMPARE_URL"

# Also print suggested title/body to copy-paste
cat <<EOM

----- Suggested PR Title -----
$TITLE

----- Suggested PR Body -----
$BODY

EOM

ok "Done."
exit 0

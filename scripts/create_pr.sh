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
# - Optional: GitHub CLI (`gh`) authenticated (`gh auth login`), or
# - GH_TOKEN environment variable set (with repo access) for REST API fallback.

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
  -B, --body TEXT       PR body (default: minimal list of changed files since base)
  -r, --reviewers LIST  Comma-separated GitHub handles to request review from (gh only)
  -a, --assignees LIST  Comma-separated GitHub handles to assign (gh only)
  -l, --labels LIST     Comma-separated labels to apply (gh only)
      --draft           Create PR as draft
      --no-push         Do NOT push the branch (assumes it's already on origin)
  -h, --help            Show this help and exit

Behavior:
- If GitHub CLI (gh) is available and authenticated, this script will run:
    gh pr create ... and open the created PR in your browser.
- Else if GH_TOKEN is set, it will call the GitHub REST API to create the PR and open its URL.
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
  # Default to 'master' (repository standard). Validate it exists on origin; otherwise fall back to origin/HEAD.
  if git ls-remote --exit-code --heads origin master >/dev/null 2>&1; then
    echo "master"
  else
    # Last resort: resolve origin/HEAD target
    git remote show origin 2>/dev/null | awk '/HEAD branch/ {print $NF}' | head -n1
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
    err "Branch '$branch' has no upstream and --no-push was set. Push with: git push -u origin \"$branch\""
    exit 1
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
  local files=""
  # Minimal list of changed files since base (if range is valid)
  if git merge-base --is-ancestor "$base" HEAD 2>/dev/null; then
    files="$(git diff --name-only "${base}..HEAD" | sed 's/^/- /')"
  else
    # If base is not ancestor (or unknown), fallback to last commit diff
    files="$(git diff --name-only HEAD~1..HEAD 2>/dev/null | sed 's/^/- /')"
  fi

  if [[ -z "$files" ]]; then
    echo "- No file changes detected"
  else
    echo "Changed files:"
    echo "$files"
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

# Validate base exists on origin
if ! git ls-remote --exit-code --heads origin "$BASE" >/dev/null 2>&1; then
  err "Base branch '$BASE' not found on origin. Use -b master or run: git fetch origin"
  exit 1
fi

# Ensure there are commits between base and HEAD (avoid 'No commits between base and head')
if git merge-base --is-ancestor "$BASE" HEAD 2>/dev/null; then
  if [[ "$(git rev-list --count "${BASE}..HEAD")" -eq 0 ]]; then
    err "No commits between '$BASE' and HEAD. Create or cherry-pick commits onto your topic branch before creating a PR."
    exit 1
  fi
fi

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

# Attempt with GitHub REST API using GH_TOKEN
if [[ -n "${GH_TOKEN:-}" ]]; then
  say "Creating PR using GitHub REST API..."
  OWNER="${REPO_SLUG%%/*}"
  REPO="${REPO_SLUG##*/}"

  # Prepare JSON; prefer jq when available for correct escaping
  if command -v jq >/dev/null 2>&1; then
    JSON="$(jq -n \
      --arg title "$TITLE" \
      --arg head "$BRANCH" \
      --arg base "$BASE" \
      --arg body "$BODY" \
      --argjson draft "$([[ ${DRAFT:-0} -eq 1 ]] && echo true || echo false)" \
      '{title:$title, head:$head, base:$base, body:$body, draft:$draft}')"
  else
    # Minimal escaping for quotes, backslashes, and newlines
    esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/$/\\n/; s/\n/\\n/g'; }
    T_ESC="$(esc "$TITLE")"
    B_ESC="$(esc "$BODY")"
    DRAFT_BOOL="$( [[ ${DRAFT:-0} -eq 1 ]] && echo true || echo false )"
    JSON="{\"title\":\"$T_ESC\",\"head\":\"$BRANCH\",\"base\":\"$BASE\",\"body\":\"$B_ESC\",\"draft\":$DRAFT_BOOL}"
  fi

  RESP="$(curl -sS \
    -H "Authorization: token ${GH_TOKEN}" \
    -H "Accept: application/vnd.github+json" \
    -X POST "https://api.github.com/repos/${REPO_SLUG}/pulls" \
    -d "$JSON" || true)"

  if command -v jq >/dev/null 2>&1; then
    PR_URL="$(printf '%s' "$RESP" | jq -r '.html_url // empty')"
  else
    PR_URL="$(printf '%s' "$RESP" | grep -Eo '\"html_url\"[[:space:]]*:[[:space:]]*\"https://github\.com/[^"]+/pull/[0-9]+' | head -n1 | sed -E 's/.*\"(https:\/\/github\.com\/[^"]+)\"/\1/')"
  fi

  if [[ -n "$PR_URL" ]]; then
    ok "PR created: $PR_URL"
    open_url "$PR_URL"
    exit 0
  else
    warn "GH_TOKEN present but REST API PR creation failed. Falling back to compare page."
  fi
fi

# Fallback: open the compare page in browser
COMPARE_URL="https://github.com/${REPO_SLUG}/compare/${BASE}...${BRANCH}?expand=1"
# Suggest installing GitHub CLI if it's not available locally
if ! command -v gh >/dev/null 2>&1; then
  warn "GitHub CLI (gh) not found. Consider installing it:"
  warn "  macOS (Homebrew): brew install gh"
  warn "  Ubuntu/Debian:    sudo apt-get install gh  (or see https://cli.github.com)"
  warn "Then authenticate via: gh auth login"
fi
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

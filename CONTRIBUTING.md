# Contributing to PathSpace

This document describes the local-first developer workflow, pull request process, commit message conventions, and the minimal checks to run before requesting a review or merging.

The project is optimized for fast local iteration:
- You build and test locally (with a pre-push hook).
- You open a PR for review (with a script that can launch your browser).
- GitHub CI is optional and not required.

---

## Prerequisites

- C++23-capable toolchain (Clang/GCC) and CMake 3.15+
- Ninja (recommended; the build scripts prefer Ninja when available)
- Optional: GitHub CLI (`gh`) for one-command PR creation
- macOS: Command Line Tools (or Xcode) installed

Helpful scripts in `scripts/`:
- `compile.sh` — configure and build, auto-parallelized
- `update_compile_commands.sh` — refresh `./compile_commands.json`
- `run_testcase.sh` — run all tests or a single test by name
- `git-hooks/pre-push` — local pre-push checks
- `create_pr.sh` — create a PR and open it in the browser

---

## Branching and workflow

- Default branch (protected): `master`
- Work on topic branches:
  - `feat/<topic>` — features
  - `fix/<topic>` — bug fixes
  - `perf/<topic>` — performance improvements
  - `refactor/<topic>` — internal refactors
  - `docs/<topic>` — documentation-only changes
- Avoid direct pushes to `master`. Open a PR from your topic branch into `master`. Push your topic branch first with `git push -u origin <branch>` (this sets the correct upstream to `origin/<branch>`), then run `./scripts/create_pr.sh` to automatically create the PR (via `gh` or `GH_TOKEN`) and open it in your browser.
- Contributor policy: Ask before committing and pushing changes. You do not need to ask to run tests.

PR creation gotchas (to avoid upstream mistakes):
- Ensure your topic branch’s upstream is `origin/<branch>`, not `origin/master`. If needed: `git push -u origin <branch>` or `git branch --set-upstream-to=origin/<branch>`.
- Do not commit on the default branch. If you did:
  - Create a topic branch from the default branch: `git checkout -b docs/<topic>`
  - Move the commit off the default branch: on the default branch, run `git reset --hard origin/<default>`; keep your work on the topic branch. Alternatively, cherry-pick onto the topic branch: `git cherry-pick <commit_sha>`.
- If `./scripts/create_pr.sh` warns “You are on 'master'”, create a topic branch and push it before re-running the script.
- If GitHub reports “No commits between base and head” or “Head sha can’t be blank”, push your topic branch (or fix its upstream) and re-run the script.

Recommended merge strategy:
- Use “Squash and merge” to keep a linear history.
- The PR title becomes the final commit subject; the PR body becomes the commit body.

---

## Local development loop

- Configure and build (auto-detects CPU cores; prefers Ninja when available):
  - `./scripts/compile.sh`
  - Full rebuild: `./scripts/compile.sh --clean`
  - Build type: `--debug` (default) or `--release`
  - Target only: `./scripts/compile.sh -t PathSpaceTests`

- Run tests:
  - Quick: `./build/tests/PathSpaceTests`
  - With helper: `./scripts/compile.sh --test`
  - Single test or subcase: `./scripts/run_testcase.sh -t "PathAlias - Forwarding"`

- Logging during tests (only when compiled with `SP_LOG_DEBUG`):
  - Enable: set `PATHSPACE_LOG=1`
  - Default is off for speed; leave unset unless debugging

- Heavy test durations:
  - Long-running multithreaded/perf tests are tuned to be reasonably fast by default.

---

## Compilation database

Keep `./compile_commands.json` up to date so editors/LSPs have correct include paths and flags.

- Refresh after adding/renaming/removing files:
  - `./scripts/update_compile_commands.sh`
  - Copy-only refresh (if already configured): `./scripts/update_compile_commands.sh --no-configure`

The build also mirrors `build/compile_commands.json` to the repository root automatically after each build.

---

## Local pre-push checks

Install once:
- `./init-git.sh` (installs `scripts/git-hooks/pre-push`)

What the hook does by default (local, not CI):
- Full clean build with tests, looped runs to surface flaky issues
- Optional example build/smoke test (can be skipped via env var)

Useful toggles:
- `SKIP_LOOP_TESTS=1` — skip looped tests for quick pushes
- `SKIP_EXAMPLE=1` — skip example app smoke test
- `BUILD_TYPE=Release|Debug` — change build type for the hook run

---

## Creating a Pull Request

Use the helper script (auto-push, create PR, open browser):

- `./scripts/create_pr.sh`
- Options:
  - Base branch: `-b master` (default attempts to detect)
  - Title: `-t "fix(iterator): …"`
  - Draft: `--draft`
  - Reviewers/assignees/labels (when using `gh`): `-r`, `-a`, `-l`
  - Skip auto-push (if you pushed already): `--no-push`

If you have GitHub CLI (`gh`) installed or `GH_TOKEN` set (with repo access), the script will create the PR automatically and open it in your browser. Otherwise, it will open the GitHub compare page so you can finalize the PR manually.

What is `gh` (GitHub CLI) and how to set it up:
- macOS (Homebrew):
  - `brew install gh`
- Ubuntu/Debian:
  - `sudo apt-get install gh` (or see https://cli.github.com for instructions)
- Authenticate:
  - `gh auth login` (follow the prompts; choose HTTPS and your browser)

Handy `gh` commands (run from your topic branch):
- Create a PR with title/body and base:
  - `gh pr create --title "fix(core): iterator bounds" --body "…" --base master`
- Draft PR:
  - `gh pr create --draft`
- Add labels/reviewers/assignees:
  - `gh pr create --label "perf,tests" --reviewer your-handle --assignee your-handle`
- Open the current PR in your browser:
  - `gh pr view --web`

---

## Commit message guidelines

Use Conventional Commits with repo-appropriate scopes. This keeps history scannable and makes release and changelog tooling possible.

Format (80-char max per line):
- `type(scope): imperative subject`

Types:
- `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `chore`, `build`, `ci`, `revert`, `style`

Scopes (suggestions; keep short and meaningful):
- `core`, `path`, `layer`, `task`, `type`, `tests`, `docs`, `build`, `scripts`, `log`

Subject:
- Imperative mood, present tense (e.g., “add”, “fix”, “improve”)
- Keep concise; subject line must be ≤ 80 characters

Body:
- Explain what and why (not just how)
- Mention concurrency and performance considerations when relevant
- Wrap all body lines to ≤ 80 characters (hard limit)

Footers (as needed):
- `Breaking-Change: <description>`
- `Refs: #123`, `Fixes: #123`
- `Co-authored-by: Name <email>`

Examples (good commit messages):
- `fix(iterator): rebind views/iterators to local storage for safe copies`
- `perf(waitmap): reduce notify scan with concrete-path fast path`
- `refactor(path): unify iterator types and remove string_view end() usage`
- `docs(overview): document compile_commands.json refresh workflow`
- `build(scripts): prefer Ninja and auto-parallelize by CPU core count`
- `test(multithreading): shorten perf case without reducing coverage`
- `chore(logging): gate SP_LOG_DEBUG output behind PATHSPACE_LOG`
- `feat(layer): add alias retarget notification path forwarding`
- `fix(task): handle timeout edge-case in wait loop with minimal lock time`
- `docs(workflow): add PR template and commit message guidelines`

---

## Documentation expectations

See `docs/.rules` for authoritative guidance. Highlights:

- If a change affects core behavior (paths, NodeData, WaitMap, TaskPool, serialization), update:
  - `docs/AI_ARCHITECTURE.md`
- If a change affects build or scripts (`CMake`, `scripts/*`), update relevant build/test documentation accordingly.
- Images/diagrams go to `docs/images/` (SVG preferred; keep them small and legible).
- Keep references stable (full repo-relative paths); update docs in the same PR if files moved.

---

## Pull Request template and review

The PR template (auto-applied on GitHub) prompts you to:
- Summarize intent and scope
- Check the “core change” and “build/scripts change” doc rules
- Confirm local checks:
  - `./scripts/compile.sh --clean --test --loop=15`
  - `./scripts/update_compile_commands.sh`
- Call out concurrency and performance implications
- Add any test coverage notes

Self-review checklist (quick skim):
- Commit message follows guidelines (type/scope/subject, clear body)
- Core/build/doc rules followed (as applicable)
- Tests pass locally; heavy tests are sensible and not overly long
- Concurrency/perf trade-offs are reasonable for critical paths
- Code is minimal, readable, consistent with existing patterns

---

## Tips for performance and concurrency changes

- Favor minimal lock scope; avoid holding registry or global locks while performing work
- Log (guarded by tags) only when actively debugging; keep logging off by default in tests
- Add short-duration looped tests where timing matters (but keep duration bounded)
- Prefer simple data structures in hot paths; document complexity expectations in PR body

---

## Questions or future improvements

- If you need to add additional scripts for automation (e.g., release notes, formatting), put them under `scripts/` and document usage in repository docs.
- Open an issue or draft PR to discuss substantial architectural changes before landing them.

Thank you for contributing to PathSpace!
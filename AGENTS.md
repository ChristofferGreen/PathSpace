# AGENTS — Quick Workflow Guide

This guide collects the conventions and scripts used when pairing with the PathSpace maintainer. Treat it as the quick-start you consult before touching the repository.

## Quick Checklist
- Confirm the assignment scope and skim `docs/AI_ARCHITECTURE.md` plus any relevant plan documents under `docs/`.
- Sync locally: `git fetch origin` then branch from `origin/master`.
- Keep changes ASCII unless the file already uses Unicode.
- Run the full test suite with the mandated loop/timeout before requesting review.
- Never push directly to `master`; request confirmation before pushing any branch.

## Build Setup
- Configure a `build/` tree before running tests:
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ```
- Re-run `cmake --build build -j` after edits to refresh binaries before the test loop.

## Repository Map
- `src/` — production sources.
- `tests/`, `Testing/` — unit/functional tests managed by CTest.
- `docs/` — architecture notes, renderer plan, path semantics; start with `docs/AI_ARCHITECTURE.md`.
- `scripts/` — automation (`create_pr.sh`, `compile.sh`, tooling helpers).
- `examples/` — small usage demonstrations.

## Architecture Snapshot
- **Core space** — `PathSpace` owns a trie of `Node` objects keyed by path component. Each node holds serialized values plus queued `Task` executions; insert/read/take manage the front element without global locks. Node payloads marshal through `NodeData` (contiguous byte/storage lanes with companion type metadata) using Alpaca today and migrating to C++26 serialization when available (`docs/AI_ARCHITECTURE.md`).
- **Paths** — type-safe `ConcretePath`/`GlobPath` wrappers provide component iterators and globbing (`*`, `**`, ranges). Pattern matching is component-wise for predictable performance (`src/pathspace/path/`).
- **Concurrency & notifications** — children are kept in a sharded concurrent map, while per-node mutexes guard payload updates. Waiters register in concrete and glob registries; notifications flow through a `NotificationSink` token so late completions drop safely during shutdown ("Wait/notify" in `docs/AI_ARCHITECTURE.md`).
- **Executions** — inserts accept callables and scheduling metadata (`In`, `Execution`, `Block`). Tasks can execute immediately or lazily; completion can trigger reinsertions or wake waiters ("Operations" → insert/read/take in `docs/AI_ARCHITECTURE.md`).
- **Layers & views** — `src/pathspace/layer/` hosts permission-checked views (`PathView`), path rewriting mounts (`PathAlias`), and OS/event bridges via PathIO adapters. Enable the macOS backends with `-DENABLE_PATHIO_MACOS=ON` (see `README.md` build options) and review the PathIO backend notes in `docs/AI_ARCHITECTURE.md` when touching input/presenter code; follow the renderer plan for present/publish semantics (`docs/AI_Plan_SceneGraph_Renderer.md`).
- **Canonical namespaces** — `docs/AI_PATHS.md` defines standard system, renderer, and scene subtrees. Keep code/docs in sync when touching surface/target/path conventions.

## Day-to-day Flow
1. **Understand the task** – read the relevant docs and existing code; prefer `rg` for searching.
2. **Create a topic branch** – follow the rules below for naming and always branch off `origin/master`.
3. **Develop** – keep diffs focused; add concise comments only when the code is non-obvious.
4. **Validate** – build and execute tests following the policy in the next section.
5. **Prepare the PR** – update docs alongside code, collect reproduction steps, and stage changes for review.

## Testing Protocol (must follow)
- Do not change tests just to silence failures.
- Always execute the full suite after any code modification (docs-only edits are exempt).
- Run the suite in a loop: minimum 15 iterations with timeout protection.
- Target runtime: < 10 s per iteration; use a 20 s timeout to catch hangs.
- Preferred helper: `./scripts/compile.sh --loop=15 --timeout=20` wraps the CTest invocation and is tuned for the WaitMap/task race scenarios described in `docs/AI_ARCHITECTURE.md`.
- Recommended command (after configuring the `build/` tree):
  ```bash
  ctest --test-dir build --output-on-failure -j --repeat-until-fail 15 --timeout 20
  ```
- If a failure reproduces, capture the failing command and logs for the PR.

## Branching and PR Workflow
- Default branch is `master` (protected). Never commit directly on it.
- Branch naming: `feat/<topic>`, `fix/<topic>`, `perf/<topic>`, `refactor/<topic>`, `docs/<topic>`.
- Standard branch setup:
  ```bash
  git fetch origin
  git checkout -b docs/<topic> origin/master
  git push -u origin docs/<topic>
  ```
- Ask before pushing any branch. Pushing sets up the upstream and avoids PR creation errors.

### PR Quickstart (always target master)
1. Create and push your topic branch (see snippet above).
2. Run the helper script:
   ```bash
   ./scripts/create_pr.sh -b master -t "docs(<topic>): concise title"
   ```
   The script uses `GH_TOKEN`; if unavailable it opens the compare view so you can finish manually.

## Troubleshooting Common Errors
- **"You are on 'master'. Create a topic branch before creating a PR."**  
  Check out a topic branch from `origin/master`, push it, then rerun the PR script.
- **"Head sha can't be blank / Base sha can't be blank / No commits between master and <branch> / Head ref must be a branch"**  
  Ensure the branch is pushed, the PR base is `master`, and `git log --oneline origin/master..HEAD` shows commits.
- **"Branch '<branch>' has no upstream and --no-push was set. PR creation may fail."**  
  Push the branch with `git push -u origin <branch>` or omit `--no-push`.
- **PR shows unrelated older commits**  
  Create a clean branch from `origin/master` and cherry-pick the intended commits:
  ```bash
  git checkout -b docs/<topic>-clean origin/master
  git cherry-pick <commit_sha1> [<commit_sha2> ...]
  git push -u origin docs/<topic>-clean
  ./scripts/create_pr.sh -b master -t "docs(<topic>): concise title"
  ```

## Commit Message Guidelines
- Use Conventional Commits: `type(scope): imperative subject` (≤ 80 characters).
- Allowed types: `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `chore`, `build`, `ci`, `revert`, `style`.
- Suggested scopes: `core`, `path`, `layer`, `task`, `type`, `tests`, `docs`, `build`, `scripts`, `log`.
- Keep bodies wrapped at 80 columns; explain the *what* and *why* when needed.

## Helpful Commands
- Refresh compile commands: `./scripts/update_compile_commands.sh`.
- Rebuild quickly: `cmake --build build -j`.
- Collect logs: `./scripts/run_log.sh`.
- Count lines (sanity check for scope): `./scripts/lines_of_code.sh`.

Stay in sync with `docs/AI_ARCHITECTURE.md` and the related plan documents when touching architecture, path semantics, or rendering logic. Update both the code and documentation in the same branch whenever behaviour changes.

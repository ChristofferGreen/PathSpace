# Pull Request

Thank you for opening a PR. This template helps keep changes reviewable and consistent with our local-first workflow.

## Summary
- What changed and why (short paragraph). Focus on intent and effects.

## Scope
- [ ] Affects core behavior (paths, NodeData, WaitMap, Task/Executor, serialization)
  - If checked: updated both `docs/AI_OVERVIEW.md` and `docs/AI_ARCHITECTURE.md`
- [ ] Affects build or scripts (CMake, `scripts/*`)
  - If checked: updated the "Tests & build" section in `docs/AI_OVERVIEW.md`
- [ ] Adds/updates images or diagrams
  - If checked: placed assets in `docs/images/` and referenced them in docs

## Local Checks (must pass before review)
- [ ] Full rebuild and tests: `./scripts/compile.sh --clean --test --loop=15`
- [ ] Compilation database refreshed: `./scripts/update_compile_commands.sh --no-configure` (or with configure if needed)
- [ ] Pre-push hook path verified locally (optional): `./init-git.sh` and push from a topic branch

## Review Notes
- Concurrency/thread-safety implications (if any):
- Performance implications (complexity, hot paths, memory trade-offs):
- Test coverage added/updated:
- Breaking changes and migration notes (if any):

## Links
- Related issues, design docs, or discussions:

---

## Commit Message Guidelines (C++ project best practices)

Use a clear, conventional format so history is scannable and tooling-friendly. Prefer Conventional Commits style, adapted with scopes relevant to this repo.

Format:
type(scope): imperative subject

Rules:
- type: one of
  - feat, fix, perf, refactor, docs, test, chore, build, ci, revert, style
- scope: a concise area, e.g., `core`, `path`, `layer`, `task`, `type`, `tests`, `docs`, `build`, `scripts`
- subject:
  - imperative mood, present tense (e.g., "add", "fix", "improve")
  - concise; aim ≤ 72 characters
- body:
  - explain what and why (not just how)
  - include rationale, concurrency and performance considerations
  - wrap lines ~72–80 chars for readability
- footers (as needed):
  - `Breaking-Change: <description>`
  - `Refs: #123`, `Fixes: #123`
  - `Co-authored-by: Name <email>` (if applicable)

Examples:
- `fix(iterator): rebind views/iterators to local storage for safe copies`
- `perf(waitmap): reduce notify scan with concrete-path fast path`
- `docs(overview): document compile_commands.json refresh workflow`
- `build(scripts): prefer Ninja and auto-parallelize by CPU core count`
- `refactor(path): unify iterator types and remove string_view end() usage`

When a change affects core behavior (paths, NodeData, WaitMap, TaskPool, serialization), ensure the PR updates both `docs/AI_OVERVIEW.md` and `docs/AI_ARCHITECTURE.md`. For build/script changes, update the "Tests & build" section in `docs/AI_OVERVIEW.md`.

---

## Checklist for the Reviewer (self or human)
- [ ] Commit message follows the guidelines (type/scope/subject, clear body)
- [ ] Core/build/doc rules followed (as applicable)
- [ ] Tests pass locally; heavy tests are sensible and not overly long
- [ ] Concurrency/perf notes are reasonable; no obvious regressions in hot paths
- [ ] Code is readable, minimal, and consistent with existing patterns
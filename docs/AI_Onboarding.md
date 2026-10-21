# PathSpace — AI Onboarding (Fresh Session)

This guide bootstraps a new AI assistant so it can work productively in the PathSpace repository without prior conversational context.

## 1. Scope & Branch Context
1. Read `AGENTS.md` (repository root) for the quick workflow checklist.
2. Run `git status` to ensure the working tree is clean, then `git fetch origin`.
3. Check the active branch with `git branch --show-current`.

## 2. Essential Documentation
1. `docs/AI_Architecture.md` — core architecture, concurrency policies.
2. `docs/AI_Paths.md` — canonical namespace/layout conventions.
3. `docs/Plan_Overview.md` — active plan documents and priorities.
4. `docs/History_Journal_Wiring.md` — undo/history journal wiring.
5. Task-specific plans under `docs/` and archived plans under `docs/finished/`.

## 3. Repository Quick Tour
- `src/pathspace/` — production code (core, history, layers, task, path, type, tools, log, trace).
- `tests/` — doctest suites under `tests/unit/`.
- `bench/` — benchmark harness (`PathSpaceBench`).
- `docs/` — architecture notes and plans.
- `scripts/` — `compile.sh`, `create_pr.sh`, and other helpers.

## 4. Environment Health Check
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./scripts/compile.sh --loop=5 --per-test-timeout 20
```
Investigate and resolve failures before starting new development.

## 5. Working Practices
- Keep edits ASCII unless the file already uses Unicode.
- Run the full compile loop after code changes (docs-only edits may skip).
- Update documentation alongside code when behavior or contracts change.
- When you notice a missing or thin test, land the coverage or record the follow-up in `docs/AI_TODO.task`.

## 6. Rapid Context Refresh
```bash
git status
git branch --show-current
git diff --stat
rg "TODO" -n
```

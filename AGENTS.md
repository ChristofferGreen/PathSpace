# AGENTS — Quick Workflow Guide

This guide collects the conventions and scripts used when pairing with the PathSpace maintainer. Treat it as the quick-start you consult before touching the repository.

## Quick Checklist
- Confirm the assignment scope and skim `docs/AI_Architecture.md`, `docs/Plan_Overview.md`, plus any relevant plan documents under `docs/` (renderer, distributed PathSpace, inspector, web server).
- Keep changes ASCII unless the file already uses Unicode.
- Run the full test suite with the mandated loop/timeout before requesting review.
- Git pushes are gated by the local `pre-push` hook (`scripts/git-hooks/pre-push.local.sh`).
  - By default it runs `./scripts/compile.sh --clean --test --loop=5 --release` (UI enabled) and then builds & smoke-tests `minimal_button_example`.
  - Set `SKIP_LOOP_TESTS=1` or `SKIP_EXAMPLE=1` in the environment if you have explicit maintainer approval to skip those steps.

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
- `docs/` — architecture notes, renderer plan, path semantics; start with `docs/AI_Architecture.md`.
- `scripts/` — automation (`create_pr.sh`, `compile.sh`, tooling helpers).
- `bench/` — benchmark apps (PathSpace hierarchy perf harness).

## Code Guidelines
- **Language/style:** C++20/23 with trailing return types, `auto` for type locality, `std::optional/expected` for error flows, `std::span` for buffers. Prefer RAII and value semantics; avoid raw `new` outside factories.
- **Names:** Prefer descriptive PascalCase/camelCase constants—avoid Hungarian `kPrefix` constants when adding or touching code; keep new constants scoped and self-explanatory. Keep legacy on-disk/wire-format identifiers stable but use the descriptive form in new APIs and helpers.
- **Casing (follow existing code):**
  - Types/structs/classes/enums/aliases: `PascalCase` (`PathSpaceBase`, `VisitOptions`).
  - Namespaces: short `PascalCase` when public (`SP`), otherwise `lower_snake_case` for internal helpers.
  - Functions (free/member/static): `camelCase` (`clone`, `makeValueHandle`, `childLimitEnabled`). Prefer verbs for actions.
  - Static/constexpr constants: `PascalCase` (`UnlimitedDepth`, `NotifyLockWatchdog`); no `k`-prefix.
  - Member fields: `camelCase`; no trailing underscores. Disambiguate with `this->member` when shadowing would occur.
  - Variables/parameters: `camelCase`; reserve `snake_case` for POD interop/layout mirrors or when matching serialized field names.
  - Macros: avoid new ones; if unavoidable, use ALL_CAPS with a clear prefix.
- **Ownership & lifetimes:** Prefer `std::unique_ptr` for sole ownership and `std::shared_ptr` only when multiple owners are intentional. Use references or `std::span` for non-owning access. Document ownership in factories and shared-context helpers.
- **`noexcept` & error flow:** Mark trivially non-throwing helpers `noexcept`; route recoverable failures through `Expected`/`Error` instead of exceptions. Keep exceptions for programmer errors or serialization preconditions.
- **Chrono/timeouts:** Use `std::chrono` types end-to-end; avoid raw integer durations. Favour `steady_clock` for waits/timeouts.
- **Threading discipline:** Keep lock scopes minimal and never hold payload/child locks across user callbacks. When adding waits/timeouts, preserve existing wait/notify semantics and related metrics/log sinks.
- **Includes & deps:** Keep headers lean; prefer forward declarations; order includes as local header, pathspace headers, third_party, STL.
- **Enums & switches:** Use `enum class`; exhaustively switch on enums (no `default` unless asserting/unreachable).
- **Logging:** Use `TaggedLogger` or existing loggers; avoid `std::cout`/`printf` in production paths.
- **Tests first-class:** Add doctests for new behavior (success, failure, concurrency edges); wire new diagnostics/paths into tests so schemas remain enforced.
- **Paths:** Use `Iterator`, `ConcretePath`, `GlobPath`, and `validation.hpp` helpers instead of hand-built strings. Keep names canonical and update `docs/AI_Paths.md` plus schemas when adding/renaming nodes.
- **Storage/serialization:** Route payloads through `NodeData` and typed `InputMetadataT<>`; keep POD fast-paths (`PodPayload`) only when `podPreferred` applies. When migrating POD→generic, preserve error propagation and `InsertReturn` accounting.
- **Concurrency:** Respect per-node mutex scope; do not hold locks across user callbacks. Queue work on `Task/Executor` and avoid blocking the executor threads. Preserve wait/notify by calling `notify()` after observable mutations and wiring `NotificationSink` correctly.
- **APIs & layering:** Public PathSpace APIs live in `include/pathspace/*`; internal helpers stay under `src/pathspace/*`. Keep views/adapters isolated (`layer/`, `distributed/`).
- **Error handling/logging:** Return `Error`/`Expected` instead of bools where context matters; populate codes + messages. Log through `log/TaggedLogger` or existing logger instances—no ad-hoc `std::cout`.
- **Testing hooks:** When adding features, mirror telemetry/paths into `tests/` (doctest) and align with the five-loop harness. Update docs alongside code (plans, schemas, debugging playbook) whenever contracts or diagnostics change.
- **Test naming:** Group doctest cases under dot-delimited suites (use `TEST_SUITE`/`TEST_SUITE_BEGIN` like `TEST_SUITE("core.nodedata.nested")`) and keep per-case titles concise but descriptive.
- **Resources/UI:** Declarative UI code must resolve themes via the runtime (no hard-coded palettes); keep renderer/HTML adapters aligned with snapshot schemas and update SceneGraph plan files when contracts move.

## Architecture Snapshot
- **Core space** — `PathSpace` owns a trie of `Node` objects keyed by path component. Each node holds serialized values plus queued `Task` executions; insert/read/take manage the front element without global locks. Node payloads marshal through `NodeData` (contiguous byte/storage lanes with companion type metadata) using Alpaca today and migrating to C++26 serialization when available (`docs/AI_Architecture.md`).
- **Paths** — type-safe `ConcretePath`/`GlobPath` wrappers provide component iterators and globbing (`*`, `**`, ranges). Pattern matching is component-wise for predictable performance (`src/pathspace/path/`).
- **Concurrency & notifications** — children are kept in a sharded concurrent map, while per-node mutexes guard payload updates. Waiters register in concrete and glob registries; notifications flow through a `NotificationSink` token so late completions drop safely during shutdown ("Wait/notify" in `docs/AI_Architecture.md`).
- **Executions** — inserts accept callables and scheduling metadata (`In`, `Execution`, `Block`). Tasks can execute immediately or lazily; completion can trigger reinsertions or wake waiters ("Operations" → insert/read/take in `docs/AI_Architecture.md`).
- **Layers & views** — `src/pathspace/layer/` hosts permission-checked views (`PathView`), path rewriting mounts (`PathAlias`), and OS/event bridges via PathIO adapters. Enable the macOS backends with `-DENABLE_PATHIO_MACOS=ON` (see `README.md` build options) and review the PathIO backend notes in `docs/AI_Architecture.md` when touching input/presenter code; follow the renderer plan for present/publish semantics (`docs/finished/Plan_SceneGraph_Renderer_Finished.md`).
- **Canonical namespaces** — `docs/AI_Paths.md` defines standard system, renderer, and scene subtrees. Keep code/docs in sync when touching surface/target/path conventions.

## Day-to-day Flow
1. **Understand the task** – read the relevant docs and existing code; prefer `rg` for searching.
2. **Develop** – keep diffs focused; add concise comments only when the code is non-obvious.
3. **Validate** – build and execute tests following the policy in the next section.
4. **Prepare the PR** – update docs alongside code, collect reproduction steps, and stage changes for review.

## Testing Protocol (must follow)
- Do not change tests just to silence failures.
- **No-flake policy:** If any test fails, pause all other work and focus exclusively on making that test reliable. Do not resume feature work or coverage expansion until the test is stable and you are confident in it (use repeated loop runs to verify).
- Always execute the full suite after any code modification (docs-only edits are exempt).
- Run the suite in a loop: minimum 5 iterations with timeout protection.
- Target runtime: < 10 s per iteration; use a 20 s timeout to catch hangs.
- Preferred helper: `./scripts/compile.sh --loop=5 --timeout=20` wraps the CTest invocation and is tuned for the WaitMap/task race scenarios described in `docs/AI_Architecture.md`.
- Recommended command (after configuring the `build/` tree):
  ```bash
  ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20
  ```
- If a failure reproduces, capture the failing command and logs for the PR.

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
- Run hierarchy benchmark: `./build/bench/PathSpaceBench --runs 10 --warmup 1 --scale 1.0`.

Stay in sync with `docs/AI_Architecture.md` and the related plan documents when touching architecture, path semantics, or rendering logic. Update both the code and documentation in the same branch whenever behaviour changes.

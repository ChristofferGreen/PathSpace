# Handoff Notice

> **Drafted:** November 30, 2025 — plan for exposing a public PathSpace visitor API and JSON exporter.

# Plan: PathSpace JSON Export & Visitor API

## Background
- Tooling (inspector, debugging CLIs, remote dashboards) currently copies node data by hand using private trie access. Public APIs only expose `insert/read/take` plus `listChildren`, which cannot discover types or iterate heterogeneous queues.
- `docs/finished/Plan_PathSpace_Inspector_Finished.md` depends on a safe JSON serializer (“JSON Serialization Support”) but no implementation landed. The inspector builder today links directly against private `Node` structures, blocking third-party embedders and layered spaces that cannot expose their roots.
- Declarative samples and diagnostics increasingly need structured snapshots (e.g., paint screenshot cards, undo telemetry). Requiring bespoke code for every subtree is brittle.

## Goals
1. Ship a supported visitor API on `PathSpaceBase` that lets callers walk subtrees without exposing internal `Node`/`NodeData` types and enables the eventual removal of `listChildren`.
2. Provide a built-in JSON exporter (`PathSpaceBase::toJSON(...)`) based on `nlohmann::json`, capable of serializing primitives and user-defined types with explicit opt-in converters.
3. Provide a clear path to deprecate and ultimately remove `PathSpaceBase::listChildren` by migrating internal callers to the visitor API.
4. Keep exports read-only, non-destructive, and bounded (depth/child limits, queue bounds, timeout hooks) so dumping large spaces is predictable.
5. Document the JSON schema, converter extension points, and traversal API so downstream tooling (inspector, CLIs, remote adapters) can rely on it.
6. Cover the feature with unit/functional tests and wire it into the mandated `scripts/compile.sh --clean --test --loop=5 --release` gate via new test targets.

## Non-Goals
- Full fidelity lossless export/import; JSON dumps are observational only (no replay guarantee).
- Changing existing serialization (Alpaca) formats or undo journal payloads.
- Supporting arbitrary binary payloads without caller-provided converters.
- Replacing `InspectorSnapshot` immediately; the new API is the migration path, not a forced rewrite in this plan.

## Deliverables
- `PathSpaceBase::visit(...)` public API with lightweight `PathEntry`/`ValueEntry` facades (no `Node*` leakage).
- `PathSpaceJsonExporter` helper (`src/pathspace/tools/PathSpaceJsonExporter.{hpp,cpp}`) returning structured JSON plus the `PathSpaceBase::toJSON(...)` convenience wrapper.
- Converter registry (`PathSpaceJsonConverters.hpp`) pre-registering built-in primitives (bool, all integer widths, float/double, std::string) and extension hooks for user structs via templates or explicit registration.
- Tests covering traversal limits, JSON output, converter errors, and fallback placeholders.
- Docs: new plan (this file), `docs/AI_Architecture.md` exporter section, `docs/AI_Debugging_Playbook.md` usage notes, `docs/Memory.md` log entry.

## Architecture Overview
```
PathSpaceBase
  ├─ visit(root, visitor, options)
  │    \__ private trie walker (uses getRootNode when available) -> PathEntry + ValueHandle
  └─ toJSON(options)
        \__ PathSpaceJsonExporter (nlohmann::json)
               ├─ Traversal via visit()
               ├─ Value conversion via converter registry
               └─ Placeholder descriptors for unsupported payloads
```

### Visitor API
- `struct VisitOptions { std::string_view root = "/"; std::size_t maxDepth = kUnlimited; std::size_t maxChildren = 256; bool includeNestedSpaces = true; bool includeValues = true; };`
- `enum class VisitControl { Continue, SkipChildren, Stop };`
- `struct PathEntry { std::string_view path; bool hasChildren; bool hasValue; bool hasNestedSpace; std::size_t approxChildCount; DataCategory frontCategory; };`
- `class ValueHandle { public: template <typename T> Expected<T> read() const; Expected<ValueSnapshot> snapshot() const; std::size_t queueDepth() const; };`
- `Expected<void> PathSpaceBase::visit(PathVisitor const&, VisitOptions opts = {});` where `PathVisitor` is `std::function<VisitControl(PathEntry const&, ValueHandle&)>`.
- Implementation respects concurrent writers: lock each node’s `payloadMutex` just long enough to snapshot metadata / copy bytes; visitors never receive raw pointers.

### JSON Exporter
- `struct PathSpaceJsonOptions { VisitOptions visit; std::size_t maxQueueEntries = 4; bool includeOpaquePlaceholders = true; bool includeDiagnostics = true; int dumpIndent = 2; };`
- Output schema per node:
```
{
  "path": "/system/widgets/foo",
  "child_count": 3,
  "values": [
      { "index": 0, "type": "int", "category": "Fundamental", "value": 42 },
      { "index": 1, "type": "ButtonState", "category": "SerializedData", "value": { ... } },
      { "index": 2, "type": "std::function", "category": "Execution", "placeholder": "callable" }
  ],
  "children_truncated": false,
  "nested_space": null
}
```
- Nodes exceeding limits include `"values_truncated": true` / `"children_truncated": true`.
- Execution payloads / futures emit diagnostics (`state`, `category`, `has_future`).
- Nested spaces either inline subtrees (when `includeNestedSpaces`) or leave a placeholder entry referencing the child space type.

### Converter Registry
- `template <typename T> struct PathSpaceJsonConvertible { static constexpr bool enabled = false; };`
- Specializations enable conversion: `PathSpaceJsonConvertible<int>::toJson(int value)` etc.
- Provide helper macro (`PATHSPACE_REGISTER_JSON_CONVERTER(T, Fn)`) to register at static init.
- Built-in converters for bool, signed/unsigned ints, float/double, std::string, Alpaca-friendly structs that also specialize the trait, and PathSpace diagnostic structs already serialized elsewhere (e.g., `WidgetTheme` if needed).
- When no converter exists, exporter emits `{ "placeholder": "opaque", "type": typeid_name }` (still notes element count and category).

## Phases
### Phase 0 — API & Plumbing (Deprecate `listChildren`)
> **Status (November 30, 2025):** `PathSpaceBase::visit` + `ValueHandle` shipped, plus delegation layers (`PathView`, `PathAlias`, `PathSpaceTrellis`, `UndoableSpace`) now forward to the helper. Remaining work in this phase is migrating legacy `listChildren` callers to the visitor facade so `listChildren` can become a thin wrapper.
1. Implement `PathSpaceBase::visit` (default returns `Error::Code::NotSupported`; `PathSpace`/`UndoableSpace` override via shared helper that walks `Node` trie).
2. Provide `VisitOptions`, `PathEntry`, `ValueHandle` definitions in `PathSpaceBase.hpp`.
3. Update `PathSpace`, `PathView`, `PathAlias`, `UndoableSpace`, and `PathSpaceTrellis` to override `visit` by delegating to the helper (ensuring nested spaces are traversed safely).
4. Rework internal callers (focus controller, inspector snapshot, etc.) to prefer `visit`; keep `listChildren` temporarily as a thin wrapper while migration completes.
5. Unit tests verifying traversal order, depth/child truncation, nested space handling, concurrency safety (multiple writers paused via scoped guard in tests).

### Phase 1 — Converter Registry & JSON Export
> **Status (November 30, 2025):** Complete. `PathSpaceJsonExporter`/`PathSpaceJsonConverters` landed with default bool/int/float/string converters, the `PathSpaceBase::toJSON` wrapper, and the new doctest coverage in `tests/unit/pathspace/test_PathSpaceJsonExporter.cpp`. Remaining work moves to Phase 2 integrations.
1. Add converter trait + registry with built-in primitives; expose extension API.
2. Implement `PathSpaceJsonExporter` (builds JSON tree using `visit`).
3. Add `PathSpaceBase::toJSON(PathSpaceJsonOptions const&) -> Expected<std::string>` convenience wrapper for CLIs/tests.
4. Tests covering:
   - Primitive value export (ints, floats, bools, strings).
   - Heterogeneous queues with limit/truncation.
   - Execution payload placeholder.
   - User converter registration (define test struct + specialization).
   - Nested spaces + include/exclude toggle.

### Phase 2 — Integrations & Tooling
> **Status (November 30, 2025):** Inspector snapshotting now rides the visitor API, `pathspace_dump_json` ships as the supported exporter CLI, and the new `PathSpaceDumpJsonDemo` test exercises the CLI against a pinned fixture. Remaining work tracks documentation updates plus downstream consumers (inspector UI, distributed mounts).
1. Migrate `InspectorSnapshot` to consume the visitor API (drop direct `Node*` usage).
2. Add CLI (`tools/pathspace_dump_json.cpp`) to exercise `toJSON` with options (root path, depth, include values, output file).
3. Update docs and onboarding to reference the new API, and cross-link with `docs/finished/Plan_PathSpace_Inspector_Finished.md` / `docs/finished/Plan_Distributed_PathSpace_Finished.md`.
4. Optional: add test ensuring `pathspace_dump_json` output matches expected fixtures.

## Validation & Testing
- Unit tests under `tests/unit/pathspace/test_PathSpaceVisit.cpp` and `tests/unit/pathspace/test_PathSpaceJsonExporter.cpp`.
- Integration test: `PathSpaceDumpJsonDemo` runs `pathspace_dump_json` against the seeded demo tree and diffs the JSON against `tests/fixtures/pathspace_dump_json_demo.json` to guarantee deterministic output.
- Performance micro-benchmarks for `visit` on a large synthetic trie to ensure traversal remains O(n) with minimal allocations.
- CI: extend `scripts/compile.sh` to build the new CLI and run the added tests within the mandated loop.

## Documentation Updates
- This plan file (`docs/finished/Plan_PathSpaceJsonExport_Finished.md`).
- `docs/AI_Architecture.md`: add “PathSpace traversal & JSON export” section referencing the visitor API and converter registry.
- `docs/AI_Debugging_Playbook.md`: add instructions for `pathspace_dump_json`, expected JSON schema, troubleshooting tips for missing converters.
- `docs/Memory.md`: capture the rollout once implemented (date, CLI info, schema pointer).
- Update `docs/finished/Plan_PathSpace_Inspector_Finished.md` dependency list to mark “JSON serialization support” as satisfied once Phase 1 ships.

## Risks & Mitigations
- **Traversal locking contention:** mitigate by holding node locks briefly and allowing callers to disable value sampling (`includeValues=false`) for structural-only walks.
- **Unbounded exports:** enforce depth/child/queue limits; surface `truncated` flags so tooling knows results are partial.
- **Type erasure confusion:** converter registry keyed by `std::type_index` plus descriptive placeholders avoids UB when types mismatch.
- **nlohmann::json binary size:** confine includes to exporter implementation; headers expose only strings/options.

## Open Questions
1. Should we expose queue indices individually (per element) or aggregate repeated types? (Default: per element up to `maxQueueEntries`; revisit if size becomes an issue.)
2. Do we need streaming/iterative JSON output for gigantic spaces? (Not in this plan; optional future Phase 3 enhancement.)
3. How should we represent `FutureAny` readiness states? (Proposal: include `{ "placeholder": "execution", "state": "pending/ready" }` in JSON.)

## Maintainer Feedback (November 30, 2025)
- **Visitor ergonomics:** `VisitOptions` now exposes explicit constants for unlimited depth/children plus a `childLimitEnabled()` helper so tooling can disable child caps without magic values. `PathSpaceJsonExporter` propagates that state by emitting `"max_children": "unlimited"` when the limit is disabled, and the CLI help/JSON stats no longer mis-report zero as a hard cap.
- **Converter registry:** The exporter reuses registry-provided type aliases for both value entries and placeholders, and the new `PathSpaceJsonRegisterConverterAs<T>(friendlyName, lambda)` helper lets embedders publish human-readable names instead of mangled `typeid` strings. Default converters now register canonical C++ type names, and the unit tests exercise both aliasing and unlimited-child reporting to lock the behavior down.

## Next Steps
1. ✅ (November 30, 2025) Captured maintainer feedback on visitor ergonomics (unlimited child helpers, clearer JSON stats) and the converter registration API (alias-aware helpers, friendly exporter labels) so downstream plans can rely on the documented surfaces.
2. ✅ (November 30, 2025) Phase 0 API landed (`visit`, `VisitOptions`, `ValueHandle`, delegation layers, unit coverage). Focus remaining Phase 0 work on migrating high-traffic `listChildren` callers to the visitor facade so the legacy helper becomes a shim.
3. ✅ (November 30, 2025) Phase 1 exporter + converter work is live (`PathSpaceJsonExporter`, `PathSpaceJsonConverters`, `PathSpaceBase::toJSON`, and doctest coverage). Keep the registry documented (`docs/AI_Architecture.md`, `docs/AI_Debugging_Playbook.md`) so downstream tooling can rely on the schema.
4. ✅ (November 30, 2025) Phase 2 kickoff delivered the visitor-backed inspector snapshot, the `pathspace_dump_json` CLI, and the fixture-backed regression test. Next deliverables in this phase focus on doc updates plus wiring the inspector/web plans to the exporter contract.
5. ✅ (November 30, 2025) PathSpaceUITests gate restored — `test_BackendAdapters`, `test_HtmlReplay`, and the renderer present suites were failing because PathSurfaceSoftware lost its CPU-buffered fallback when IOSurface allocation was unavailable, so no buffered screenshots ever reached the adapters. Re-enabled the vector-backed staging/front buffers on macOS when IOSurface creation fails, which puts the workflow loop back to green.

# PrimeScript Syntax & Semantics Spec (Draft v0.1)

_Last updated: November 21, 2025 — Maintainer: PathSpace research track_

## Purpose & Scope
PrimeScript unifies gameplay/domain scripting, UI automation, and shader authoring. This document freezes the minimum viable surface syntax and semantic rules required to unblock parser, IR, and backend prototype work described in `docs/Plan_PrimeScript.md`. It captures:

- Lexical rules, token categories, and source file constraints.
- The uniform definition/execution envelope and how transforms rewrite surface code into the canonical IR.
- Type, mutability, and effect semantics shared by every backend (C++, GLSL/SPIR-V, VM).
- Module/include resolution, namespace scoping, and version pinning.
- Status of unresolved design questions.

This spec is intentionally narrow: it focuses on constructs that must be deterministic across every backend. Advanced tooling topics (IDE, incremental compilation, visual editors) remain out of scope.

## Source Files & Encoding
- Files use UTF-8 encoding without BOM. ASCII-only content is strongly preferred until Unicode identifiers ship (see TODOs).
- Newlines are normalized to `\n`. Tabs are prohibited; use spaces for indentation.
- Each file contains zero or more top-level definition blocks. There is no implicit module loader; the include system (see below) controls dependency order.
- File extensions: `.prime` for source, `.primi` for pre-expanded include archives, `.prir` for serialized IR snapshots.

## Lexical Grammar
- **Whitespace** (space, newline) separates tokens but has no semantic meaning.
- **Comments** support `// line` and `/* block */`. Nested block comments are not allowed; the lexer reports an error when encountering an unterminated block.
- **Identifiers** consist of `[A-Za-z_][A-Za-z0-9_]*` and may include namespace separators `::` when qualified. Future Unicode identifiers are explicitly deferred.
- **Literals**
  - Integers: decimal (`123`), hexadecimal (`0xFF`), binary (`0b1010`). Suffixes (`i32`, `u64`) select explicit widths. All literals are exact; overflow raises a compile-time error.
  - Floats: decimal (`1.0`, `0.5f`, `3e-2`). Suffixes (`f32`, `f64`) choose precision. Backend lowering rejects unsupported widths (e.g., GLSL forbids `f64`).
  - Strings: double-quoted, UTF-8, support the escape set `\n`, `\t`, `\"`, `\\`, `\u{XXXX}`. Raw strings are a TODO.
  - Booleans: `true`, `false`.
- **Keywords**: PrimeScript reserves a minimal keyword set (`return`, `include`, `namespace`, `mut`, `assign`, `execute_if`, `then_block`, `else_block`). The transform pipeline is responsible for expanding higher-level syntactic sugar; anything not explicitly reserved may be used as an identifier.
- **Delimiters**: `[ ]`, `< >`, `( )`, `{ }`, `,`, `::`. Statements terminate on newline/whitespace boundaries; there is no semicolon token anywhere in the language.

## Uniform Envelope
Every construct adheres to:
```
[transform-list] identifier<template-list>(parameter-list) {body}
```
- `transform-list` (optional): whitespace-separated identifiers. Each transform is invoked in order before semantic analysis. Missing brackets imply `[]`.
- `identifier`: may be qualified (`math::vector_add`). The identifier namespace is global; collisions are rejected during semantic analysis.
- `template-list` (optional): comma-separated type or constant tokens. Template arguments feed transforms, not the backend directly.
- `parameter-list`: zero or more comma-separated parameters. Each parameter is `binding:Type` with optional attributes (`[mut]`, `[copy]`, `[restrict]`). Default arguments are not supported yet.
- `body`: either a definition body (statement list) or, when empty, marks the construct as an execution.

### Definitions vs Executions
- **Definition** — a construct with a non-empty body. The compiler emits callable IR artifacts for every backend flagged in the active target set. Example:
  ```
  [return<float> default_operators]
  blend<float>(a:float, b:float) {
    assign(result, multiply(plus(a, b), 0.5f));
    return(result);
  }
  ```
- **Execution** — a construct invoked without a body. Example:
  ```
  [default_operators] execute_add<int>(x:int, y:int)
  ```
  Executions can appear anywhere expressions are allowed. The compiler schedules or inlines them depending on the target backend and effect contracts.

### Stack-value bindings
- Local bindings are introduced via ordinary executions that instantiate a type in the current frame: `[float] exposure(1.0f)` allocates and initializes a stack slot named `exposure`.
- `mut` inside the bracket list marks the execution as writable so later `assign` calls may target that binding (`[float mut] exposure(1.0f)`). When a definition has no explicit type token (e.g., nested helpers), `[mut] helper()` marks the helper itself as stateful.
- Statements end at newline boundaries—there is no semicolon token in PrimeScript. Blocks group statements using `{ … }` as in the uniform envelope.
- Namespace blocks mirror C++ inheritance: `namespace demo { … }` automatically qualifies enclosed identifiers (`demo::hello_values`). The parser treats namespace declarations as part of the uniform envelope so transforms can inspect them when generating metadata.

## Transform Pipeline
1. Parse-time validates brackets and identifier names but defers semantics to transforms.
2. The ordered transforms list is applied to the raw AST. Built-ins include:
   - `default_operators` — desugars infix math/logical operators into prefix calls (`a + b` → `plus(a, b)`).
   - `control_flow` — lowers `if/else`, `while`, `for`, and short-circuit expressions into canonical `execute_if`, `loop`, or `dispatch` calls.
   - `desugar_assignment` — replaces `=` and compound assignments with explicit `assign` and effect-checked pointer writes.
   - `return<T>` — annotates the function’s return type and ensures every path issues `return(value)`.
   - `effects(...)` — attaches explicit side-effect metadata.
   - `struct`, `pod`, `stack`, `heap`, `buffer` — metadata-only tags that validate contracts (layout rules, placement targets) without changing the underlying syntax. They emit diagnostics when violated.
3. Custom transforms: `primescriptc --transform-list=path/to/list.pritr` loads project-specific passes. Each entry is validated to prevent reordering built-in passes unless the project marks them `overrideable` in the manifest.
4. Transform failures are fatal and must emit deterministic diagnostics (error code, message, source span, transform name).
5. After transforms run, the AST is canonical: no infix operators, no statements that require further desugaring, and all effect/mutability annotations are explicit.

## Types & Mutability
- Scalar types: `bool`, `int{8,16,32,64}`, `uint{8,16,32,64}`, `float{16,32,64}`.
- Vector/matrix types: spelled `vec<N><T>` and `mat<RxC><T>`; transforms enforce GPU backend limits.
- References follow the borrow rules:
  - Bindings are immutable by default.
  - `mut` marks a binding as writable.
  - `[copy]` requests by-value capture; `[restrict]` promises exclusive access and enables backend-specific aliasing optimizations.
- Structs are user-defined (see below). Fields/lanes default to immutable; include `mut` in the stack-value execution when writes are required.
- Type inference is limited to local bindings inside definitions. Top-level signatures must name every parameter and return type.

### Struct & Placement Transforms
- `[struct ...]` is an optional transform layered onto the normal definition envelope. It records a layout manifest (field names, types, offsets, attributes) while ensuring the body satisfies struct rules (all lanes initialized before executable statements, no early returns before initialization, etc.). Removing the transform leaves syntax unchanged but skips the extra validation.
- `[pod]` is another optional transform: it asserts trivially-copyable semantics (no handles, async captures, or hidden lifetimes). Violations emit diagnostics; untagged definitions behave permissively.
- Placement transforms declare where a manifest may live:
  - `[stack]` — legal to instantiate on the stack; attempting to heap/gpu-instantiate raises an error.
  - `[heap allocator=arena::frame]` — generate heap-friendly constructors/destructors for the named allocator.
  - `[buffer kind=uniform std=std140]` — emit metadata for GPU buffer layouts; fails if the manifest violates std rules.
  All placement tags examine the same manifest emitted by `[struct]` (or synthesized implicitly if `[struct]` was omitted).
- Fields/lanes are declared using the usual stack-value syntax inside the body:
  ```
  namespace demo {
    [struct pod stack align_bytes(16)]
    color_grade() {
      [float] exposure(1.0f)
      [float3 mut] tone_curve(0.8f, 0.9f, 1.1f)
      [handle<PathNode>] target(default_target())
    }
  }
  ```
  Each `[Type] name(args)` line becomes a manifest entry with `{name, type, initializer, attributes}`. `mut`, `[align_bytes(n)]`, `[handle<...>]`, etc. simply decorate the execution.
- Layout metadata emitted to IR must include `total_size_bytes`, `alignment_bytes`, and ordered `fields[]` entries with `{name, type, offset_bytes, size_bytes, padding_kind, category}`. Backends consume the same descriptor; if `[stack]` promised no implicit padding and a backend must inject some, compilation fails.
- Stack instances require every lane to have an initializer. Heap/GPU placements may permit deferred initialization but must encode that choice explicitly in the manifest so borrow/effect checkers can enforce it.

### Lifecycle Helpers (Create/Destroy)
- Nested definitions named `Create`/`Destroy` (or their placement-specific variants) are treated as lifecycle helpers when the enclosing definition is tagged `[struct]`.
- `Create(…)` executes immediately after field initialization; `Destroy(…)` executes just prior to teardown. `this` is implicitly available (mutable by default) so helpers may either modify the instance or stay read-only. Additional parameters map to placement-provided context (allocators, buffer bindings, etc.).
- Placement-specific variants reuse the same naming pattern with suffixes: `CreateStack`/`DestroyStack`, `CreateHeap`, `DestroyBuffer`, etc. When multiple helpers match a placement, the most specific name wins (`CreateStack` before the generic `Create`).
- If no helper is present for a placement, initialization/teardown consists solely of evaluating the field list. `[pod]` requires this trivial path.
- Helpers run in lexical order (constructors top-to-bottom, destructors bottom-to-top) so authors can reason about dependencies without extra syntax.
- Only the documented helper names (`Create`, `Destroy`, `CreateStack`, `DestroyHeap`, etc.) receive special handling; other uppercase identifiers behave like ordinary nested definitions.

## Effects & Purity
- Definitions are pure unless decorated with `[effects(...)]`.
- Allowed effects: `global_read`, `global_write`, `io_stdout`, `io_stderr`, `pathspace_insert`, `pathspace_take`, `gpu_texture_write`, etc. The allowed list is versioned; unknown entries fail the build.
- Backends validate effect compatibility (e.g., GLSL forbids `io_stdout`). When a backend cannot honor an effect, compilation fails with a diagnostic referencing the offending definition.

## Module & Include System
- `include<"/std/io", version="1.2.0">` statements must appear at the top of a file before any definitions.
- Include path resolution:
  1. Extract the version tuple. `1` or `1.2` select the latest matching archive; `1.2.0` requires an exact match.
  2. Search the include roots for either a zipped archive (`.pspkg`) or directory whose layout mirrors `/version/namespace/...`.
  3. Inline each `.prime` exactly once. Duplicate includes are ignored after the first expansion.
- Namespaces follow the on-disk path. Files under `std/io/*.prime` register identifiers under `std::io::*`.
- Paths prefixed with `_` remain private: their contents are still parsed (to satisfy dependencies) but not exported.

## Control Flow Canonical Form
- `if`, `else if`, `else` are rewired into `execute_if<bool>(cond, then_block{...}, else_block{...})`.
- Loops map to `loop` constructs:
  - `while (cond) { ... }` → `loop(condition_block{ ... }, body_block{ ... })`.
  - `for (init; cond; step) { body }` desugars into explicit blocks executed in order.
- Short-circuit logic uses `select` transforms; `a && b` lowers to `logical_and(a, delayed_block{ b })`.
- Pattern matching is not yet supported; glob dispatch stays in PathSpace host code.

## Diagnostics
- The compiler emits structured diagnostics: `{code, severity, message, source_range, documentation_url}`.
- Every transform and backend registers its error namespace (`PS0001` for parser, `TR1001` for transforms, `BE2001` for backends).
- Diagnostics feed `diagnostics/primeScript/*` metrics described in `docs/Plan_PrimeScript.md`.

## Backend Contracts
- C++ emitter generates deterministic host code using PathSpace helper APIs for IO/task wiring.
- GLSL/SPIR-V emitter enforces GPU-safe subsets (no heap allocation, limited recursion).
- VM bytecode stays canonical to simplify replay/testing. Struct layout metadata is serialized alongside bytecode objects for debuggers.
- All backends consume the same IR; no backend may re-run transforms.

## Open Questions / TODOs
1. Define the exhaustive built-in transform set and document override policy (`docs/Plan_PrimeScript.md` Task 4).
2. Finish composite literal syntax for texture/array initializers (Plan Task 7).
3. Decide VM machine-code/JIT strategy (Plan Task 8) and capture how the spec constrains stack/class features.
4. Specify the diagnostics/tooling handshake (Plan Task 9) for IDE and incremental compilation.
5. Unicode identifiers, raw strings, and pattern matching remain deferred until after the parser prototype stabilizes.
6. Clarify resource-binding descriptors shared by CPU/GPU emitters (Plan Risks: Resource bindings).

---
Status owner: PathSpace maintainers. Update this spec whenever PrimeScript semantics change and log the revision/date in this header.

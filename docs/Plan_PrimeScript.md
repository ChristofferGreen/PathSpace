# Handoff Notice

> **Handoff note (October 19, 2025):** PrimeScript remains exploratory. Treat this document as a living research memo until the language graduates onto the active roadmap.

# PrimeScript Plan (Concept)

PrimeScript is built around a simple philosophy: program meaning emerges from two primitives—**definitions** (potential) and **executions** (actual). Rather than bolting on dozens of bespoke constructs, we give both forms the same syntactic envelope and let compile-time transforms massage the surface into a canonical core. From that small nucleus we can target C++, GLSL, or the PrimeScript VM, wire into PathSpace, and even feed future visual editors, all without sacrificing deterministic semantics.

## Goals
- Single authoring language spanning gameplay/domain scripting, UI logic, automation, and rendering shaders.
- Emit high-performance C++ for engine integration, GLSL/SPIR-V for GPU shading, and bytecode for an embedded VM without diverging semantics.
- Share a consistent standard library (math, texture IO, resource bindings, PathSpace helpers) across backends while preserving determinism for replay/testing.

## Proposed Architecture
- **Front-end parser:** C/TypeScript-inspired surface syntax with explicit types, deterministic control flow, and borrow-checked resource usage.
- **Transform pipeline:** ordered `[transform]` functions rewrite the forthcoming AST (or raw tokens) before semantic analysis. The default chain desugars infix operators, control-flow, assignment, etc.; projects can override via `--transform-list` flags.
- **Intermediate representation:** strongly-typed SSA-style IR shared by every backend (C++, GLSL, VM, future LLVM). Normalisation happens once; backends never see syntactic sugar.
- **Backends:**
  - **C++ emitter** – generates host code or LLVM IR for native binaries/JITs.
  - **GLSL/SPIR-V emitter** – produces shader code; a Metal translation remains future work.
  - **VM bytecode** – compact instruction set executed by the embedded interpreter/JIT.
- **Tooling:** CLI compiler `primescriptc` plus build/test helpers. The definition/execution split maps cleanly to future node-based editors; full IDE/LSP integration is deferred until the compiler stabilises.

## Language Design Highlights
- **Uniform envelope:** every construct uses `[transform-list] identifier<template-list>(parameter-list) {body-list}`. Lists recursively reuse whitespace-separated tokens.
  - `[...]` enumerates metafunction transforms applied in order (see “Built-in transforms”).
  - `<...>` supplies compile-time types/templates—primarily for transforms or when inference must be overridden.
  - `(...)` lists runtime parameters and captures.
  - `{...}` holds either a definition body or, in the execution case, an argument list for higher-order constructs.
- **Definitions vs executions:** definitions include a body (`{…}`) and optional transforms; executions are call-style (`execute_task<…>(args)`) with no body. The compiler decides whether to emit callable artifacts or schedule work based on that presence.
- **Return annotation:** definitions declare return types via transforms (e.g., `[return<float>] blend<…>(…) { … }`). Executions return values explicitly (`return(value)`); the desugared form is always canonical.
- **Effects:** functions are pure by default. Authors opt into side effects with attributes such as `[effects(global_write, io_stdout)]`. Standard library routines permit stdout/stderr logging; backends reject unsupported effects (e.g., GPU code requesting filesystem access).
- **Namespaces & includes:** identifiers follow `namespace::symbol`. `include<"/std/io", version="1.2.0">` searches the include path for a zipped archive or plain directory whose layout mirrors `/version/first_namespace/second_namespace/...`. Versions live in the leading segment (e.g., `1.2/std/io/*.prime` or `1/std/io/*.prime`). If the version attribute provides one or two numbers (`1` or `1.2`), the newest matching archive is selected; three-part versions (`1.2.0`) require an exact match. Each `.prime` source file is inline-expanded exactly once and registered under `std::io::`; duplicate includes are ignored. Folders prefixed with `_` remain private.
- **Transform-driven control flow:** control structures desugar into prefix calls (`if(cond, then_block{…}, else_block{…})`). Infix operators (`a + b`) become canonical calls (`plus(a, b)`), ensuring IR/backends see a small, predictable surface.
- **Mutability:** bindings are immutable by default. Opt into mutation by placing `mut` inside the stack-value execution or helper (`[Integer mut] exposure(42)`, `[mut] Create()`). Transforms enforce that only mutable bindings can serve as `assign` or pointer-write targets.

### Example function syntax
```
namespace demo {
  [return<void>]
  hello_values() {
    [string] message("Hello PrimeScript")
    [i32] iterations(3i32)
    [float mut] exposure(1.25f)
    [float3] tone_curve(0.8f, 0.9f, 1.1f)

    log_line(message)
    assign(exposure, clamp(exposure, 0.0f, 2.0f))
    execute_repeat(iterations) {
      apply_curve(tone_curve, exposure)
    }
  }
}
```
Statements are separated by newlines; semicolons never appear in PrimeScript source.

### Struct & type categories (draft)
- **Struct tag as transform:** `[struct ...]` in the envelope is purely declarative. It records a layout manifest (field names, types, offsets) and validates the body, but the underlying syntax remains a standard definition. Un-tagged definitions may still be instantiated as structs; they simply skip the extra validation/metadata until another transform (e.g. `[stack]`) demands it.
- **Placement tags (`[stack]`, `[heap]`, `[buffer]`):** placement transforms consume the manifest and enforce where the shape may live. `[stack]` means “legal to instantiate on the stack”; attempting to allocate it on the heap triggers a compile-time error. Future tags like `[heap allocator=arena::frame]` or `[buffer std=std140]` follow the same pattern.
- **POD tag as validation:** `[pod]` asserts trivially-copyable semantics. Violations (hidden lifetimes, handles, async captures) raise diagnostics; without the tag the compiler treats the body permissively.
- **Member syntax:** every field is just a stack-value execution (`[float mut] exposure(1.0f)`, `[handle<PathNode>] target(get_default())`). Attributes (`[mut]`, `[align_bytes(16)]`, `[handle<PathNode>]`) decorate the execution, and transforms record the metadata for layout consumers.
- **Baseline layout rule:** members default to source-order packing. Backend-imposed padding is allowed only when the metadata (`layout.fields[].padding_kind`) records the reason; `[no_padding]` and `[platform_independent_padding]` fail the build if the backend cannot honor them bit-for-bit.
- **Alignment transforms:** `[align_bytes(n)]` (or `[align_kbytes(n)]`) may appear on the struct or field; violations again produce diagnostics instead of silent adjustments.
- **Stack value executions:** every local binding—including struct “fields”—materializes via `[Type qualifiers…] name(args)` so stack frames remain declarative (e.g., `[float mut] exposure(1.0f)`). Default expressions are mandatory for `[stack]` layouts to keep frames fully initialized.
- **Lifecycle helpers (Create/Destroy):** Within a struct-tagged definition, nested definitions named `Create` and `Destroy` gain constructor/destructor semantics. Placement-specific variants add suffixes (`CreateStack`, `DestroyHeap`, etc.). Without these helpers the field initializer list defines the default constructor/destructor semantics. `this` is implicitly available inside helpers (mutable by default) so they can either mutate the instance or merely perform side effects such as logging. Add `mut` to the helper’s transform list when it writes to `this`; omit it for pure helpers. We capitalise system-provided helper names so they stand out, but authors are free to use uppercase identifiers elsewhere—only the documented helper names receive special treatment.
  ```
  namespace demo {
    [struct pod stack]
    color_grade() {
      [float mut] exposure(1.0f)

      [mut]
      Create() {
        assign(this.exposure, clamp(this.exposure, 0.0f, 2.0f))
      }

      Destroy() {
        log_line("color grade destroyed")
      }
    }
  }
  ```
- **IR layout manifest:** `[struct]` extends the IR descriptor with `layout.total_size_bytes`, `layout.alignment_bytes`, and ordered `layout.fields`. Each field record stores `{ name, type, offset_bytes, size_bytes, padding_kind, category }`. Placement transforms consume this manifest verbatim, ensuring C++, VM, and GPU backends share one source of truth.
- **Categories:** `[pod]`, `[handle]`, `[gpu_lane]` tags classify members for borrow/resource rules. Handles remain opaque tokens with subsystem-managed lifetimes; GPU lanes require staging transforms before CPU inspection.
- **Documentation TODO:** finalise how manifest categories map to PathSpace storage classes and ensure placement transforms emit consistent diagnostics when tags conflict (`[stack]` + `[gpu_lane]`, etc.).

### Built-in transforms (draft)
- **Purpose:** built-in transforms are metafunctions that stamp semantic flags on the AST; later passes (borrow checker, backend filters) consume those flags. They do not emit code directly.
- **Evaluation mode:** when the compiler sees `[transform ...]`, it routes through the metafunction's declared signature—pure token rewrites operate on the raw stream, while semantic transforms receive the AST node and in-place metadata writers.
- **`copy`:** force copy-on-entry for a parameter or binding, even when references are the default. Often paired with `mut`.
- **`mut`:** mark the local binding as writable; without it the binding behaves like a `const` reference.
- **`restrict<T>`:** constrain the accepted type to `T` (or satisfy concept-like predicates once defined). Applied alongside `copy`/`mut` when needed.
- **`return<T>`:** optional contract that pins the inferred return type. Recommended for public APIs or when disambiguation is required.
- **`effects(...)`:** declare side-effect capabilities; absence implies purity. Backends reject unsupported capabilities.
- **`align_bytes(n)`, `align_kbytes(n)`:** encode alignment requirements for struct members and buffers. `align_kbytes` applies `n * 1024` bytes before emitting the metadata.
- **Scheduling helpers:** `stack_arena("id")`, `runner("hint")`, `capabilities(...)` reuse the same transform plumbing to annotate execution metadata.
- **`struct`, `pod`, `stack`, `heap`, `buffer`:** declarative tags that emit metadata/validation only. They never change syntax; instead they fail compilation when the body violates the advertised contract (e.g., `[stack]` forbids heap placement, `[pod]` forbids handles/async fields).
- **Documentation TODO:** ship a full catalog of built-in transforms once the borrow checker and effect model solidify; this list captures the current baseline only.

### Core library surface (draft)
- **`assign(target, value)`:** canonical mutation primitive; only valid when `target` carried `mut` at declaration time.
- **`plus`, `minus`, `multiply`, `divide`:** arithmetic wrappers used after operator desugaring.
- **`clamp(value, min, max)`:** numeric helper used heavily in rendering scripts.
- **`execute_if<Bool>(cond, then_block{…}, else_block{…})`:** canonical conditional form after control-flow desugaring.
- **`notify(path, payload)`, `insert`, `take`:** PathSpace integration hooks for signaling and data movement.
- **`return(value)`:** explicit return primitive; implicit `return(void)` fires at end-of-body when omitted.
- **Documentation TODO:** expand this surface into a versioned standard library reference before PrimeScript moves onto an active milestone.

## Runtime Stack Model (draft)
- **Frames:** each execution pushes a frame recording the instruction pointer, constants, locals, captures, and effect mask. Frames are immutable from the caller’s perspective; `assign` creates new bindings.
- **Deterministic evaluation:** arguments evaluate left-to-right; `return(value)` unwinds automatically. Implicit `return(void)` fires if the body reaches the end.
- **Transform boundaries:** rewrites annotate frame entry/exit so the VM, C++, and GLSL backends share a consistent calling convention.
- **Resource handles:** PathSpace references/handles live inside frames as opaque values; lifetimes follow lexical scope.
- **Tail execution (planned):** future optimisation collapses tail executions to reuse frames (VM optional, GPU required).
- **Effect annotations:** purity by default; explicit `[effects(...)]` opt-ins. Standard library defaults to stdout/stderr effects.
- **Future: stack arenas (optional):** exploring named stack arenas (launch executions on specific stacks, clone/snapshot stacks, resume in parallel). Deferred until after v1; flagged as an advanced runtime capability needing copy semantics + effect-safety rules.

### Execution Metadata (draft)
- **Placement:** executions may annotate a target stack arena (`[stack_arena("physics")] execute_task<…>(…)`). Absent an annotation, they run on the default stack.
- **Scheduler affinity:** optional hint for the runtime thread/fiber executing the frame (`[runner("render-thread")]`). Backends can ignore hints they cannot satisfy.
- **Capabilities:** effect masks double as capability descriptors (IO, global write, GPU access, etc.). Additional attributes can narrow capabilities (`[capabilities(io_stdout, pathspace_insert)]`).
- **Instrumentation:** executions carry metadata (source file/line, stack id, runner hint) for diagnostics and tracing.
- **Open design items:** finalise attribute syntax, scheduling semantics, and enforcement rules for stack/runner hints across VM, C++, and GLSL backends.

## Type & Class Semantics (draft)
- **Structural classes:** `[return<void>] class<Name>(members{…})` desugars into namespace `Name::` plus constructors/metadata. Instances are produced via constructor executions.
- **Composition over inheritance:** “extends” rewrites replicate members and install delegation logic; no hidden virtual dispatch unless a transform adds it.
- **Generics:** classes accept template parameters (`class<Vector<T>>(…)`) and specialise through the transform pipeline.
- **Interop:** generated code treats classes as structs plus free functions (`Name::method(instance, …)`); VM closures follow the same convention.
- **Open design items:** decide field visibility syntax, static/constant member handling, and constructor semantics once package and effect designs settle.

## Lambdas & Higher-Order Functions (draft)
- **Syntax mirrors definitions:** lambdas omit the identifier (`[capture] <T>(params){ body }`). Captures rewrite into explicit parameters/structs.
- **Capture semantics:** support `[]`, `[=]`, `[&]`, and explicit notations (`[value x, ref y]`). Captures compile to generated structs with `invoke` methods.
- **First-class values:** closures are storable, passable, and returnable. Backends emit them as struct + function pointer (C++), inline function objects (GLSL, where legal), or closure objects (VM).
- **Inlining transforms:** standard transforms may inline pure lambdas; async/task-oriented lambdas stay as closures.
- **PathSpace interop:** captured handles respect frame lifetimes; transforms force reference-count or move semantics as required.

## Literals & Composite Construction (draft)
- **Numeric literals:** decimal, float, hexadecimal with optional width suffixes (`42u32`, `1.0f64`).
- **Strings:** quoted with escapes (`"…"`) or raw (`R"( … )"`).
- **Boolean & null:** keywords `true`, `false`, `null` map to backend equivalents.
- **Composite constructors:** structured values are introduced through standard type executions (`ColorGrade(hue_shift = 0.1f, exposure = 0.95f)`) or helper transforms that expand the uniform envelope. Named arguments map to fields, and every field must have either an explicit argument or a placement-provided default before validation.
- **Collections:** `array<Type>{ … }`, `map<Key,Value>{ … }` (or bracket sugar) rewrite to standard builder functions.
- **Conversions:** no implicit coercions. Use explicit executions (`convert<float>(value)`) or custom transforms.
- **Mutability:** values immutable by default; include `mut` in the stack-value execution to opt-in (`[float mut] value(...)`).
- **Open design:** finalise literal suffix catalogue, raw string semantics across backends, and the composite-constructor defaults/validation rules.

## Pointers & References (draft)
- **Explicit types:** `Pointer<T>`, `Reference<T>` mirror C++ semantics; no implicit conversions.
- **Operator transforms:** dereference (`*ptr`), address-of (`&value`), pointer arithmetic desugar to canonical calls (`deref(ptr)`, `address_of(value)`, `pointer_add(ptr, offset)`).
- **Ownership:** references are non-owning, frame-bound views. Pointers can be tagged `raw`, `unique`, `shared` via transform-generated wrappers around PathSpace-aware allocators.
- **Raw memory:** `memory::load/store` primitives expose byte-level access; opt-in to highlight unsafe operations.
- **Layout control:** attributes like `[packed]` guarantee interop-friendly layouts for C++/GLSL.
- **Open design:** pointer qualifier syntax, aliasing rules (restrict/readonly), and GPU backend constraints remain TBD.

## VM Design (draft)
- **Instruction set:** ~50 stack-based ops covering control flow, stack manipulation, memory/pointer access, optional coroutine primitives. No implicit conversions; opcodes mirror the canonical language surface.
- **Frames & stack:** per-call frame with IP, constants, locals, capture refs, effect mask; tail calls reuse frames. Data stack stores tagged `Value` union (primitives, structs, closures, buffers).
- **Bytecode chunks:** compiler emits a chunk (bytecode + const pool) per definition. Executions reference chunks by index; constant pools hold literals, handles, metadata.
- **Native interop:** `CALL_NATIVE` bridges to host/PathSpace helpers via a function table. Effect masks gate what natives can do.
- **Closures:** compile to closure structs (chunk pointer + capture data). Captured handles obey lifetime/ownership rules.
- **Optimisation:** reference counting for heap values; optional chunk caching; future LLVM-backed JIT can feed on the same bytecode when needed.
- **Deployment target:** the VM serves as the sandboxed runtime for user-supplied scripts (e.g., on iOS) where native code generation is unavailable. Effect masks and capabilities enforce per-platform restrictions.

## Examples (sketch)
```
// Pull std::io at version 1.2.0
include<"/std/io", version="1.2.0">

[return<int> default_operators] add<int>(a, b) { return(plus(a, b)); }

[default_operators] execute_add<int>(x, y)

clamp_exposure(img) {
  execute_if<bool>(
    greater_than(img.exposure, 1.0f),
    then_block{ notify("/warn/exposure", img.exposure); },
    else_block{ }
  );
}

tweak_color([copy mut restrict<Image>] img) {
  assign(img.exposure, clamp(img.exposure, 0.0f, 1.0f));
  assign(img.gamma, plus(img.gamma, 0.1f));
  apply_grade(img);
}

[return<float> default_operators control_flow desugar_assignment]
float blend(float a, float b) {
  float result = (a + b) * 0.5f;
  if (result > 1.0f) {
    result = 1.0f;
  }
  return result;
}

// Canonical, post-transform form
[return<float>] blend<float>(a, b) {
  assign(result, multiply(plus(a, b), 0.5f));
  execute_if<bool>(
    greater_than(result, 1.0f),
    then_block{ assign(result, 1.0f); },
    else_block{ }
  );
  return(result);
}
```

## Integration Points
- **Build system:** extend CMake/tooling to run `primescriptc`, track dependency graphs, and support incremental builds.
- **PathSpace Scene Graph:** helper APIs map PrimeScript modules to scenes/renderers and manage path-based resource bindings.
- **Testing:** unit/looped regression suites verify backend parity (C++, VM, GLSL).
- **Diagnostics:** metrics/logs land under `diagnostics/primeScript/*`. Effect annotations drive error messaging.
- **PathSpace runtime wiring:** generated code uses PathSpace helper APIs (`insert`, `take`, `notify`). Transforms wrap high-level IO primitives so emitted C++/VM code interacts through typed handles; GLSL outputs map onto renderer inputs. Lifetimes/ownership align with PathSpace nodes.

## Dependencies & Related Work
- Stable IR definition & serialization (std::serialization once available).
- Scene graph/rendering plans (`docs/Plan_SceneGraph_Renderer.md`).
- PathIO/device plans for IO abstractions (`docs/AI_Paths.md`).

## Risks & Research Tasks
- **Memory model** – value/reference semantics, handle lifetimes, POD vs GPU staging rules.
- **Resource bindings** – unify descriptors across CPU/GPU backends.
- **Debugging** – source maps, stack inspection across VM/GPU.
- **Performance** – ensure parity with handwritten code; benchmark harnesses.
- **Adoption** – migration strategy from existing shaders/scripts.
- **Diagnostics** – map compile/runtime errors back to PrimeScript source across backends.
- **Concurrency** – finalise coroutine/await integrations with PathSpace scheduling.
- **Security** – sandbox policy for transforms/archives.
- **Tooling** – IDE/LSP roadmap, incremental compilation caches, metadata outputs.

## Validation Strategy
- Golden tests comparing outputs from C++, VM, GLSL for shared modules.
- Performance benchmarks recorded under `benchmarks/`.
- Static analysis/lint integrated into CI to catch undefined constructs before codegen.

## Next Steps (Exploratory)
1. Draft detailed syntax/semantics spec and circulate for review. _(Draft v0.1 captured in `docs/PrimeScript_SyntaxSpec.md` on 2025-11-21; review/feedback tracking TBD.)_
2. Prototype parser + IR builder (Phase 0).
3. Evaluate reuse of existing shader toolchains (glslang, SPIRV-Cross) vs bespoke emitters.
4. Design import/package system (module syntax, search paths, visibility rules, transform distribution).
5. Define library/versioning strategy so include resolution enforces compatibility.
6. Flesh out stack/class specifications (calling convention, class sugar transforms, dispatch strategy) across backends.
7. Lock down composite literal syntax across backends and add conformance tests.
8. Decide machine-code strategy (C++ emission, direct LLVM IR, third-party JIT) and prototype.
9. Define diagnostics/tooling plan (source maps, error reporting pipeline, incremental tooling, future PathSpace-native editor).
10. Document staffing/time requirements before promoting PrimeScript onto the active PathSpace roadmap.

---

*Logged as a research idea on 2025-10-15. Not scheduled for current milestones but tracked for future convergence of scripting and shading.*

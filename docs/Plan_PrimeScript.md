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
- **Mutability:** bindings are immutable by default. Mutability is opt-in via attributes (`[mutable] let x = …`, `[mutable] datum{…}`). Transforms enforce that only mutable bindings can serve as `assign` or pointer-write targets.

### Struct & type categories (draft)
- **Struct definition priority:** before we can classify data lanes or label POD types, lock down how PrimeScript structs are declared, laid out, and mapped onto backend storage. This includes member ordering, padding, default mutability, and transform hooks for layout attributes (`[packed]`, `[align]`).
- **Proposed syntax (exploratory):** treat struct declarations as a transform envelope with no runtime parameters, e.g. `[struct pod no_padding] Color { first_member<Integer> second_member<string> }`. Each member becomes a transform-style execution; additional tags like `mutable`, `default(value)` or `handle<PathNode>` can decorate the member invocation.
- **Baseline layout rule (suggested):** members default to source-order packing with no implicit padding. A struct tagged `[pod]` may still introduce backend-mandated padding, but only when the layout metadata records the exact reason (e.g., SIMD granularity). `[no_padding]` is an explicit contract—no extra bytes may be inserted; if a backend cannot honour it, compilation fails. `[platform_independent_padding]` locks the final byte layout so every backend emits identical padding bytes, ensuring bit-for-bit portability.
- **Alignment transforms:** member declarations accept layout helpers such as `[align_bytes(8)] color<Float>` or `[align_kbytes(1)] buffer_handle<Handle>`, giving backends explicit padding guarantees without bespoke syntax.
- **Stack value executions:** every local binding is introduced through an execution that materialises a value for the current frame’s stack, e.g. `[Integer] exposure(42)` or `[Color] tint(default_color())`. A default expression is mandatory so the stack slot is fully initialised before use.
- **Type transforms size the frame:** metafunctions such as `Integer` rewrite the AST with sizing metadata so the compiler can reserve the correct stack width before lowering to IR. Custom types are expected to expose the same metadata hooks.
- **Platform constraints:** each backend may enforce minimum alignment/padding for certain types (SIMD lanes, GPU storage buffers); the struct metadata must record both the requested layout and the runtime-imposed adjustments so high-speed code stays portable.
- **IR layout manifest:** struct transforms embed a `layout` table directly into the IR type descriptor (field name → offset, size, padding flag). Backends consume that single source of truth; layout validation tools read it via `primescriptc --emit-ir`. If `[no_padding]` or `[platform_independent_padding]` cannot be honoured, codegen aborts during IR validation.
  - **Proposed schema:** extend the IR type descriptor with `layout.total_size_bytes`, `layout.alignment_bytes`, and an ordered `layout.fields` array. Each field record carries `{ name, offset_bytes, size_bytes, padding_kind }`, where `padding_kind` is one of `none`, `explicit` (requested by user transforms), or `backend` (platform-imposed with diagnostic details). Struct-level tags (`pod`, `no_padding`, `platform_independent_padding`) translate into validation clauses against these metadata entries.
- **Plain-old data (POD):** trivially copyable scalars/structs with no hidden lifetimes; default pass-by-reference is safe and `[copy]` generates value snapshots.
- **Handles:** opaque identifiers that reference managed resources (PathSpace nodes, textures, etc.). Lifetimes tie to the owning subsystem rather than value semantics; borrow rules must spell out readable vs mutable access.
- **GPU-resident values:** data that only exists in device memory (buffers, textures). Requires explicit staging/copy transforms before CPU inspection.
- **Documentation TODO:** formalise how each category maps onto IR storage classes and backend-specific codegen before promoting PrimeScript beyond research.

### Built-in transforms (draft)
- **Purpose:** built-in transforms are metafunctions that stamp semantic flags on the AST; later passes (borrow checker, backend filters) consume those flags. They do not emit code directly.
- **Evaluation mode:** when the compiler sees `[transform ...]`, it routes through the metafunction's declared signature—pure token rewrites operate on the raw stream, while semantic transforms receive the AST node and in-place metadata writers.
- **`copy`:** force copy-on-entry for a parameter or binding, even when references are the default. Often paired with `mutable`.
- **`mutable`:** mark the local binding as writable; without it the binding behaves like a `const` reference.
- **`restrict<T>`:** constrain the accepted type to `T` (or satisfy concept-like predicates once defined). Applied alongside `copy`/`mutable` when needed.
- **`return<T>`:** optional contract that pins the inferred return type. Recommended for public APIs or when disambiguation is required.
- **`effects(...)`:** declare side-effect capabilities; absence implies purity. Backends reject unsupported capabilities.
- **`align_bytes(n)`, `align_kbytes(n)`:** encode alignment requirements for struct members and buffers. `align_kbytes` applies `n * 1024` bytes before emitting the metadata.
- **Scheduling helpers:** `stack("id")`, `runner("hint")`, `capabilities(...)` reuse the same transform plumbing to annotate execution metadata.
- **Documentation TODO:** ship a full catalog of built-in transforms once the borrow checker and effect model solidify; this list captures the current baseline only.

### Core library surface (draft)
- **`assign(target, value)`:** canonical mutation primitive; only valid when `target` carries the `mutable` flag.
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
- **Placement:** executions may annotate a target stack arena (`[stack("physics")] execute_task<…>(…)`). Absent an annotation, they run on the default stack.
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

## Literals & Data Blocks (draft)
- **Numeric literals:** decimal, float, hexadecimal with optional width suffixes (`42u32`, `1.0f64`).
- **Strings:** quoted with escapes (`"…"`) or raw (`R"( … )"`).
- **Boolean & null:** keywords `true`, `false`, `null` map to backend equivalents.
- **Datum blocks:** `datum<Type>{ field = value }` desugar into constructor executions; type omitted implies structural inference (`datum{ x = 1, y = 2 }`).
- **Collections:** `array<Type>{ … }`, `map<Key,Value>{ … }` (or bracket sugar) rewrite to standard builder functions.
- **Conversions:** no implicit coercions. Use explicit executions (`convert<float>(value)`) or custom transforms.
- **Mutability:** values immutable by default; `[mutable]` attribute required to opt-in.
- **Open design:** finalise literal suffix catalogue, raw string semantics across backends, and whether datum blocks allow nested includes/compile-time evaluation.

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

tweak_color([copy mutable restrict<Image>] img) {
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
1. Draft detailed syntax/semantics spec and circulate for review.
2. Prototype parser + IR builder (Phase 0).
3. Evaluate reuse of existing shader toolchains (glslang, SPIRV-Cross) vs bespoke emitters.
4. Design import/package system (module syntax, search paths, visibility rules, transform distribution).
5. Define library/versioning strategy so include resolution enforces compatibility.
6. Flesh out stack/class specifications (calling convention, class sugar transforms, dispatch strategy) across backends.
7. Lock down literal/datum syntax across backends and add conformance tests.
8. Decide machine-code strategy (C++ emission, direct LLVM IR, third-party JIT) and prototype.
9. Define diagnostics/tooling plan (source maps, error reporting pipeline, incremental tooling, future PathSpace-native editor).
10. Document staffing/time requirements before promoting PrimeScript onto the active PathSpace roadmap.

---

*Logged as a research idea on 2025-10-15. Not scheduled for current milestones but tracked for future convergence of scripting and shading.*

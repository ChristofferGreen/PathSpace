# Handoff Notice

> **Handoff note (October 19, 2025):** PrimeScript remains exploratory. New assistants should treat this as an archived research note and coordinate future updates via `docs/AI_Onboarding_Next.md`.

# PrimeScript Plan (Concept)

> **Context update (October 15, 2025):** Research notes assume the assistant context introduced for this cycle; adapt legacy terminology from prior contexts as needed.

> **Status:** Exploratory. PrimeScript would be a unified scripting/shading language targeting C++, GLSL, and a VM runtime. Not part of the current implementation roadmap; captured here for future research.

## Goals
- Single authoring language for gameplay scripting, UI logic, and rendering shaders.
- Emit high-performance C++ for engine integration, GLSL/SPIR-V for GPU shading, and bytecode for an embedded VM.
- Share a consistent standard library (math, texture IO, ECS bindings) across backends.

## Proposed Architecture
- **Front-end parser:** PrimeScript syntax (inspired by GLSL/TypeScript) with explicit type annotations and deterministic control flow.
- **Intermediate representation:** strongly typed AST → SSA IR suitable for multiple code generators.
- **Backends:**
  - **C++ emitter:** produce headers/source suitable for static or JIT compilation.
  - **GLSL emitter:** produce GLSL/SPIR-V for GPU shaders.
  - **VM bytecode:** compact instruction set for runtime scripting, executed by an embedded interpreter/JIT.
- **Tooling:** CLI compiler, editor plugins with diagnostics, hot-reload pipeline.

## MVP Sketch
1. Implement parser/AST + basic type system.
2. Emit C++ and GLSL for a limited subset (math ops, functions, structs).
3. Add VM interpreter executing the same subset.
4. Gradually expand to full BRDF library, ECS hooks, async scripting features.

## Open Questions
- Memory model and ownership semantics in C++ backend.
- Texture/resource binding abstraction across GLSL/C++.
- Debugging and profiling workflows (source maps, trace hooks).
- Integration with existing build systems (incremental compile, caching).

---

*Logged as a research idea on 2025-10-15. Not scheduled for current milestones but tracked for future convergence of scripting and shading.*

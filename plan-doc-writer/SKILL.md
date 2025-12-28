---
name: plan-doc-writer
description: Create concise, actionable plan docs for engineering changes; include worklog with status icons and instructions to move finished plans into docs/finished/.
---

# Plan Doc Writer

Use this skill to draft or refine a plan document before changing code.

## Template (copy/edit)
- **Title**: Plan: <short objective>
- **Goal**: One–two sentences of desired end state.
- **Current state / problem**: What’s wrong or missing today.
- **Scope / desired behavior**: Briefly describe the desired post-change behavior.
- **Work items**: Bullet list; each prefixed with status icon:
  - ✖ not started, ⏳ in progress, ✔ complete.
  - Include code touch points (files/modules) and any schema/runtime impacts.
- **Validation**: Exact commands to build/test/run + what to check (e.g., `cmake --build build -j`; sample app command + expected paths/values).
- **Acceptance criteria**: Concrete checks proving completion.
- **Work log**: Running list of milestones with status icons and brief notes of what changed.
- **Finish step**: When all work items are ✔, move/rename the plan into `docs/finished/` with `_Finished.md` suffix; note this in the work log.

## Guidance
- Be terse; prefer bullets over prose.
- Always include a worklog and update it as you progress.
- Always include the finish step (move to docs/finished/ when done).
- Call out specific files/areas to touch; note schema/runtime/exporter impacts.
- Include validation with commands and expected observations.
- Note any legacy/compat handling and doc updates.

## Example snippet (adapt)
- **Work items**
  - ✖ Flatten child writes to `<widget>/children/<child>`.
  - ⏳ Move housekeeping to `<widget>/runtime/...` and make it lazy.
  - ✖ Add regression: assert `button_column/children/hello_button` (no extra children layers); no empty `space` nodes.
  - ✖ Docs: update schema reference to flattened children.
- **Work log**
  - ✖ Initial draft.
  - ⏳ Runtime changes started (date, note).
  - ✔ Tests/validation done (date, commands).
  - ✔ Moved to `docs/finished/Plan_<Name>_Finished.md`.
- **Validation**
  - `cmake --build build -j`
  - `./build/declarative_button_example --dump_json > out.json`
  - Verify: labels present; paths `.../children/<child>` (no `children/children`); no empty housekeeping nodes.
  - `./scripts/compile.sh --loop=5 --timeout=20`


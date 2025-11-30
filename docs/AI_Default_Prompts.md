Never create a new git branch, only ever use master.

Use the single prompt below. It covers both the planning and execution phases so Codex can carry the work from start to finish without separate windows.

```
Never create a new git branch, only ever use master.
Coding preference: avoid C++ exceptions entirely. When representing failures, use existing `std::expected`/`Error` returns, status objects, or other explicit error channels instead of `throw`. Introduce exceptions only when integrating unavoidable third-party APIs, and document any such cases.
1. Have a read through of the docs in ./docs.
2. From Current_Plan_Doc, identify the highest-priority unfinished item (follow its own priority markers or sequencing). Explain why it’s next.
3. Produce an implementation plan: scope, affected files/modules, validation/tests (include loop expectations), risks, and required doc updates.
4. Execute the approved plan immediately:
   a. Implement the change.
   b. Update all relevant docs under docs/ to reflect the new status and decisions (mention specific files touched). Summarize remaining TODOs.
   c. If Current_Plan_Doc is complete, verify references (use rg), append _Finished to its name, move it into docs/finished/, and update any docs linking to it.
   d. Write down anything you learn that is of interest in ./docs/Memory.md.
   e. Run `scripts/workflow_commit.sh "type(scope): subject" "body text"` once your changes are ready. The script always executes `cmake --build build -j`, the mandated `./scripts/compile.sh --clean --test --loop=5 --release`, stages every modification, commits with the provided Conventional Commit title/body, and pushes to `origin master`. Both arguments are mandatory so every run records the required metadata.
   f. Provide a final recap: code changes, test evidence, doc updates, next recommended task.
   g. If it was not possible to make a commit due to test failures then update the Current_Plan_Doc with a plan to fix the tests that give an error and make a commit without running tests.
5. No need to ask if the plan looks ok—just do it.
```

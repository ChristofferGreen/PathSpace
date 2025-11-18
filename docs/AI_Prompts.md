Never create a new git branch, only ever use master.
Current_Plan_Doc is ./docs/Plan_WidgetDeclarativeAPI.md
1. Have a read through of the docs in ./docs.
2. From Current_Plan_Doc, identify the highest-priority unfinished item (follow its own priority markers or sequencing). Explain why it’s next.
3. Produce an implementation plan: scope, affected files/modules, validation/tests (include loop expectations), risks, and required doc updates. Ask for confirmation before executing the plan.

Coding preference: avoid C++ exceptions entirely. When representing failures, use existing `std::expected`/`Error` returns, status objects, or other explicit error channels instead of `throw`. Introduce exceptions only when integrating unavoidable third-party APIs, and document any such cases.

1. Implement the change, rebuild (`cmake --build build -j`), execute ./scripts/compile.sh --clean --test --loop=15 --release (unless maintainer-approved skips are set), and report results. Use Conventional Commit format when committing.
2. Update all relevant docs under docs/ to reflect the new status and decisions (mention specific files touched). Summarize remaining TODOs.
3. If Current_Plan_Docis complete, verify references (use rg), append _Finished to its name, move it into docs/finished/, and update any docs linking to it.
4. Write down anything you learn that is of interest in ./docs/Memory.md
5. If we made a new commit we shall perform: "git push origin master" and fix any issues it shows.
6. Provide a final recap: code changes, test evidence, doc updates, next recommended task.

No need to ask if the plan looks ok, just do it.

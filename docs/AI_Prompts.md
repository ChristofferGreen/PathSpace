Never create a new git branch, only ever use master.
Current_Plan_Doc is ./docs/finished/Plan_IOPump_Finished.md
1. Have a read through of the docs in ./docs.
2. From Current_Plan_Doc, identify the highest-priority unfinished item (follow its own priority markers or sequencing). Explain why it’s next.
3. Produce an implementation plan: scope, affected files/modules, validation/tests (include loop expectations), risks, and required doc updates. Ask for confirmation before executing the plan.

1. Implement the change, rebuild (`cmake --build build -j`), execute ./scripts/compile.sh --clean --test --loop=15 --release (unless maintainer-approved skips are set), and report results. Use Conventional Commit format when committing.
2. Update all relevant docs under docs/ to reflect the new status and decisions (mention specific files touched). Summarize remaining TODOs.
3. If Current_Plan_Docis complete, verify references (use rg), append _Finished to its name, move it into docs/finished/, and update any docs linking to it.
4. If we made a new commit we shall perform: "git push origin master" and fix any issues it shows.
5. Provide a final recap: code changes, test evidence, doc updates, next recommended task.

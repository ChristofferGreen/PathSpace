Docs can be found in ./docs such as docs/AI_ARCHITECTURE.md which describe the code architecture of the project.

Do not fix issues with tests failing by changing the tests.
The entire test suite should finish in less than 10 seconds so a 20 second timeout will be enough to discover any hangs.
Never run the test suite without timeout protections
Always run the test suite to verify any code changes to make sure they work.
Always run the test suite on loop with at least 15 iterations to find any multithreading issues.

The sources of the project can be found in ./src
Ask before committing and pushing changes.
You do not need to ask to run tests.

Contributing and PR workflow
- Default branch (protected): master
- Always work on topic branches:
  - feat/<topic> — features
  - fix/<topic> — bug fixes
  - perf/<topic> — performance work
  - refactor/<topic> — internal refactors
  - docs/<topic> — documentation-only changes
- Never commit directly on master. Open PRs from your topic branch into master.

PR quickstart (always to master)
1) Create and push a topic branch:
   - git fetch origin
   - git checkout -b docs/<topic> origin/master
   - git push -u origin docs/<topic>   # sets upstream to origin/<branch>
2) Create the PR:
   - ./scripts/create_pr.sh -b master -t "docs(<topic>): concise title"
   - The script will create the PR via gh/GH_TOKEN, or open the compare page if not available

Troubleshooting (common errors and fixes)
- Error: “You are on 'master'. Create a topic branch before creating a PR.”
  - Fix: git checkout -b docs/<topic> origin/master; git push -u origin docs/<topic>; re-run ./scripts/create_pr.sh -b master
- Error: “Head sha can't be blank / Base sha can't be blank / No commits between master and <branch> / Head ref must be a branch”
  - Cause: branch not pushed or wrong upstream; base not set to master; or branch has no new commits
  - Fix:
    - Push and set upstream: git push -u origin <branch>
    - Ensure base is master: ./scripts/create_pr.sh -b master ...
    - Verify branch is ahead: git log --oneline origin/master..HEAD
- Error: “Branch '<branch>' has no upstream and --no-push was set. PR creation may fail.”
  - Fix: push first (git push -u origin <branch>) or omit --no-push
- PR shows unrelated older commits
  - Fix: create a clean branch from origin/master and cherry-pick:
    - git checkout -b docs<<topic>-clean origin/master
    - git cherry-pick <commit_sha1> [<commit_sha2> ...]
    - git push -u origin docs/<topic>-clean
    - ./scripts/create_pr.sh -b master -t "docs(<topic>): concise title"

Creating a Pull Request (CLI)
- ./scripts/create_pr.sh -b master -t "fix(core): iterator bounds"
- Options:
  - --draft, -r reviewers, -a assignees, -l labels
- gh equivalents:
  - gh pr create --title "fix(core): iterator bounds" --body "…" --base master
  - gh pr view --web

Commit message guidelines (Conventional Commits)
- Format: type(scope): imperative subject
- Types: feat, fix, perf, refactor, docs, test, chore, build, ci, revert, style
- Scopes (suggestions): core, path, layer, task, type, tests, docs, build, scripts, log
- Subject: ≤ 80 chars, imperative mood; body explains what and why; wrap at 80
- Examples:
  - fix(iterator): rebind views/iterators to local storage for safe copies
  - perf(waitmap): reduce notify scan with concrete-path fast path
  - docs(overview): document compile_commands.json refresh workflow

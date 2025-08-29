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
- For authoritative contributor workflow, branching, PR quickstart, troubleshooting, and commit message guidelines, see docs/AI_ARCHITECTURE.md (section “Contributing to PathSpace”). This avoids duplication and keeps rules in one place.

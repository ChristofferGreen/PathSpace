# State Machine Audit

Ledger of the implicit state machines in `src/pathspace` that were audited, the deficiency found in
each, the regression test that demonstrates it and the status of the fix.

Status values: `fixed` (test failed before, passes now), `refuted` (could not be made to fail; no
code change), `open-decision` (test written and skipped; needs a maintainer decision), `not-fixed`
(confirmed, reason given).

## History (`src/pathspace/history`)

### H1 - Journal transaction left open
- What: `UndoableSpace::out` returned early without committing when the journal's "after" capture
  or `recordJournalMutation` failed, and `JournalTransactionGuard`/`HistoryTransaction` had defaulted
  destructors. The root's `activeTransaction` stayed open forever: other threads blocked in
  `beginJournalTransactionInternal`, later edits of the owner nested and never flushed, undo/redo
  reported "transaction open". A moved-from `HistoryTransaction` also still reported itself active.
- Tests: `tests/unit/history/test_UndoableSpaceStateMachine.cpp`
  - `a take that cannot be journaled still closes its transaction`
  - `a HistoryTransaction dropped without commit releases the root`
  - `moving a HistoryTransaction keeps exactly one owner of the open transaction`
- Status: fixed.
- Fix: the guard is now a real RAII owner (move transfers ownership, destruction of an active guard
  commits); `out` commits on every path and reports the journaling error afterwards.
  Design choice: an abandoned transaction commits instead of rolling back, because its mutations
  already reached the inner space and the journal has to keep describing the tree.

### H2 - Undo/redo cursor moved before the step was applied
- What: `performJournalStep` called `journal.undo()/redo()` first; a failed decode/apply left the
  cursor moved while the tree was unchanged, and a multi-step undo that failed midway lost a step.
- Tests: same file
  - `a failed undo step leaves the journal cursor where the tree is`
  - `a failed redo step leaves the journal cursor where the tree is`
  - `a multi-step undo that fails midway reports the steps it applied through the cursor`
- Status: fixed.
- Fix: peek the entry, apply it, then move the cursor.

### H3 - Persisted journal kept records the in-memory journal had dropped
- What: `compactJournalPersistence` only flushed. Records trimmed by retention and the redo tail
  discarded by a new edit after undo stayed in `journal.log`; a restart replayed them, so undone
  edits came back and the file grew without bound.
- Tests: same file
  - `a new edit after undo drops the discarded redo tail from the persisted journal`
  - `retention trims are applied to the persisted journal`
- Status: fixed.
- Fix: `compactJournalPersistence` rewrites the file from the surviving entries through
  `UndoJournal::compactJournal` (closing the writer first). A commit that discards a redo tail
  rewrites instead of appending; loading compacts only when retention trimmed on load.

### H4 - Undo position is not durable
- What: `journal.log` has no record of how many entries are currently undone. An undo that is not
  followed by a new edit is lost on restart: every record is replayed and the redo stack is empty.
  `persistenceDirty` is raised on that path but nothing acts on it; `stateDirty` is written in
  several places and never read.
- Test: same file, `undo position survives a restart` (skipped; fails with `--no-skip`).
- Status: open-decision. Needs an on-disk format decision; options:
  1. store the cursor in the header's reserved `uint32` (rewrite 4 bytes on undo/redo),
  2. append undo/redo marker records (new record kind, entry version bump),
  3. side file next to `journal.log` holding the cursor,
  4. no format change: on undo, rewrite the file with only the applied entries (redo is then not
     available after a restart, but the tree is right).

### H5 - Savefile import left the journal writer on the replaced file
- What: `importHistorySavefile` replaces `journal.log` while `persistenceWriter` still holds the old
  file open, so every later commit was appended to an unlinked file and lost on restart.
- Test: same file, `edits made after a savefile import reach the persisted journal`.
- Status: fixed.
- Fix: the writer is closed before the file is replaced; the next commit reopens it.

## Tasks (`src/pathspace/task`)

### T1 - Waiting on a failed task never returned
- What: `Future::wait`, `wait_until`/`wait_for` and `copy_result_to` looped on `isCompleted()`. A
  task whose callable threw is `Failed`, never `Completed`, so `wait()`/`copy_result_to()` spun
  forever and timed waits spun until their deadline.
- Tests: `tests/unit/task/test_TaskFailureState.cpp`
  - `Future::wait returns when the task failed`
  - `Future::copy_result_to reports a failed task instead of waiting for a result`
  - `Future timed waits end as soon as the task failed`
- Status: fixed.
- Fix: waits end on the terminal state (`Task::isTerminal`), `copy_result_to` returns false for a
  failed task, timed waits return false.

### T2 - Typed future of a throwing `TaskT` blocked forever
- What: `SharedState<T>` only knew "ready"; when the callable threw nobody set a value and
  `FutureT::wait/get` and `FutureAny::wait/copy_to` blocked forever.
- Tests: same file
  - `a throwing TaskT releases the waiters on its typed future`
  - `a failed shared state cannot be fulfilled afterwards`
- Status: fixed.
- Fix: `SharedState<T>` tracks one `Phase {Pending, Ready, Failed}` instead of a ready flag;
  `TaskT` calls `fail()` when the callable throws. `wait()` returns on either final phase, `ready()`
  stays false, `get`/`copy_to` return false.

### T3 - A failed execution blocks its node forever
- What: read/take of a path whose front execution failed keeps returning "Task is not completed"
  (same as still running), blocks until timeout, and everything queued behind it is unreachable.
- Test: same file, `a failed execution does not block its node forever` (skipped; fails with
  `--no-skip`).
- Status: open-decision. Options: (a) read and take return a dedicated error, take also removes the
  failed task; (b) the failed task is dropped from the queue when the failure is observed and the
  error is reported once; (c) store the failure as a value (e.g. `Expected<T>`) so readers receive
  it like a result. All change the documented read/take contract.

### T4 - Concurrent `TaskPool::shutdown` calls joined the same threads
- What: a second `shutdown()` that arrived while the first was joining took the "already shutting
  down" branch and joined the same `std::thread` objects (undefined behaviour; on this platform the
  second caller got a `std::system_error`), while the first went on to clear the vector.
- Test: `tests/unit/task/test_TaskPoolShutdownState.cpp`,
  `two concurrent shutdown calls both return after the workers were joined once`.
- Status: fixed.
- Fix: `shutdown()` is serialised by a dedicated `shutdownMutex`; the second caller waits and then
  finds no workers left.

### T5 - `TaskStateAtomic::markFailed` is a load-then-store
- What: `markFailed` can overwrite `Completed` if `markCompleted` runs between its load and store,
  and it accepts any source state.
- Status: refuted for the application. The only caller is the pool worker's `catch`, on the thread
  that would also have called `markCompleted`, so the two cannot race; no test could be made to
  fail. No change.

## Node payload (`src/pathspace/core`)

### N1 - Execution queues drifted apart
- What: `NodeData` keeps `tasks`, `futures` and `anyFutures` as parallel queues; pops removed from
  the first two only and `anyFutures` only received entries for typed tasks. After a typed
  execution was taken, `read<FutureAny>` returned the previous task's future for the next one and
  `hasExecutionPayload()` stayed true for a node that only held plain values (which made history
  capture reject the node).
- Tests: `tests/unit/core/test_NodeDataExecutionQueues.cpp`
  - `the typed future at a path follows the front execution after a take`
  - `a node stops reporting an execution payload once its executions were taken`
- Status: fixed.
- Fix: `anyFutures` is index-aligned with `tasks` (an invalid handle for untyped tasks) and popped
  with them; `peekAnyFuture` ignores invalid handles.

### N2 - Nested spaces were never destroyed with their node
- What: `NestedSlot::self` was an owning `shared_ptr` to the slot itself. The cycle kept every slot
  alive, so a nested space that was still mounted when its `NodeData` was destroyed (parent
  `clear()`, parent destruction, payload replacement) was leaked: its destructor never ran.
- Tests: `tests/unit/pathspace/test_PathSpaceContextOwnership.cpp`
  - `a mounted nested space is destroyed when its parent is cleared`
  - `a mounted nested space is destroyed with its parent`
  - `a nested space replaced at its node is destroyed`
- Status: fixed.
- Fix: `self` is a `weak_ptr`. The field is kept (not removed) because an existing test
  (`tests/unit/core/test_NodeDataDeserializeEdges.cpp`) assigns it.

### N3 - Generic readers could not read a POD lane of the same type
- What: a node keeps trivially copyable values on a POD lane (`podPayload`) or in generic storage
  (`data`). A reader asking with `podPreferred == false` (everything that goes through
  `UndoableSpace`) got "Type mismatch" for a lane holding exactly the requested type, so values
  inserted into a space before it was wrapped were unreadable through the wrapper.
- Test: `tests/unit/history/test_UndoableSpaceStateMachine.cpp`,
  `values stored on the POD lane before wrapping stay readable through the wrapper`.
- Status: fixed.
- Fix: `Leaf::outAtNode` upgrades a lane of the requested type to generic storage and serves the
  read from there; a lane of another type is still a mismatch.

### N4 - History capture ignored the POD lane
- What: `captureJournalNodeData` looked at `node->data` only. For a node whose values were on the
  POD lane the "before" snapshot was empty, so undoing the first journaled insert wiped the
  pre-existing values too.
- Test: same file, `undo keeps values that were on the POD lane before the journaled edit`.
- Status: fixed.
- Fix: the capture includes the lane's elements (after the generic ones, the order an upgrade
  produces).

### N5 - `migratePodToNodeData` ignores `freezeForUpgrade` and drops drained elements on error
- Status: refuted by reading, no test. The migration runs under `payloadMutex`, so a second freeze
  attempt cannot overlap, and the error branches need `takeTo`/`serialize` to fail for a trivially
  copyable element; no way to provoke that was found. No change.

## PathSpace lifecycle (`src/pathspace/PathSpace.*`, `core/PathSpaceContext.hpp`)

### P1 - clear()/shutdown() could miss an operation in flight
- What: `out()` checked `clearingInProgress` and only then incremented `activeOutCount`, so a
  `clear()` that started in between saw no reader and wiped the tree under it. `in()` did not take
  part at all: an insert could run against a tree that was being freed, or be reported as
  successful and then erased.
- Tests: `tests/unit/pathspace/test_PathSpaceClearHandshake.cpp`
  - `clear waits for a read that was admitted before clear started`
  - `clear waits for an insert that was admitted before clear started`
  - `an insert that arrives during clear lands in the cleared space`
  - `an insert made from inside a read callback does not deadlock with a pending clear` (guards
    the fix itself: the first version of the insert gate deadlocked here; the original code did not)
- Status: fixed for `in()` and `out()`.
- Fix: operations register first and sample the flag second (sequentially consistent on both
  sides); `in()` is counted too and waits for a running clear instead of failing. An insert issued
  from inside another operation of the same thread on the same space (span callback) is admitted
  without waiting, since the outer operation already holds the tree. The insert leaves the
  handshake before it notifies, so observers may call `clear()`. New test hook
  `testing::SetPathSpaceAdmissionHook`.
- Not covered: `packInsert`, `spanPackConst/Mut`, `visit` and `listChildren` still reach the tree
  without registering; `clearingInProgress` is a bool shared by overlapping `clear()` calls.
  Neither could be demonstrated without further hooks; unchanged.

### P2 - Observer notifications dropped while another delivery was in progress
- What: `PathSpaceContext::notify` used one `notifyingSink` flag for all threads as its
  re-entrancy guard. A notification from thread B that arrived while thread A was inside the
  observer was silently dropped (and an observer that threw left the flag set for good).
- Tests: `tests/unit/core/test_PathSpaceContextObserver.cpp`
  - `the observer receives a notification sent while another delivery is in progress`
  - `re-entrant notifications from inside the observer are still dropped per thread` (pins the
    behaviour that had to survive; passed before the fix as well)
- Status: fixed.
- Fix: re-entrancy is tracked per thread (a thread-local stack of contexts being delivered to).

### P3 - A nested space shut down the context it shares with its parent
- What: a nested space adopts its parent's `PathSpaceContext`. Its destructor ran `shutdown()`,
  which marked that shared context as shutting down for good, removed the parent's observer and
  cleared the parent's wait registry. After a nested space was taken out and dropped, every
  blocking read on the parent failed with "Shutting down". (For spaces destroyed while mounted
  this was masked by N2: they were never destroyed.)
- Tests: `tests/unit/pathspace/test_PathSpaceContextOwnership.cpp`
  - `destroying a nested space that was taken out leaves the parent's context running`
  - `clearing a space that contains a nested space leaves its own context running`
  - `replacing a mounted nested space leaves the parent's context running`
  (the last two fail once N2 is fixed without this fix)
- Status: fixed.
- Fix: a space that adopted another space's context (`contextBorrowed`) only tears down its own
  tree; it never shuts the context down or clears its wait registry.

### P4 - `spanPackMut` slept until the first pack insert
- What: `Leaf::packInsertSeen` made every `spanPackMut` call sleep up to 5 x 1 ms on a space that
  had never seen a pack insert.
- Test: `tests/unit/pathspace/test_PathSpaceSpanPackLatency.cpp`,
  `span pack take does not stall on spaces that never saw a pack insert`.
- Status: fixed.
- Fix: the latch and the sleep are removed.

## Layers (`src/pathspace/layer`)

### L1 - Snapshot worker retried a failing rebuild in a tight loop
- What: a failed rebuild left the cache dirty with an unchanged `lastMutation`, so the worker's
  debounce wait returned at once and it rebuilt again (about 2.9 million visits in 150 ms in the
  test). Synchronous rebuild-on-read retried on every read.
- Tests: `tests/unit/layer/test_SnapshotCacheWorkerState.cpp`
  - `a failing rebuild is retried with a backoff, not in a tight loop`
  - `synchronous rebuilds on read back off after a failure`
- Status: fixed.
- Fix: a failed rebuild sets `retryNotBefore = now + max(debounce, 50 ms)`; worker and
  rebuild-on-read honour it.

### L2 - Reconfiguring the snapshot cache clobbered in-flight worker state
- What: `setSnapshotOptions` reset `rebuildInProgress` and `stopWorker`. Resetting the first let a
  second rebuild run concurrently with one in progress; resetting the second cancelled a stop that
  another thread was joining on, which then never returned, and left the cache enabled without a
  new worker.
- Tests: same file
  - `reconfiguring during a rebuild does not let a second rebuild run concurrently`
  - `enabling the cache while its worker is being stopped does not strand the stop`
- Status: fixed.
- Fix: both flags are only cleared by whoever set them; `setSnapshotOptions` bumps the mutation
  counter so a rebuild that predates the new configuration leaves the cache dirty;
  `startSnapshotWorker` waits for a worker that is winding down before starting a new one.

### L3 - BoundedPathSpace counts
- What: (a) when the queue had been drained through the backing space the stale count made every
  later insert "evict" from an empty queue and get dropped, permanently; (b) counts were keyed by
  the raw path string, so a take through `"/q[0]"` was not counted against `"/q"`; (c) an insert
  dropped because eviction failed returned an empty `InsertReturn`.
- Tests: `tests/unit/layer/test_BoundedPathSpaceCounts.cpp`
  - `the bound recovers when the queue was drained through the backing space`
  - `an indexed take through the wrapper is counted against its queue`
  - `an insert dropped because nothing could be evicted is reported`
- Status: fixed.
- Fix: an eviction that finds the queue empty resets the count and inserts; counts are keyed by
  the path without index suffixes; a dropped insert carries an error.
  Design choice: the documented behaviour "the insert is dropped" is kept, the drop is now also
  reported in `InsertReturn::errors`.

## Types (`src/pathspace/type`)

### Y1 - Builtin type registration latch was global
- What: `RegisterBuiltinTypeMetadata` kept a non-atomic `static bool` regardless of the registry
  passed in: a second registry never received the builtins, and concurrent first callers of
  `TypeMetadataRegistry::instance()` could see the registry before it was filled.
- Test: `tests/unit/type/test_TypeMetadataBootstrap.cpp`,
  `builtin registration populates the registry it is given`.
- Status: fixed (the concurrent first-use part is not covered by a test).
- Fix: the function fills whatever registry it is given (registration is idempotent);
  `instance()` uses `std::call_once`.

## Wait registry (`src/pathspace/core/WaitMap.*`)

### W1 - `clear()` destroyed entries under a waiter that registered after the drain
- What: `WaitMap::clear()` waited for the active waiters to reach zero and then, in a separate
  step, destroyed the registry. A waiter registering in between (counted outside the registry
  lock) ended up waiting on a destroyed condition variable and could not be woken any more.
  Reachable through `PathSpace::clear()`/`shutdown()` whenever another waiter shares the context
  (blocking span-pack takes, spaces that borrow the context).
- Test: `tests/unit/core/test_WaitMapClearRace.cpp`,
  `clear does not destroy the registry under a waiter that registered while it drained`.
- Status: fixed.
- Fix: `wait()` counts the waiter while it holds the registry lock; `clear()` re-checks the count
  under that lock and leaves the registry alone when a waiter registered since the drain. New test
  hook `testing::SetWaitMapClearDrainedHook`.
  Design choice: `clear()` does not wait for the late waiter (that could take as long as its
  timeout); the registry is cleaned up by a later `clear()`.

## Existing tests affected

- `tests/unit/pathspace/test_PathSpaceCopy.cpp`: `copy handles nested serialization failures
  gracefully` and `copy skips nested when snapshot restore fails to attach` end with
  `clone.read<int>(..., Block{})` on a path that does not exist and expect no value. They used to
  return at once only because of P3: the nested copy that `clone()` discards on failure shut the
  clone's context down, so every blocking read on the clone failed with "Shutting down". With P3
  fixed the reads wait for their timeout, which for `Block{}` is unbounded outside ctest's 1 s
  clamp (`PATHSPACE_TEST_TIMEOUT=1`). With maintainer approval the two reads now use
  `Block{20ms}`; the assertions are unchanged.
- `tests/unit/core/test_NodeDataDeserializeEdges.cpp` assigns `NestedSlot::self`; the field was
  kept (as a `weak_ptr`) for that reason.

## Examined, no change

- Task state enum (`NotStarted/Starting/Running/Completed/Failed`): transitions are CAS-guarded
  except `markFailed` (T5).
- `PathSpaceTrellis` shutdown latch and round-robin cursor, `PathAlias` prefix swap,
  `TaggedLogger` worker lifecycle, `JournalFileWriter` (reopens lazily after a failure),
  `SlidingBuffer` compaction, `NodeData` indexed and serialized pop paths, `PathSpace::copyFrom`:
  read through, no state/transition defect found beyond the ones listed above.
- `stateDirty` (`UndoJournalRootState`) is written and never read. It has no behaviour to test and
  was left in place; see H4 for `persistenceDirty`.
- Journal entries for indexed paths (`/q[1]`): from reading `captureJournalNodeData`, the literal
  component `q[1]` is looked up, no node is found and an empty before/after pair is recorded, so
  such a take would not be undoable. Seen while working on H1; not tested, not a state-machine
  defect, not pursued.

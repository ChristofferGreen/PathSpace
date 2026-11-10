#define private public
#include "history/UndoableSpace.hpp"
#undef private

#include "PathSpace.hpp"
#include "history/UndoJournalPersistence.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::History;

namespace {

// Inner space that lets a test run code right after a take reached the backing PathSpace, i.e.
// between the wrapper's mutation and its "after" capture. Stands in for a writer that reaches the
// inner space without going through the history wrapper.
struct TakeHookSpace final : PathSpace {
    std::function<void(TakeHookSpace&)> afterTake;

    using PathSpace::insert;

protected:
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj)
        -> std::optional<Error> override {
        auto result = PathSpace::out(path, inputMetadata, options, obj);
        if (!result.has_value() && options.doPop && this->afterTake) {
            auto hook = std::move(this->afterTake);
            this->afterTake = nullptr;
            hook(*this);
        }
        return result;
    }
};

auto uniqueTempRoot(std::string_view label) -> std::filesystem::path {
    auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
           / (std::string("pathspace_history_sm_") + std::string(label) + "_" + std::to_string(stamp));
}

auto journalOptions() -> HistoryOptions {
    HistoryOptions opts;
    opts.useMutationJournal = true;
    return opts;
}

auto persistentOptions(std::filesystem::path const& root, std::string ns) -> HistoryOptions {
    HistoryOptions opts;
    opts.useMutationJournal   = true;
    opts.persistHistory       = true;
    opts.persistenceRoot      = root.string();
    opts.persistenceNamespace = std::move(ns);
    return opts;
}

auto countPersistedEntries(std::filesystem::path const& journalPath) -> std::size_t {
    std::size_t count  = 0;
    auto        replay = UndoJournal::replayJournal(journalPath, [&](UndoJournal::JournalEntry&&) -> Expected<void> {
        ++count;
        return {};
    });
    REQUIRE(replay.has_value());
    return count;
}

} // namespace

TEST_SUITE_BEGIN("history.undoable.statemachine");

TEST_CASE("a take that cannot be journaled still closes its transaction") {
    auto  innerOwned = std::make_unique<TakeHookSpace>();
    auto* inner      = innerOwned.get();
    UndoableSpace space{std::move(innerOwned), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());

    REQUIRE(space.insert("/doc/value", 1).errors.empty());
    REQUIRE(space.insert("/doc/value", 2).errors.empty());

    // An execution appears at the node between the take and the journal's "after" capture, which
    // makes the capture fail after the transaction was opened.
    inner->afterTake = [](TakeHookSpace& backing) {
        auto ret = backing.insert("/doc/value", [] { return 7; }, In{.executionCategory = ExecutionCategory::Lazy});
        REQUIRE(ret.errors.empty());
    };
    auto taken = space.take<int>("/doc/value");
    CHECK_FALSE(taken.has_value());

    auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
    REQUIRE(state);
    {
        std::scoped_lock lock(state->mutex);
        CHECK_FALSE(state->activeTransaction.has_value());
    }
    // With the transaction left open every history command reports "transaction open" for this
    // thread and every other thread blocks forever in beginJournalTransactionInternal.
    auto undo = space.undo(ConcretePathStringView{"/doc"});
    CHECK(undo.has_value());
}

TEST_CASE("a HistoryTransaction dropped without commit releases the root") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());

    {
        auto tx = space.beginTransaction(ConcretePathStringView{"/doc"});
        REQUIRE(tx.has_value());
        REQUIRE(space.insert("/doc/value", 5).errors.empty());
        // Scope exit without commit(): an early return or an exception in caller code.
    }

    auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
    REQUIRE(state);
    {
        std::scoped_lock lock(state->mutex);
        CHECK_FALSE(state->activeTransaction.has_value());
    }

    // The mutation made inside the abandoned transaction is in the tree, so it must be in the
    // journal too; otherwise undo cannot revert it.
    auto stats = space.getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 1);
    REQUIRE(space.undo(ConcretePathStringView{"/doc"}).has_value());
    CHECK_FALSE(space.read<int>("/doc/value").has_value());
}

TEST_CASE("moving a HistoryTransaction keeps exactly one owner of the open transaction") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());

    auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
    REQUIRE(state);

    auto first = space.beginTransaction(ConcretePathStringView{"/doc"});
    REQUIRE(first.has_value());
    {
        auto moved = std::move(first.value());
        std::scoped_lock lock(state->mutex);
        REQUIRE(state->activeTransaction.has_value());
        CHECK(state->activeTransaction->depth == 1);
    }
    // `moved` went out of scope and released the transaction; the moved-from handle must not
    // release it a second time or report itself as active.
    CHECK_FALSE(static_cast<bool>(first.value()));
    std::scoped_lock lock(state->mutex);
    CHECK_FALSE(state->activeTransaction.has_value());
}

TEST_CASE("a failed undo step leaves the journal cursor where the tree is") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());
    REQUIRE(space.insert("/doc/a", std::string{"alpha"}).errors.empty());
    REQUIRE(space.insert("/doc/a", std::string{"beta"}).errors.empty());

    auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
    REQUIRE(state);
    REQUIRE(state->journal.cursor() == 2);

    // Corrupt the inverse payload of the newest entry so the undo step cannot be applied.
    auto goodInverse = state->journal.entries.back().inverseValue;
    state->journal.entries.back().inverseValue.bytes = {std::byte{0xFF}, std::byte{0xFF}};

    auto failed = space.undo(ConcretePathStringView{"/doc"});
    REQUIRE_FALSE(failed.has_value());
    CHECK(state->journal.cursor() == 2);
    auto stats = space.getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 2);
    CHECK(stats->counts.redo == 0);

    // Once the entry is usable again the same undo must still be available and revert the tree.
    state->journal.entries.back().inverseValue = goodInverse;
    REQUIRE(space.undo(ConcretePathStringView{"/doc"}).has_value());
    CHECK(state->journal.cursor() == 1);
    auto first = space.take<std::string>("/doc/a");
    REQUIRE(first.has_value());
    CHECK(*first == "alpha");
    CHECK_FALSE(space.read<std::string>("/doc/a").has_value());
}

TEST_CASE("a failed redo step leaves the journal cursor where the tree is") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());
    REQUIRE(space.insert("/doc/a", std::string{"alpha"}).errors.empty());
    REQUIRE(space.undo(ConcretePathStringView{"/doc"}).has_value());

    auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
    REQUIRE(state);
    REQUIRE(state->journal.cursor() == 0);

    auto goodValue = state->journal.entries.back().value;
    state->journal.entries.back().value.bytes = {std::byte{0xFF}};

    auto failed = space.redo(ConcretePathStringView{"/doc"});
    REQUIRE_FALSE(failed.has_value());
    CHECK(state->journal.cursor() == 0);

    state->journal.entries.back().value = goodValue;
    REQUIRE(space.redo(ConcretePathStringView{"/doc"}).has_value());
    CHECK(state->journal.cursor() == 1);
    auto value = space.read<std::string>("/doc/a");
    REQUIRE(value.has_value());
    CHECK(*value == "alpha");
}

TEST_CASE("a multi-step undo that fails midway reports the steps it applied through the cursor") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());
    REQUIRE(space.insert("/doc/a", 1).errors.empty());
    REQUIRE(space.insert("/doc/b", 2).errors.empty());
    REQUIRE(space.insert("/doc/c", 3).errors.empty());

    auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
    REQUIRE(state);
    // Entry for /doc/a has no inverse payload (the node did not exist); make it claim one it
    // cannot decode so the third undo step fails.
    state->journal.entries.front().inverseValue.present = true;
    state->journal.entries.front().inverseValue.bytes   = {std::byte{0xFF}};

    auto failed = space.undo(ConcretePathStringView{"/doc"}, 3);
    REQUIRE_FALSE(failed.has_value());
    // Two steps were applied to the tree, so exactly two must be redoable.
    CHECK(state->journal.cursor() == 1);
    CHECK_FALSE(space.read<int>("/doc/c").has_value());
    CHECK_FALSE(space.read<int>("/doc/b").has_value());
    CHECK(space.read<int>("/doc/a").has_value());
}

TEST_CASE("a new edit after undo drops the discarded redo tail from the persisted journal") {
    auto tempRoot = uniqueTempRoot("redo_tail");
    auto opts     = persistentOptions(tempRoot, "redo_tail");

    std::filesystem::path journalPath;
    {
        UndoableSpace space{std::make_unique<PathSpace>(), {}};
        REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
        REQUIRE(space.insert("/doc/x", 1).errors.empty());
        REQUIRE(space.undo(ConcretePathStringView{"/doc"}).has_value());
        REQUIRE(space.insert("/doc/y", 2).errors.empty());

        CHECK_FALSE(space.read<int>("/doc/x").has_value());
        auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
        REQUIRE(state);
        CHECK(state->journal.size() == 1);
        journalPath = state->journalPath;
    }

    CHECK(countPersistedEntries(journalPath) == 1);

    UndoableSpace reloaded{std::make_unique<PathSpace>(), {}};
    REQUIRE(reloaded.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    // The undone insert of /doc/x was replaced by the insert of /doc/y; it must not come back.
    CHECK_FALSE(reloaded.read<int>("/doc/x").has_value());
    auto y = reloaded.read<int>("/doc/y");
    REQUIRE(y.has_value());
    CHECK(*y == 2);
    auto stats = reloaded.getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 1);
    CHECK(stats->counts.redo == 0);

    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("retention trims are applied to the persisted journal") {
    auto tempRoot = uniqueTempRoot("trim");
    auto opts     = persistentOptions(tempRoot, "trim");
    opts.maxEntries = 2;

    std::filesystem::path journalPath;
    {
        UndoableSpace space{std::make_unique<PathSpace>(), {}};
        REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
        for (int i = 0; i < 6; ++i) {
            REQUIRE(space.insert("/doc/v" + std::to_string(i), i).errors.empty());
        }
        auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
        REQUIRE(state);
        REQUIRE(state->journal.size() == 2);
        journalPath = state->journalPath;
    }
    // Without compaction the file keeps every record ever written and grows without bound.
    CHECK(countPersistedEntries(journalPath) == 2);

    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("values stored on the POD lane before wrapping stay readable through the wrapper") {
    auto inner = std::make_unique<PathSpace>();
    // int takes the POD fast path: the node holds a PodPayload, not a NodeData.
    REQUIRE(inner->insert("/plain/value", 11).errors.empty());
    REQUIRE(inner->insert("/doc/value", 1).errors.empty());

    UndoableSpace space{std::move(inner), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());

    // The wrapper always asks for the generic representation. The node's representation is an
    // internal detail; a matching type must still be served.
    auto outside = space.read<int>("/plain/value");
    REQUIRE(outside.has_value());
    CHECK(*outside == 11);
    auto inside = space.read<int>("/doc/value");
    REQUIRE(inside.has_value());
    CHECK(*inside == 1);

    // A different type is still a mismatch.
    CHECK_FALSE(space.read<std::string>("/plain/value").has_value());

    auto taken = space.take<int>("/plain/value");
    REQUIRE(taken.has_value());
    CHECK(*taken == 11);
    CHECK_FALSE(space.read<int>("/plain/value").has_value());
}

TEST_CASE("undo keeps values that were on the POD lane before the journaled edit") {
    auto inner = std::make_unique<PathSpace>();
    REQUIRE(inner->insert("/doc/value", 1).errors.empty());

    UndoableSpace space{std::move(inner), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, journalOptions()).has_value());

    REQUIRE(space.insert("/doc/value", 2).errors.empty());
    REQUIRE(space.undo(ConcretePathStringView{"/doc"}).has_value());

    // Undoing the insert of 2 must leave the queue as it was: [1]. The "before" capture used to
    // look at the generic payload only, saw an empty node and made undo wipe the 1 as well.
    auto first = space.take<int>("/doc/value");
    REQUIRE(first.has_value());
    CHECK(*first == 1);
    CHECK_FALSE(space.read<int>("/doc/value").has_value());

    // Undo the take, redo it, and undo again: the value follows.
    REQUIRE(space.undo(ConcretePathStringView{"/doc"}).has_value());
    auto restored = space.read<int>("/doc/value");
    REQUIRE(restored.has_value());
    CHECK(*restored == 1);
}

TEST_CASE("edits made after a savefile import reach the persisted journal") {
    auto tempRoot = uniqueTempRoot("import_writer");
    auto opts     = persistentOptions(tempRoot, "import_writer");
    auto savefile = tempRoot / "export.history";

    std::filesystem::path journalPath;
    {
        UndoableSpace space{std::make_unique<PathSpace>(), {}};
        REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
        // The first commit opens the journal writer and keeps the file handle.
        REQUIRE(space.insert("/doc/a", 1).errors.empty());
        REQUIRE(space.exportHistorySavefile(ConcretePathStringView{"/doc"}, savefile).has_value());
        // Import replaces journal.log on disk while the writer still points at the old file.
        REQUIRE(space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile).has_value());
        REQUIRE(space.insert("/doc/b", 2).errors.empty());

        auto state = space.findJournalRoot(ConcretePathStringView{"/doc"});
        REQUIRE(state);
        REQUIRE(state->journal.size() == 2);
        journalPath = state->journalPath;
    }

    CHECK(countPersistedEntries(journalPath) == 2);

    UndoableSpace reloaded{std::make_unique<PathSpace>(), {}};
    REQUIRE(reloaded.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    auto b = reloaded.read<int>("/doc/b");
    CHECK(b.has_value());

    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
}

// OPEN DECISION (docs/State_Machine_Audit.md, H4): journal.log has no place to record how many
// of its records are currently undone, so an undo that is not followed by a new edit is lost on
// restart and the undone edits come back. Fixing it means persisting the cursor (header field,
// marker record or side file), which changes the on-disk format. Skipped until decided; run it with --no-skip (it fails today).
TEST_CASE("undo position survives a restart" * doctest::skip()) {
    auto tempRoot = uniqueTempRoot("cursor");
    auto opts     = persistentOptions(tempRoot, "cursor");

    {
        UndoableSpace space{std::make_unique<PathSpace>(), {}};
        REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
        REQUIRE(space.insert("/doc/x", 1).errors.empty());
        REQUIRE(space.insert("/doc/y", 2).errors.empty());
        REQUIRE(space.undo(ConcretePathStringView{"/doc"}).has_value());
        REQUIRE_FALSE(space.read<int>("/doc/y").has_value());
    }

    UndoableSpace reloaded{std::make_unique<PathSpace>(), {}};
    REQUIRE(reloaded.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    auto stats = reloaded.getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    CHECK_FALSE(reloaded.read<int>("/doc/y").has_value());
    CHECK(stats->counts.undo == 1);
    CHECK(stats->counts.redo == 1);
}

TEST_SUITE_END();

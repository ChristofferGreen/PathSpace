#include "UndoableSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.persistence");

TEST_CASE("persistence restores state and undo history") {
    auto tempRoot = std::filesystem::temp_directory_path() / "undoable_space_persist_test";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    HistoryOptions defaults;
    defaults.persistHistory      = true;
    defaults.persistenceRoot     = tempRoot.string();
    defaults.persistenceNamespace = "suite";
    defaults.ramCacheEntries     = 2;

    auto space = makeUndoableSpace(defaults);
    REQUIRE(space);
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    REQUIRE(space->insert("/doc/title", std::string{"alpha"}).errors.empty());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 1);

    space.reset();

    auto reloaded = makeUndoableSpace(defaults);
    REQUIRE(reloaded);
    REQUIRE(reloaded->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto value = reloaded->read<std::string>("/doc/title");
    REQUIRE(value.has_value());
    CHECK(*value == std::string{"alpha"});

    REQUIRE(reloaded->undo(ConcretePathStringView{"/doc"}).has_value());
    auto missing = reloaded->read<std::string>("/doc/title");
    CHECK_FALSE(missing.has_value());

    REQUIRE(reloaded->redo(ConcretePathStringView{"/doc"}).has_value());
    auto restored = reloaded->read<std::string>("/doc/title");
    REQUIRE(restored.has_value());
    CHECK(*restored == std::string{"alpha"});

    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("journal persistence replays entries on enable") {
    auto tempRoot = std::filesystem::temp_directory_path() / "undoable_space_journal_persist_test";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    HistoryOptions defaults;
    defaults.persistHistory       = true;
    defaults.persistenceRoot      = tempRoot.string();
    defaults.persistenceNamespace = "journal_suite";
    defaults.useMutationJournal   = true;

    {
        auto space = makeUndoableSpace(defaults);
        REQUIRE(space);
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

        REQUIRE(space->insert("/doc/value_a", std::string{"alpha"}).errors.empty());
        REQUIRE(space->insert("/doc/value_b", std::string{"beta"}).errors.empty());

        auto currentA = space->read<std::string>("/doc/value_a");
        REQUIRE(currentA.has_value());
        CHECK(*currentA == std::string{"alpha"});

        auto currentB = space->read<std::string>("/doc/value_b");
        REQUIRE(currentB.has_value());
        CHECK(*currentB == std::string{"beta"});
    }

    auto reloaded = makeUndoableSpace(defaults);
    REQUIRE(reloaded);
    REQUIRE(reloaded->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto reloadedA = reloaded->read<std::string>("/doc/value_a");
    REQUIRE(reloadedA.has_value());
    CHECK(*reloadedA == std::string{"alpha"});

    auto reloadedB = reloaded->read<std::string>("/doc/value_b");
    REQUIRE(reloadedB.has_value());
    CHECK(*reloadedB == std::string{"beta"});

    REQUIRE(reloaded->undo(ConcretePathStringView{"/doc"}).has_value());
    auto afterUndoB = reloaded->read<std::string>("/doc/value_b");
    CHECK_FALSE(afterUndoB.has_value());
    auto afterUndoA = reloaded->read<std::string>("/doc/value_a");
    REQUIRE(afterUndoA.has_value());
    CHECK(*afterUndoA == std::string{"alpha"});

    REQUIRE(reloaded->redo(ConcretePathStringView{"/doc"}).has_value());
    auto afterRedoB = reloaded->read<std::string>("/doc/value_b");
    REQUIRE(afterRedoB.has_value());
    CHECK(*afterRedoB == std::string{"beta"});
    auto afterRedoA = reloaded->read<std::string>("/doc/value_a");
    REQUIRE(afterRedoA.has_value());
    CHECK(*afterRedoA == std::string{"alpha"});

    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("persistence namespace validation rejects path traversal tokens") {
    auto tempRoot = std::filesystem::temp_directory_path() / "undoable_space_namespace_validation";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    HistoryOptions snapshotDefaults;
    snapshotDefaults.persistHistory       = true;
    snapshotDefaults.persistenceRoot      = tempRoot.string();
    snapshotDefaults.persistenceNamespace = "invalid/namespace";

    {
        auto space = makeUndoableSpace(snapshotDefaults);
        REQUIRE(space);
        auto enable = space->enableHistory(ConcretePathStringView{"/doc"});
        REQUIRE_FALSE(enable.has_value());
        CHECK(enable.error().code == Error::Code::InvalidPermissions);
    }

    snapshotDefaults.persistenceNamespace = "snapshot_ns";
    {
        auto space = makeUndoableSpace(snapshotDefaults);
        REQUIRE(space);
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    }

    HistoryOptions journalDefaults = snapshotDefaults;
    journalDefaults.useMutationJournal   = true;
    journalDefaults.persistenceNamespace = "bad namespace";
    {
        auto space = makeUndoableSpace(journalDefaults);
        REQUIRE(space);
        auto enable = space->enableHistory(ConcretePathStringView{"/doc"});
        REQUIRE_FALSE(enable.has_value());
        CHECK(enable.error().code == Error::Code::InvalidPermissions);
    }

    journalDefaults.persistenceNamespace = "journal_ns";
    {
        auto space = makeUndoableSpace(journalDefaults);
        REQUIRE(space);
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    }

    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("savefile export import roundtrip retains history") {
    auto savePath = std::filesystem::temp_directory_path() / "undoable_space_savefile.bin";
    std::error_code removeEc;
    std::filesystem::remove(savePath, removeEc);

    HistoryOptions journalDefaults;
    journalDefaults.useMutationJournal = true;

    auto source = makeUndoableSpace(journalDefaults);
    REQUIRE(source);
    REQUIRE(source->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    REQUIRE(source->insert("/doc/value", std::string{"alpha"}).errors.empty());
    REQUIRE(source->insert("/doc/value", std::string{"beta"}).errors.empty());

    auto exportResult =
        source->exportHistorySavefile(ConcretePathStringView{"/doc"}, savePath, true);
    REQUIRE(exportResult.has_value());

    auto destination = makeUndoableSpace(journalDefaults);
    REQUIRE(destination);
    REQUIRE(destination->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto importResult =
        destination->importHistorySavefile(ConcretePathStringView{"/doc"}, savePath);
    REQUIRE(importResult.has_value());

    auto statsBefore = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsBefore.has_value());
    CHECK(statsBefore->counts.undo >= 1);

    REQUIRE(destination->undo(ConcretePathStringView{"/doc"}).has_value());
    auto statsAfterUndo = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterUndo.has_value());
    CHECK(statsAfterUndo->counts.undo + 1 == statsBefore->counts.undo);
    CHECK(statsAfterUndo->counts.redo >= 1);

    REQUIRE(destination->redo(ConcretePathStringView{"/doc"}).has_value());
    auto statsAfterRedo = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterRedo.has_value());
    CHECK(statsAfterRedo->counts.undo == statsBefore->counts.undo);
    CHECK(statsAfterRedo->counts.redo == 0);

    auto first = destination->take<std::string>("/doc/value");
    REQUIRE(first.has_value());
    CHECK(*first == std::string{"alpha"});
    auto second = destination->take<std::string>("/doc/value");
    REQUIRE(second.has_value());
    CHECK(*second == std::string{"beta"});

    auto stats = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo >= 1);

    std::filesystem::remove(savePath, removeEc);
}

TEST_SUITE_END();

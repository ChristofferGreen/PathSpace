#include "UndoableSpaceHistoryCoverageTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.coverage.guards");

TEST_CASE("UndoableSpace enableHistory reports invalid existing roots") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    auto invalidState = std::make_shared<UndoJournalRootState>();
    space.journalRoots.emplace("/bad/../root", invalidState);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    auto result = space.enableHistory(ConcretePathStringView{"/doc"}, opts);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPathSubcomponent);
}

TEST_CASE("UndoableSpace findJournalRootByPath rejects malformed paths") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    auto matched = space.findJournalRootByPath("/bad/../path");
    CHECK_FALSE(matched.has_value());
}

TEST_CASE("UndoableSpace findJournalRootByPath finds nearest history root") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto matched = space.findJournalRootByPath("/doc/value");
    REQUIRE(matched.has_value());
    CHECK(matched->key == "/doc");
    CHECK(matched->relativePath == "value");
    CHECK_FALSE(matched->diagnostics);
}

TEST_CASE("UndoableSpace undo and redo reject missing history roots") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    auto undoResult = space.undo(ConcretePathStringView{"/missing"});
    CHECK_FALSE(undoResult.has_value());
    CHECK(undoResult.error().code == Error::Code::NotFound);

    auto redoResult = space.redo(ConcretePathStringView{"/missing"});
    CHECK_FALSE(redoResult.has_value());
    CHECK(redoResult.error().code == Error::Code::NotFound);
}

TEST_CASE("UndoableSpace insert rejects mismatched journal components") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    REQUIRE(stateIt->second);
    stateIt->second->components = {"other"};

    auto result = space.insert("/doc/value", 1);
    CHECK_FALSE(result.errors.empty());
    CHECK(result.errors.front().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace take rejects mismatched journal components") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    REQUIRE(stateIt->second);
    stateIt->second->components = {"other"};

    auto result = space.take<int>("/doc/value");
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace insert reports nested payload capture errors") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto nested = std::make_unique<PathSpace>();
    auto seed = space.insert("/doc/nested", std::move(nested));
    CHECK(seed.nbrSpacesInserted == 1);

    auto result = space.insert("/doc/nested", 1);
    CHECK_FALSE(result.errors.empty());
    CHECK(result.errors.front().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace take reports nested payload capture errors") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto nested = std::make_unique<PathSpace>();
    auto seed = space.insert("/doc/nested", std::move(nested));
    CHECK(seed.nbrSpacesInserted == 1);

    auto result = space.take<int>("/doc/nested");
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace garbage collect rejects active transactions") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto txExpected = space.beginTransaction(ConcretePathStringView{"/doc"});
    REQUIRE(txExpected.has_value());
    auto tx = std::move(txExpected.value());

    auto gc = space.insert("/doc/_history/garbage_collect", true);
    CHECK_FALSE(gc.errors.empty());
    CHECK(gc.errors.front().code == Error::Code::InvalidPermissions);

    CHECK(tx.commit().has_value());
}

TEST_CASE("UndoableSpace history commands reject active transactions") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto txExpected = space.beginTransaction(ConcretePathStringView{"/doc"});
    REQUIRE(txExpected.has_value());
    auto tx = std::move(txExpected.value());

    auto undo = space.insert("/doc/_history/undo", 1);
    CHECK_FALSE(undo.errors.empty());
    CHECK(undo.errors.front().code == Error::Code::InvalidPermissions);

    auto redo = space.insert("/doc/_history/redo", 1);
    CHECK_FALSE(redo.errors.empty());
    CHECK(redo.errors.front().code == Error::Code::InvalidPermissions);

    CHECK(tx.commit().has_value());
}

TEST_CASE("UndoableSpace history command insert validates payload and command name") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto badTag = space.insert("/doc/_history/set_tag", 5);
    CHECK_FALSE(badTag.errors.empty());
    CHECK(badTag.errors.front().code == Error::Code::InvalidType);

    auto unknown = space.insert("/doc/_history/unknown_command", true);
    CHECK_FALSE(unknown.errors.empty());
    CHECK(unknown.errors.front().code == Error::Code::UnknownError);
}

TEST_SUITE_END();

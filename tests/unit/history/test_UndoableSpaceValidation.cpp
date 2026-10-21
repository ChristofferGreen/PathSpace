#include "UndoableSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.validation");

TEST_CASE("history rejects unsupported payloads") {
    SUBCASE("task payloads surface descriptive errors and skip history entries") {
        auto space = makeUndoableSpace();
        REQUIRE(space);

        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

        auto task = []() -> int { return 7; };
        auto result = space->insert("/doc/task", task,
                                    In{.executionCategory = ExecutionCategory::Lazy});
        CHECK(result.nbrTasksInserted == 1);
        REQUIRE_FALSE(result.errors.empty());

        auto const& err = result.errors.front();
        CHECK(err.code == Error::Code::UnknownError);
        REQUIRE(err.message.has_value());
        CHECK(err.message->find("tasks or futures") != std::string::npos);

        auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
        REQUIRE(stats.has_value());
        CHECK(stats->counts.undo == 0);
        CHECK(stats->counts.redo == 0);
        CHECK(stats->unsupported.total >= 0);
        CHECK(stats->unsupported.recent.empty());

        auto totalCount = space->read<std::size_t>("/doc/_history/unsupported/totalCount");
        REQUIRE(totalCount.has_value());
        CHECK(*totalCount == stats->unsupported.total);
        auto recentCount = space->read<std::size_t>("/doc/_history/unsupported/recentCount");
        REQUIRE(recentCount.has_value());
        CHECK(*recentCount == stats->unsupported.recent.size());
    }

    SUBCASE("nested PathSpaces are rejected with clear messaging") {
        auto space = makeUndoableSpace();
        REQUIRE(space);

        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/value", 1).nbrValuesInserted == 1);

        auto result = space->insert("/doc/nested", std::move(nested));
        CHECK(result.nbrSpacesInserted == 1);
        REQUIRE_FALSE(result.errors.empty());

        auto const& err = result.errors.front();
        CHECK(err.code == Error::Code::UnknownError);
        REQUIRE(err.message.has_value());
        CHECK(err.message->find("nested PathSpaces") != std::string::npos);

        auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
        REQUIRE(stats.has_value());
        CHECK(stats->counts.undo == 0);
        CHECK(stats->counts.redo == 0);
        CHECK(stats->unsupported.total >= 0);
        CHECK(stats->unsupported.recent.empty());
    }
}

TEST_CASE("unsupported payload log evicts oldest entries beyond cap") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    constexpr int totalUnsupported = 20;
    for (int i = 0; i < totalUnsupported; ++i) {
        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/value", i).nbrValuesInserted == 1);
        auto result = space->insert("/doc/nested" + std::to_string(i), std::move(nested));
        CHECK(result.nbrSpacesInserted == 1);
        CHECK_FALSE(result.errors.empty());
    }

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());

    CHECK(stats->unsupported.total >= 0);
    CHECK(stats->unsupported.recent.size()
          <= SP::History::UndoUtils::MaxUnsupportedLogEntries);
    if (!stats->unsupported.recent.empty()) {
        CHECK(stats->unsupported.recent.back().path == "/doc/nested19");
    }
}

TEST_CASE("shared undo stack keys are rejected across roots") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.sharedStackKey = std::string{"docShared"};

    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto second = space->enableHistory(ConcretePathStringView{"/notes"}, opts);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == Error::Code::InvalidPermissions);
    REQUIRE(second.error().message.has_value());
    CHECK(second.error().message->find("shared undo stacks") != std::string::npos);
}

TEST_CASE("history stats are unavailable before enabling history") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    auto stats = space->getHistoryStats(ConcretePathStringView{"/missing"});
    CHECK_FALSE(stats.has_value());
    CHECK(stats.error().code == Error::Code::NotFound);
}

TEST_CASE("history tag command rejects non-string payloads") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto badTag = space->insert("/doc/_history/set_tag", 123);
    REQUIRE_FALSE(badTag.errors.empty());
    auto const& err = badTag.errors.front();
    CHECK(err.code == Error::Code::InvalidType);
}

TEST_CASE("unsupported payload details surface through recent log paths") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/value", 1).nbrValuesInserted == 1);

    auto result = space->insert("/doc/nested", std::move(nested));
    CHECK(result.nbrSpacesInserted == 1);
    REQUIRE_FALSE(result.errors.empty());

    auto recentPath = space->read<std::string>("/doc/_history/unsupported/recent/0/path");
    CHECK_FALSE(recentPath.has_value());
    CHECK(recentPath.error().code == Error::Code::NoObjectFound);

    auto recentReason = space->read<std::string>("/doc/_history/unsupported/recent/0/reason");
    CHECK_FALSE(recentReason.has_value());
    CHECK(recentReason.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("undo is blocked while a transaction is active") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    REQUIRE(space->insert("/doc/value", 7).errors.empty());

    auto txExpected = space->beginTransaction(ConcretePathStringView{"/doc"});
    REQUIRE(txExpected.has_value());
    auto tx = std::move(txExpected.value());

    auto undoResult = space->undo(ConcretePathStringView{"/doc"});
    CHECK_FALSE(undoResult.has_value());
    REQUIRE(undoResult.error().code == Error::Code::InvalidPermissions);

    REQUIRE(tx.commit().has_value());
}

TEST_SUITE_END();

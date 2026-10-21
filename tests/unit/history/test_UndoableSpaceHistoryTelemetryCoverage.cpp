#include "UndoableSpaceHistoryCoverageTestSupport.hpp"

namespace {

struct FailingPathSpace final : PathSpace {
    FailingPathSpace() = default;

protected:
    auto in(Iterator const&, InputData const&) -> InsertReturn override {
        InsertReturn ret;
        std::string message = (counter < repeatCount)
            ? repeatedMessage
            : repeatedMessage + " " + std::to_string(counter);
        ++counter;
        ret.errors.push_back(Error{Error::Code::UnknownError, message});
        return ret;
    }

private:
    int         counter = 0;
    int         repeatCount = 2;
    std::string repeatedMessage = "forced failure";
};

struct SilentFailingPathSpace final : PathSpace {
protected:
    auto in(Iterator const&, InputData const&) -> InsertReturn override {
        InsertReturn ret;
        Error err{Error::Code::UnknownError, ""};
        err.message.reset();
        ret.errors.push_back(std::move(err));
        return ret;
    }
};

} // namespace

TEST_SUITE_BEGIN("history.undoable.coverage.telemetry");

TEST_CASE("UndoableSpace records and reorders unsupported payload telemetry") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;

    space.recordJournalUnsupportedPayload(state, "/a", "reason");
    space.recordJournalUnsupportedPayload(state, "/b", "reason2");
    REQUIRE(state.telemetry.unsupportedLog.size() == 2);

    space.recordJournalUnsupportedPayload(state, "/a", "reason");
    REQUIRE(state.telemetry.unsupportedLog.size() == 2);
    CHECK(state.telemetry.unsupportedLog.back().path == "/a");
    CHECK(state.telemetry.unsupportedLog.back().reason == "reason");
    CHECK(state.telemetry.unsupportedLog.back().occurrences == 2);
}

TEST_CASE("UndoableSpace trims unsupported payload telemetry log") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;

    for (std::size_t i = 0; i <= UndoUtilsAlias::MaxUnsupportedLogEntries; ++i) {
        space.recordJournalUnsupportedPayload(state,
                                              "/path/" + std::to_string(i),
                                              "reason");
    }

    REQUIRE(state.telemetry.unsupportedLog.size() == UndoUtilsAlias::MaxUnsupportedLogEntries);
    CHECK(state.telemetry.unsupportedLog.front().path == "/path/1");
}

TEST_CASE("UndoableSpace readHistoryStatsValue handles last operation and unsupported entries") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryStats stats;

    HistoryLastOperation op;
    op.type        = "insert";
    op.timestampMs = 123;
    op.success     = false;
    stats.lastOperation = op;

    HistoryUnsupportedRecord record;
    record.path            = "/bad";
    record.reason          = "unsupported";
    record.occurrences     = 2;
    record.lastTimestampMs = 99;
    stats.unsupported.recent.push_back(record);
    stats.unsupported.total = 1;

    std::string opType;
    InputMetadata metaStr{InputMetadataT<std::string>{}};
    auto err = space.readHistoryStatsValue(stats,
                                           std::optional<std::size_t>{5},
                                           std::string(UndoPaths::HistoryLastOperationType),
                                           metaStr,
                                           &opType);
    CHECK_FALSE(err.has_value());
    CHECK(opType == "insert");

    std::string recPath;
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedRecentPrefix) + "0",
                                      metaStr,
                                      &recPath);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);

    std::size_t totalCount = 0;
    InputMetadata metaSize{InputMetadataT<std::size_t>{}};
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedTotalCount),
                                      metaSize,
                                      &totalCount);
    CHECK_FALSE(err.has_value());
    CHECK(totalCount == 1);

    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedRecentPrefix) + "1/path",
                                      metaStr,
                                      &recPath);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);

    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedRecentPrefix) + "bad/path",
                                      metaStr,
                                      &recPath);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);

    std::size_t head = 0;
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{},
                                      std::string(UndoPaths::HistoryHeadGeneration),
                                      metaSize,
                                      &head);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("UndoableSpace readHistoryStatsValue validates metadata and pointers") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryStats stats;
    stats.counts.undo = 4;

    std::size_t undoCount = 0;
    InputMetadata metaSize{InputMetadataT<std::size_t>{}};
    auto err = space.readHistoryStatsValue(stats,
                                           std::optional<std::size_t>{0},
                                           std::string(UndoPaths::HistoryStatsUndoCount),
                                           metaSize,
                                           &undoCount);
    CHECK_FALSE(err.has_value());
    CHECK(undoCount == 4);

    std::string wrong;
    InputMetadata metaStr{InputMetadataT<std::string>{}};
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{0},
                                      std::string(UndoPaths::HistoryStatsUndoCount),
                                      metaStr,
                                      &wrong);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidType);

    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{0},
                                      std::string(UndoPaths::HistoryStatsUndoCount),
                                      metaSize,
                                      nullptr);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
}

TEST_CASE("UndoableSpace readHistoryStatsValue reports missing last operation") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryStats stats;

    std::string opType;
    InputMetadata metaStr{InputMetadataT<std::string>{}};
    auto err = space.readHistoryStatsValue(stats,
                                           std::optional<std::size_t>{0},
                                           std::string(UndoPaths::HistoryLastOperationType),
                                           metaStr,
                                           &opType);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("UndoableSpace readDiagnosticsHistoryValue reports missing state and null output") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    InputMetadata metaU64{InputMetadataT<std::uint64_t>{}};

    UndoableSpace::MatchedJournalRoot missing{
        std::shared_ptr<UndoJournalRootState>{},
        "",
        "",
        true,
        false};

    std::uint64_t value = 0;
    auto missingErr = space.readDiagnosticsHistoryValue(
        missing,
        std::string(UndoPaths::HistoryDiagnosticsHeadSequence),
        metaU64,
        &value);
    REQUIRE(missingErr.has_value());
    CHECK(missingErr->code == Error::Code::UnknownError);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());

    UndoableSpace::MatchedJournalRoot withState{
        stateIt->second,
        "/doc",
        std::string(UndoPaths::HistoryDiagnosticsHeadSequence),
        true,
        false};

    auto nullOut = space.readDiagnosticsHistoryValue(
        withState,
        std::string(UndoPaths::HistoryDiagnosticsHeadSequence),
        metaU64,
        nullptr);
    REQUIRE(nullOut.has_value());
    CHECK(nullOut->code == Error::Code::MalformedInput);
}

TEST_CASE("UndoableSpace readJournalHistoryValue reports missing state") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    InputMetadata metaSize{InputMetadataT<std::size_t>{}};

    UndoableSpace::MatchedJournalRoot missing{
        std::shared_ptr<UndoJournalRootState>{},
        "",
        "",
        false,
        false};

    std::size_t value = 0;
    auto err = space.readJournalHistoryValue(
        missing,
        std::string(UndoPaths::HistoryStatsUndoCount),
        metaSize,
        &value);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace insert rejects diagnostics paths") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto result = space.insert("/diagnostics/history/_doc/entries/0/path", std::string{"bad"});
    CHECK_FALSE(result.errors.empty());
    CHECK(result.errors.front().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace diagnostics entries expose tag and timing fields") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space.insert("/doc/_history/set_tag", std::string{"paint"}).errors.empty());
    REQUIRE(space.insert("/doc/value", 1).errors.empty());

    auto tag = space.read<std::string>("/diagnostics/history/_doc/entries/0/tag");
    REQUIRE(tag.has_value());
    CHECK(*tag == "paint");

    auto timestamp = space.read<std::uint64_t>("/diagnostics/history/_doc/entries/0/timestampMs");
    CHECK(timestamp.has_value());

    auto monotonic = space.read<std::uint64_t>("/diagnostics/history/_doc/entries/0/monotonicNs");
    CHECK(monotonic.has_value());

    auto sequence = space.read<std::uint64_t>("/diagnostics/history/_doc/entries/0/sequence");
    CHECK(sequence.has_value());

    auto barrier = space.read<bool>("/diagnostics/history/_doc/entries/0/barrier");
    CHECK(barrier.has_value());

    auto inverseBytes = space.read<std::size_t>("/diagnostics/history/_doc/entries/0/inverseBytes");
    CHECK(inverseBytes.has_value());

    auto hasInverse = space.read<bool>("/diagnostics/history/_doc/entries/0/hasInverse");
    CHECK(hasInverse.has_value());

    auto badType = space.read<std::string>("/diagnostics/history/_doc/entries/0/timestampMs");
    CHECK_FALSE(badType.has_value());
    CHECK(badType.error().code == Error::Code::InvalidType);
}

TEST_CASE("UndoableSpace history stats surface unsupported recent records") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    space.recordJournalUnsupportedPayload(*stateIt->second, "/doc/bad", "unsupported");

    auto path = space.read<std::string>("/doc/_history/unsupported/recent/0/path");
    REQUIRE(path.has_value());
    CHECK(*path == "/doc/bad");

    auto reason = space.read<std::string>("/doc/_history/unsupported/recent/0/reason");
    REQUIRE(reason.has_value());
    CHECK(*reason == "unsupported");

    auto occurrences = space.read<std::size_t>("/doc/_history/unsupported/recent/0/occurrences");
    REQUIRE(occurrences.has_value());
    CHECK(*occurrences == 1);

    auto timestamp = space.read<std::uint64_t>("/doc/_history/unsupported/recent/0/timestampMs");
    CHECK(timestamp.has_value());
}

TEST_CASE("UndoableSpace trimHistory reports unsupported status") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    auto result = space.trimHistory(ConcretePathStringView{"/doc"},
                                    [](std::size_t) { return true; });
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotSupported);
}

TEST_CASE("UndoableSpace take rejects diagnostics and history command paths") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto diagnostics = space.take<std::uint64_t>("/diagnostics/history/_doc/head/sequence");
    CHECK_FALSE(diagnostics.has_value());
    CHECK(diagnostics.error().code == Error::Code::InvalidPermissions);

    auto command = space.take<std::string>("/doc/_history/undo");
    CHECK_FALSE(command.has_value());
    CHECK(command.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace insert logs unsupported errors and trims history log") {
    UndoableSpace space{std::make_unique<FailingPathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    REQUIRE(stateIt->second);
    auto state = stateIt->second;

    auto first = space.insert("/doc/value", 1);
    CHECK_FALSE(first.errors.empty());
    auto second = space.insert("/doc/value", 2);
    CHECK_FALSE(second.errors.empty());

    auto repeated = std::find_if(state->telemetry.unsupportedLog.begin(),
                                 state->telemetry.unsupportedLog.end(),
                                 [](auto const& entry) { return entry.reason == "forced failure"; });
    REQUIRE(repeated != state->telemetry.unsupportedLog.end());
    CHECK(repeated->occurrences == 2);

    for (std::size_t i = 0; i < UndoUtilsAlias::MaxUnsupportedLogEntries + 1; ++i) {
        auto result = space.insert("/doc/value", static_cast<int>(i));
        CHECK_FALSE(result.errors.empty());
    }

    CHECK(state->telemetry.unsupportedTotal
          == static_cast<std::uint64_t>(UndoUtilsAlias::MaxUnsupportedLogEntries + 3));
    CHECK(state->telemetry.unsupportedLog.size() == UndoUtilsAlias::MaxUnsupportedLogEntries);
}

TEST_CASE("UndoableSpace insert skips unsupported errors without messages") {
    UndoableSpace space{std::make_unique<SilentFailingPathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    REQUIRE(stateIt->second);
    auto state = stateIt->second;

    auto result = space.insert("/doc/value", 1);
    CHECK_FALSE(result.errors.empty());
    CHECK(state->telemetry.unsupportedLog.empty());
    CHECK(state->telemetry.unsupportedTotal == 0);
}

TEST_SUITE_END();

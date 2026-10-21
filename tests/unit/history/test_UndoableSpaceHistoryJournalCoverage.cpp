#include "UndoableSpaceHistoryCoverageTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.coverage.journal");

TEST_CASE("UndoableSpace payloadBytes clamps empty buffers") {
    NodeData empty;
    CHECK(UndoableSpace::payloadBytes(empty) == 0);

    std::optional<NodeData> none;
    CHECK(UndoableSpace::payloadBytes(none) == 0);

    int value = 7;
    InputMetadata meta{InputMetadataT<int>{}};
    NodeData filled;
    REQUIRE_FALSE(filled.serialize(InputData{&value, meta}).has_value());
    CHECK(UndoableSpace::payloadBytes(filled) > 0);
}

TEST_CASE("UndoableSpace adjustLiveBytes clamps underflow") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    std::size_t live = 3;
    space.adjustLiveBytes(live, 10, 2);
    CHECK(live == 0);

    live = 5;
    space.adjustLiveBytes(live, 2, 9);
    CHECK(live == 12);
}

TEST_CASE("UndoableSpace parseJournalRelativeComponents rejects invalid roots") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.components = {"root"};

    auto wrongRoot = space.parseJournalRelativeComponents(state, "/other/node");
    CHECK_FALSE(wrongRoot.has_value());
    CHECK(wrongRoot.error().code == Error::Code::InvalidPermissions);

    auto globPath = space.parseJournalRelativeComponents(state, "/root/*");
    CHECK_FALSE(globPath.has_value());
    CHECK(globPath.error().code == Error::Code::InvalidPathSubcomponent);
}

TEST_CASE("UndoableSpace parseJournalRelativeComponents rejects shorter paths") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.components = {"root", "child"};

    auto result = space.parseJournalRelativeComponents(state, "/root");
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace interpretSteps normalizes step counts") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    int steps = 4;
    InputMetadata metaInt{InputMetadataT<int>{}};
    CHECK(space.interpretSteps(InputData{&steps, metaInt}) == 4);

    int zero = 0;
    CHECK(space.interpretSteps(InputData{&zero, metaInt}) == 1);

    std::int64_t neg = -3;
    InputMetadata metaI64{InputMetadataT<std::int64_t>{}};
    CHECK(space.interpretSteps(InputData{&neg, metaI64}) == 1);

    unsigned int stepsUnsigned = 6;
    InputMetadata metaUnsigned{InputMetadataT<unsigned int>{}};
    CHECK(space.interpretSteps(InputData{&stepsUnsigned, metaUnsigned}) == 6);

    double unknownType = 2.5;
    InputMetadata metaDouble{InputMetadataT<double>{}};
    CHECK(space.interpretSteps(InputData{&unknownType, metaDouble}) == 1);

    InputMetadata emptyMeta;
    CHECK(space.interpretSteps(InputData{nullptr, emptyMeta}) == 1);
}

TEST_CASE("UndoableSpace applyJournalSteps validates state and step count") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    auto missing = space.applyJournalSteps({}, 1, true);
    CHECK_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);

    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}).has_value());
    REQUIRE(space.insert("/doc/value", 1).errors.empty());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());

    auto result = space.applyJournalSteps(stateIt->second, 0, true);
    CHECK(result.has_value());
}

TEST_CASE("UndoableSpace computeJournalLiveBytes returns zero without backend") {
    UndoableSpace space{std::unique_ptr<PathSpaceBase>{}, {}};
    UndoJournalRootState state;
    state.components = {"root"};
    CHECK(space.computeJournalLiveBytes(state) == 0);
}

TEST_CASE("UndoableSpace captureJournalNodeData returns empty for missing nodes") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());

    auto result = space.captureJournalNodeData(*stateIt->second, {"missing"});
    REQUIRE(result.has_value());
    CHECK_FALSE(result.value().has_value());
}

TEST_CASE("UndoableSpace captureJournalNodeData reports missing backend") {
    UndoableSpace space{std::unique_ptr<PathSpaceBase>{}, {}};
    UndoJournalRootState state;

    auto result = space.captureJournalNodeData(state, {});
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace computeJournalLiveBytes reports subtree payloads") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}).has_value());

    REQUIRE(space.insert("/doc/a", 1).errors.empty());
    REQUIRE(space.insert("/doc/b/c", 2).errors.empty());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());

    auto liveBytes = space.computeJournalLiveBytes(*stateIt->second);
    CHECK(liveBytes > 0);

    UndoJournalRootState missing;
    missing.components = {"missing"};
    CHECK(space.computeJournalLiveBytes(missing) == 0);
}

TEST_CASE("UndoableSpace performJournalStep reports path and payload errors") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    UndoJournalRootState state;
    state.rootPath = "/root";
    state.components = {"root"};

    UndoJournal::JournalEntry badPath = makeSavefileEntry("/other");
    state.journal.append(badPath, false);

    auto badPathResult = space.performJournalStep(state, true, "undo", "Nothing to undo");
    CHECK_FALSE(badPathResult.has_value());
    CHECK(badPathResult.error().code == Error::Code::InvalidPermissions);

    state.journal.clear();

    UndoJournal::JournalEntry badPayload = makeSavefileEntry("/root/child");
    badPayload.inverseValue.present = true;
    badPayload.inverseValue.bytes = {std::byte{0x00}};
    state.journal.append(badPayload, false);

    auto badPayloadResult = space.performJournalStep(state, true, "undo", "Nothing to undo");
    CHECK_FALSE(badPayloadResult.has_value());
    CHECK(badPayloadResult.error().code == Error::Code::MalformedInput);
}

TEST_CASE("UndoableSpace performJournalStep surfaces apply failures") {
    UndoableSpace space{std::unique_ptr<PathSpaceBase>{}, {}};

    UndoJournalRootState state;
    state.rootPath = "/root";
    state.components = {"root"};

    UndoJournal::JournalEntry entry = makeSavefileEntry("/root/child");
    entry.value.present = false;
    entry.inverseValue.present = false;
    state.journal.append(entry, false);

    auto result = space.performJournalStep(state, true, "undo", "Nothing to undo");
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace handleJournalControlInsert reports missing state") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoableSpace::MatchedJournalRoot matched;
    matched.state = nullptr;

    InputMetadata meta;
    auto ret = space.handleJournalControlInsert(matched,
                                                std::string(UndoPaths::CommandUndo),
                                                InputData{nullptr, meta});
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace computeJournalLiveBytes sums subtree payloads") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space.insert("/doc/value", 42).errors.empty());
    REQUIRE(space.insert("/doc/child", 84).errors.empty());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    REQUIRE(stateIt->second);

    auto bytes = space.computeJournalLiveBytes(*stateIt->second);
    CHECK(bytes > 0);
}

TEST_SUITE_END();

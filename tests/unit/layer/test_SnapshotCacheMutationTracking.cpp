#include "SnapshotCachedPathSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.snapshot_cache.mutation_tracking");

TEST_CASE("Snapshot cache marks pop mutations dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/queue", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/queue", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto taken = cached.take<int>("/queue");
    REQUIRE(taken.has_value());
    CHECK(taken.value() == 1);

    auto next = cached.read<int>("/queue");
    REQUIRE(next.has_value());
    CHECK(next.value() == 2);
}

TEST_CASE("Snapshot cache marks root dirty on glob inserts") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/a", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    CHECK(cached.insert("/a*", 5).nbrValuesInserted >= 1);

    auto readB = cached.read<int>("/b");
    REQUIRE(readB.has_value());
    CHECK(readB.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache marks dirty for pack insert") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 42).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto packRet = cached.insert<"/one", "/two">(1, 2);
    CHECK(packRet.errors.empty());

    auto readOne = cached.read<int>("/one");
    REQUIRE(readOne.has_value());
    CHECK(readOne.value() == 1);

    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 42);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits >= before.hits + 1);
}

TEST_CASE("Snapshot cache pack insert empty input does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 60).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<std::string, 0> paths{};
    std::array<void const*, 0> values{};
    auto packRet = cached.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                                     InputMetadata{InputMetadataT<int>{}},
                                     std::span<void const* const>(values.data(), values.size()));
    CHECK(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 60);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache pack insert invalid path does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 55).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    int value = 7;
    std::array<std::string, 1> paths{{"relative"}};
    std::array<void const*, 1> values{{&value}};
    auto packRet = cached.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                                     InputMetadata{InputMetadataT<int>{}},
                                     std::span<void const* const>(values.data(), values.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 55);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache pack insert arity mismatch does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 57).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    int value = 7;
    std::array<std::string, 2> paths{{"/one", "/two"}};
    std::array<void const*, 1> values{{&value}};
    auto packRet = cached.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                                     InputMetadata{InputMetadataT<int>{}},
                                     std::span<void const* const>(values.data(), values.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 57);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache pack insert non-pod metadata does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 59).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::string value = "hello";
    std::array<std::string, 1> paths{{"/one"}};
    std::array<void const*, 1> values{{&value}};
    auto packRet = cached.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                                     InputMetadata{InputMetadataT<std::string>{}},
                                     std::span<void const* const>(values.data(), values.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 59);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache marks dirty after span pack mutation") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    std::array<int, 2> a{{1, 2}};
    std::array<int, 2> b{{3, 4}};
    CHECK(cached.insert<"a", "b">("/root", std::span<const int>(a), std::span<const int>(b)).errors.empty());

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto mutResult = cached.take<"a", "b">("/root", [&](std::span<int> aSpan, std::span<int> bSpan) {
        REQUIRE(aSpan.size() == 2);
        REQUIRE(bSpan.size() == 2);
        aSpan[0] = 9;
        bSpan[0] = 7;
    });
    CHECK(mutResult.has_value());

    auto readA = cached.read<int>("/root/a");
    REQUIRE(readA.has_value());
    CHECK(readA.value() == 9);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache spanPackConst forwards and bypasses metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/one", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/two", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<std::string, 2> paths{{"/one", "/two"}};
    auto before = cached.snapshotMetrics();
    auto spanRes = cached.spanPackConst(
        std::span<const std::string>(paths.data(), paths.size()),
        InputMetadata{InputMetadataT<int>{}},
        Out{},
        [](std::span<const RawConstSpan> spans) {
            REQUIRE(spans.size() == 2);
            REQUIRE(spans[0].count >= 1);
            REQUIRE(spans[1].count >= 1);
            auto first = static_cast<int const*>(spans[0].data);
            auto second = static_cast<int const*>(spans[1].data);
            CHECK(first[0] == 1);
            CHECK(second[0] == 2);
            return std::optional<Error>{};
        });
    CHECK(spanRes.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache spanPackConst missing path does not touch metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 14).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<std::string, 1> paths{{"/missing"}};
    auto before = cached.snapshotMetrics();
    auto spanRes = cached.spanPackConst(
        std::span<const std::string>(paths.data(), paths.size()),
        InputMetadata{InputMetadataT<int>{}},
        Out{},
        [](std::span<const RawConstSpan>) {
            return std::optional<Error>{};
        });
    CHECK_FALSE(spanRes.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache spanPackMut marks dirty on success") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/one", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/two", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<std::string, 2> paths{{"/one", "/two"}};
    auto before = cached.snapshotMetrics();
    auto spanRes = cached.spanPackMut(
        std::span<const std::string>(paths.data(), paths.size()),
        InputMetadata{InputMetadataT<int>{}},
        Out{},
        [](std::span<const RawMutSpan> spans) {
            REQUIRE(spans.size() == 2);
            REQUIRE(spans[0].count >= 1);
            REQUIRE(spans[1].count >= 1);
            auto first = static_cast<int*>(spans[0].data);
            auto second = static_cast<int*>(spans[1].data);
            first[0] = 11;
            second[0] = 12;
            return SpanPackResult{.error = std::nullopt, .shouldPop = false};
        });
    CHECK(spanRes.has_value());

    auto readOne = cached.read<int>("/one");
    REQUIRE(readOne.has_value());
    CHECK(readOne.value() == 11);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache spanPackMut callback error does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/one", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<std::string, 1> paths{{"/one"}};
    auto spanRes = cached.spanPackMut(
        std::span<const std::string>(paths.data(), paths.size()),
        InputMetadata{InputMetadataT<int>{}},
        Out{},
        [](std::span<const RawMutSpan>) {
            return SpanPackResult{.error = Error{Error::Code::InvalidPermissions, "forced error"}, .shouldPop = false};
        });
    CHECK_FALSE(spanRes.has_value());

    auto before = cached.snapshotMetrics();
    auto readOne = cached.read<int>("/one");
    REQUIRE(readOne.has_value());
    CHECK(readOne.value() == 1);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache does not mark dirty on failed insert") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 101).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto badInsert = cached.insert("invalid", 5, In{.validationLevel = ValidationLevel::Full});
    CHECK_FALSE(badInsert.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 101);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache marks dirty for span pack insert") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 21).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<int, 2> a{{3, 4}};
    std::array<int, 2> b{{5, 6}};
    auto packRet = cached.insert<"a", "b">("/root", std::span<const int>(a), std::span<const int>(b));
    CHECK(packRet.errors.empty());

    auto readA = cached.read<int>("/root/a");
    REQUIRE(readA.has_value());
    CHECK(readA.value() == 3);

    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 21);

    auto before = cached.snapshotMetrics();
    auto readStableAgain = cached.read<int>("/stable");
    REQUIRE(readStableAgain.has_value());
    CHECK(readStableAgain.value() == 21);
    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses);
    CHECK(after.hits >= before.hits);
}

TEST_CASE("Snapshot cache span pack insert empty input does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 28).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<std::string, 0> paths{};
    std::array<SpanInsertSpec, 0> specs{};
    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 28);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache span pack insert invalid path does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 31).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    int value = 9;
    SpanInsertSpec spec{
        .metadata = InputMetadata{InputMetadataT<int>{}},
        .span = RawConstSpan{&value, 1},
        .elementSize = sizeof(int),
    };
    std::array<std::string, 1> paths{{"relative"}};
    std::array<SpanInsertSpec, 1> specs{{spec}};
    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 31);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache span pack insert count mismatch does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 29).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<int, 1> one{{1}};
    std::array<int, 2> two{{2, 3}};
    SpanInsertSpec specA{
        .metadata = InputMetadata{InputMetadataT<int>{}},
        .span = RawConstSpan{one.data(), one.size()},
        .elementSize = sizeof(int),
    };
    SpanInsertSpec specB{
        .metadata = InputMetadata{InputMetadataT<int>{}},
        .span = RawConstSpan{two.data(), two.size()},
        .elementSize = sizeof(int),
    };
    std::array<std::string, 2> paths{{"/one", "/two"}};
    std::array<SpanInsertSpec, 2> specs{{specA, specB}};
    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 29);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache span pack insert arity mismatch does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 27).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    int value = 7;
    SpanInsertSpec spec{
        .metadata = InputMetadata{InputMetadataT<int>{}},
        .span = RawConstSpan{&value, 1},
        .elementSize = sizeof(int),
    };
    std::array<std::string, 2> paths{{"/one", "/two"}};
    std::array<SpanInsertSpec, 1> specs{{spec}};
    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 27);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache span pack insert non-pod metadata does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 25).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::string value = "value";
    SpanInsertSpec spec{
        .metadata = InputMetadata{InputMetadataT<std::string>{}},
        .span = RawConstSpan{&value, 1},
        .elementSize = sizeof(std::string),
    };
    std::array<std::string, 1> paths{{"/one"}};
    std::array<SpanInsertSpec, 1> specs{{spec}};
    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 25);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache span pack insert marks dirty on success") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 41).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    int one = 7;
    int two = 9;
    InputData dataOne{one};
    InputData dataTwo{two};
    SpanInsertSpec specA{
        .metadata = dataOne.metadata,
        .span = RawConstSpan{&one, 1},
        .elementSize = sizeof(int),
    };
    SpanInsertSpec specB{
        .metadata = dataTwo.metadata,
        .span = RawConstSpan{&two, 1},
        .elementSize = sizeof(int),
    };
    std::array<std::string, 2> paths{{"/one", "/two"}};
    std::array<SpanInsertSpec, 2> specs{{specA, specB}};
    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK(packRet.errors.empty());

    auto before = cached.snapshotMetrics();
    auto readOne = cached.read<int>("/one");
    REQUIRE(readOne.has_value());
    CHECK(readOne.value() == 7);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits == before.hits);
}

TEST_CASE("Snapshot cache span pack insert rebuild clears dirty roots") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 43).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    int one = 2;
    int two = 4;
    InputData dataOne{one};
    InputData dataTwo{two};
    SpanInsertSpec specA{
        .metadata = dataOne.metadata,
        .span = RawConstSpan{&one, 1},
        .elementSize = sizeof(int),
    };
    SpanInsertSpec specB{
        .metadata = dataTwo.metadata,
        .span = RawConstSpan{&two, 1},
        .elementSize = sizeof(int),
    };
    std::array<std::string, 2> paths{{"/one", "/two"}};
    std::array<SpanInsertSpec, 2> specs{{specA, specB}};
    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK(packRet.errors.empty());

    auto dirtyMetrics = cached.snapshotMetrics();
    auto dirtyRead = cached.read<int>("/one");
    REQUIRE(dirtyRead.has_value());
    CHECK(dirtyRead.value() == 2);
    auto afterDirty = cached.snapshotMetrics();
    CHECK(afterDirty.misses >= dirtyMetrics.misses + 1);

    cached.rebuildSnapshotNow();
    auto beforeHit = cached.snapshotMetrics();
    auto hitRead = cached.read<int>("/one");
    REQUIRE(hitRead.has_value());
    CHECK(hitRead.value() == 2);
    auto afterHit = cached.snapshotMetrics();
    CHECK(afterHit.hits >= beforeHit.hits + 1);
    CHECK(afterHit.misses == beforeHit.misses);
}

TEST_CASE("Snapshot cache span pack insert existing length mismatch preserves data") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 23).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    int a = 1;
    int b = 2;
    InputData dataA{a};
    InputData dataB{b};
    SpanInsertSpec specA{
        .metadata = dataA.metadata,
        .span = RawConstSpan{&a, 1},
        .elementSize = sizeof(int),
    };
    SpanInsertSpec specB{
        .metadata = dataB.metadata,
        .span = RawConstSpan{&b, 1},
        .elementSize = sizeof(int),
    };
    std::array<std::string, 2> paths{{"/one", "/two"}};
    std::array<SpanInsertSpec, 2> specs{{specA, specB}};
    auto seedRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK(seedRet.errors.empty());

    CHECK(cached.insert("/one", 7).nbrValuesInserted == 1);
    cached.rebuildSnapshotNow();

    auto packRet = cached.packInsertSpans(std::span<const std::string>(paths.data(), paths.size()),
                                          std::span<SpanInsertSpec const>(specs.data(), specs.size()));
    CHECK_FALSE(packRet.errors.empty());

    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 23);
}

TEST_CASE("Snapshot cache span pack reads bypass snapshot metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    std::array<int, 2> a{{1, 2}};
    std::array<int, 2> b{{3, 4}};
    CHECK(cached.insert<"a", "b">("/root", std::span<const int>(a), std::span<const int>(b)).errors.empty());

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto readResult = cached.read<"a", "b">("/root", [&](std::span<const int> aSpan, std::span<const int> bSpan) {
        REQUIRE(aSpan.size() == 2);
        REQUIRE(bSpan.size() == 2);
        CHECK(aSpan[0] == 1);
        CHECK(aSpan[1] == 2);
        CHECK(bSpan[0] == 3);
        CHECK(bSpan[1] == 4);
    });
    CHECK(readResult.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache span pack mutation failure does not mark dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    std::array<std::string, 1> paths{{"/missing"}};
    auto spanMut = cached.spanPackMut(
        std::span<const std::string>(paths.data(), paths.size()),
        InputMetadata{InputMetadataT<int>{}},
        Out{},
        [](std::span<RawMutSpan const>) {
            return SpanPackResult{.error = std::nullopt, .shouldPop = false};
        });
    CHECK_FALSE(spanMut.has_value());

    auto before = cached.snapshotMetrics();
    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 1);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache pack insert marks each path dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 11).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto packRet = cached.insert<"/p1", "/p2", "/p3">(1, 2, 3);
    CHECK(packRet.errors.empty());

    auto readStable = cached.read<int>("/stable");
    REQUIRE(readStable.has_value());
    CHECK(readStable.value() == 11);

    auto before = cached.snapshotMetrics();
    auto readStableAgain = cached.read<int>("/stable");
    REQUIRE(readStableAgain.has_value());
    CHECK(readStableAgain.value() == 11);
    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses);
    CHECK(after.hits >= before.hits);
}

TEST_CASE("Snapshot cache span pack mutation marks each path dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    std::array<int, 2> a{{1, 2}};
    std::array<int, 2> b{{3, 4}};
    std::array<int, 2> c{{5, 6}};
    CHECK(cached.insert<"a", "b", "c">("/root", std::span<const int>(a), std::span<const int>(b), std::span<const int>(c)).errors.empty());

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto mutResult = cached.take<"a", "b", "c">("/root", [&](std::span<int> aSpan,
                                                              std::span<int> bSpan,
                                                              std::span<int> cSpan) {
        aSpan[0] = 7;
        bSpan[0] = 8;
        cSpan[0] = 9;
    });
    CHECK(mutResult.has_value());

    auto readA = cached.read<int>("/root/a");
    REQUIRE(readA.has_value());
    CHECK(readA.value() == 7);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_SUITE_END();

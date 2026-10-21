#include "SnapshotCachedPathSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.snapshot_cache.read_paths");

TEST_CASE("Snapshot cache trySnapshotRead handles empty and relative paths") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    cached.stopSnapshotWorker();

    InputMetadata meta{InputMetadataT<int>{}};
    int outValue = 0;
    auto before = cached.snapshotMetrics();

    CHECK_FALSE(cached.trySnapshotRead(Iterator{""}, meta, Out{}, &outValue));
    CHECK_FALSE(cached.trySnapshotRead(Iterator{"relative"}, meta, Out{}, &outValue));

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 2);
}

TEST_CASE("Snapshot cache trySnapshotRead returns false before configuration") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    InputMetadata meta{InputMetadataT<int>{}};
    int outValue = 0;

    CHECK_FALSE(cached.trySnapshotRead(Iterator{"/missing"}, meta, Out{}, &outValue));
    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.hits == 0);
    CHECK(metrics.misses == 0);
}

TEST_CASE("Snapshot cache trySnapshotRead reports deserialize failure") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 7).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    cached.stopSnapshotWorker();

    InputMetadata meta{InputMetadataT<float>{}};
    float outValue = 0.0f;
    CHECK_FALSE(cached.trySnapshotRead(Iterator{"/value"}, meta, Out{}, &outValue));
}

TEST_CASE("Snapshot cache listChildrenCanonical handles missing backing and invalid paths") {
    SnapshotCachedPathSpace missing{std::shared_ptr<PathSpaceBase>{}};
    CHECK(missing.listChildrenCanonical("/anything").empty());
    CHECK_FALSE(missing.typedPeekFuture("/anything").has_value());

    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};
    CHECK(cached.listChildrenCanonical("relative/path").empty());
}

TEST_CASE("Snapshot cache bypasses snapshot for pop, block, and execution") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/item", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    cached.stopSnapshotWorker();

    auto beforePop = cached.snapshotMetrics();
    auto popped = cached.take<int>("/item");
    REQUIRE(popped.has_value());
    auto afterPop = cached.snapshotMetrics();
    CHECK(afterPop.hits == beforePop.hits);

    CHECK(cached.insert("/item", 2).nbrValuesInserted == 1);
    cached.rebuildSnapshotNow();
    auto beforeBlock = cached.snapshotMetrics();
    auto blocked = cached.read<int>("/item", Out{} & Block{std::chrono::milliseconds{0}});
    REQUIRE(blocked.has_value());
    auto afterBlock = cached.snapshotMetrics();
    CHECK(afterBlock.hits == beforeBlock.hits);

    InputMetadata execMeta{};
    execMeta.dataCategory = DataCategory::Execution;
    execMeta.typeInfo = &typeid(int);
    int outValue = 0;
    CHECK_FALSE(cached.trySnapshotRead(Iterator{"/item"}, execMeta, Out{}, &outValue));
}

TEST_CASE("Snapshot cache snapshots nested spaces") {
    auto backing = std::make_shared<PathSpace>();
    auto nested = std::make_unique<PathSpace>();

    CHECK(backing->insert("/nested", std::move(nested)).nbrSpacesInserted == 1);
    CHECK(backing->insert("/nested/value", 42).nbrValuesInserted == 1);

    SnapshotCachedPathSpace cached{backing};
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto value = cached.read<int>("/nested/value");
    REQUIRE(value.has_value());
    CHECK(value.value() == 42);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
}

TEST_CASE("Snapshot cache invalid path read returns error without metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto invalid = cached.read<int>("relative/path");
    CHECK_FALSE(invalid.has_value());
    CHECK(invalid.error().code == Error::Code::InvalidPath);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache disabled missing read leaves metrics zero") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto missing = cached.read<int>("/missing");
    CHECK_FALSE(missing.has_value());

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.hits == 0);
    CHECK(metrics.misses == 0);
}

TEST_CASE("Snapshot cache read<Children> forwards backing children") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/alpha", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/root/child", 2).nbrValuesInserted == 1);

    auto rootChildren = cached.read<Children>("/");
    REQUIRE(rootChildren.has_value());
    auto rootKids = rootChildren->names;
    std::sort(rootKids.begin(), rootKids.end());
    CHECK(rootKids == std::vector<std::string>{"alpha", "root"});

    auto nestedChildren = cached.read<Children>("/root");
    REQUIRE(nestedChildren.has_value());
    auto nestedKids = nestedChildren->names;
    std::sort(nestedKids.begin(), nestedKids.end());
    CHECK(nestedKids == std::vector<std::string>{"child"});
}

TEST_CASE("Snapshot cache read<Children> returns empty for missing path") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/only", 1).nbrValuesInserted == 1);

    auto children = cached.read<Children>("/missing");
    REQUIRE(children.has_value());
    CHECK(children->names.empty());
}

TEST_CASE("Snapshot cache read<Children> bypasses snapshot metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/first", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/root/second", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto children = cached.read<Children>("/root");
    REQUIRE(children.has_value());
    auto names = children->names;
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"first", "second"});

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache read<Children> reflects mutations while dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/first", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/root/second", 2).nbrValuesInserted == 1);

    auto children = cached.read<Children>("/root");
    REQUIRE(children.has_value());
    auto names = children->names;
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"first", "second"});
}

TEST_CASE("Snapshot cache disabled uses backing without metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 11).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 11);

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.hits == 0);
    CHECK(metrics.misses == 0);
}

TEST_CASE("Snapshot cache ignores execution reads") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 7).nbrValuesInserted == 1);
    CHECK(cached.insert("/exec", []() -> int { return 5; }, In{.executionCategory = ExecutionCategory::Lazy}).nbrTasksInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto hit = cached.read<int>("/value");
    REQUIRE(hit.has_value());
    CHECK(hit.value() == 7);

    auto before = cached.snapshotMetrics();
    auto future = cached.read("/exec");
    REQUIRE(future.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache missing FutureAny read bypasses metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 3).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto missing = cached.read("/missing");
    CHECK_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NoObjectFound);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache pop reads bypass metrics") {
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

    auto readValue = cached.read<int>("/queue");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 1);

    auto before = cached.snapshotMetrics();
    auto popped = cached.take<int>("/queue");
    REQUIRE(popped.has_value());
    CHECK(popped.value() == 1);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache blocking reads bypass metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 12).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value", Block{200ms});
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 12);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache disable resets metrics and bypasses reads") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 33).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto hit = cached.read<int>("/value");
    REQUIRE(hit.has_value());
    CHECK(hit.value() == 33);

    auto beforeDisable = cached.snapshotMetrics();
    CHECK(beforeDisable.hits >= 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    auto reset = cached.snapshotMetrics();
    CHECK(reset.hits == 0);
    CHECK(reset.misses == 0);

    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 33);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == 0);
    CHECK(after.misses == 0);
}

TEST_CASE("Snapshot cache children reads bypass snapshot metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/a", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/root/b", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto children = cached.read<Children>("/root");
    REQUIRE(children.has_value());
    CHECK(children->names.size() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache glob reads do not touch metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/alpha", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    (void)cached.read<int>("/a*");

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache visit bypasses snapshot metrics") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/a", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/root/b", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    bool sawValues = false;
    VisitOptions options{};
    options.root = "/";
    options.includeNestedSpaces = true;
    options.includeValues = true;
    auto result = cached.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            if (entry.path == "/root/a" || entry.path == "/root/b") {
                sawValues = true;
            }
            return VisitControl::Continue;
        },
        options);
    REQUIRE(result.has_value());
    CHECK(sawValues);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_SUITE_END();

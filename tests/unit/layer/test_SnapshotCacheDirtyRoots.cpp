#include "SnapshotCachedPathSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.snapshot_cache.dirty_roots");

TEST_CASE("Snapshot cache markSnapshotDirty is ignored when disabled") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto before = cached.snapshotMetrics();
    cached.markSnapshotDirty("/value");
    auto after = cached.snapshotMetrics();

    CHECK(after.hits == before.hits);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache retains shorter non-prefix dirty roots") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/b", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    cached.stopSnapshotWorker();

    cached.markSnapshotDirty("/b");
    InputMetadata meta{InputMetadataT<int>{}};
    int outValue = 0;
    auto before = cached.snapshotMetrics();
    CHECK_FALSE(cached.trySnapshotRead(Iterator{"/b"}, meta, Out{}, &outValue));
    auto afterFirst = cached.snapshotMetrics();
    CHECK(afterFirst.misses >= before.misses + 1);

    cached.markSnapshotDirty("/alpha/beta");
    auto beforeSecond = cached.snapshotMetrics();
    CHECK_FALSE(cached.trySnapshotRead(Iterator{"/b"}, meta, Out{}, &outValue));
    auto afterSecond = cached.snapshotMetrics();
    CHECK(afterSecond.misses >= beforeSecond.misses + 1);
}

TEST_CASE("Snapshot cache keeps dirty roots for invalid canonical paths") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    cached.stopSnapshotWorker();

    cached.markSnapshotDirty("relative/./bad");
    InputMetadata meta{InputMetadataT<int>{}};
    int outValue = 0;

    auto before = cached.snapshotMetrics();
    CHECK_FALSE(cached.trySnapshotRead(Iterator{"relative/./bad"}, meta, Out{}, &outValue));
    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache empty path does not match dirty roots") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    cached.stopSnapshotWorker();

    cached.markSnapshotDirty("relative/./bad");
    InputMetadata meta{InputMetadataT<int>{}};
    int outValue = 0;

    auto before = cached.snapshotMetrics();
    CHECK_FALSE(cached.trySnapshotRead(Iterator{""}, meta, Out{}, &outValue));
    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache markSnapshotDirty widens roots") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/short", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/other", 2).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    cached.stopSnapshotWorker();

    auto first = cached.read<int>("/other");
    REQUIRE(first.has_value());

    cached.markSnapshotDirty("/short");
    auto mid = cached.snapshotMetrics();
    auto stillHit = cached.read<int>("/other");
    REQUIRE(stillHit.has_value());
    auto afterShort = cached.snapshotMetrics();
    CHECK(afterShort.hits >= mid.hits + 1);

    cached.markSnapshotDirty("/");
    auto beforeRoot = cached.snapshotMetrics();
    auto rootMiss = cached.read<int>("/other");
    REQUIRE(rootMiss.has_value());
    auto afterRoot = cached.snapshotMetrics();
    CHECK(afterRoot.misses >= beforeRoot.misses + 1);
}

TEST_CASE("Snapshot cache hits and dirty fallback") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/a", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto first = cached.read<int>("/a");
    REQUIRE(first.has_value());
    CHECK(first.value() == 1);

    CHECK(cached.insert("/b", 10).nbrValuesInserted == 1);

    auto readA = cached.read<int>("/a");
    REQUIRE(readA.has_value());
    CHECK(readA.value() == 1);

    auto readB = cached.read<int>("/b");
    REQUIRE(readB.has_value());
    CHECK(readB.value() == 10);

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.hits >= 1);
    CHECK(metrics.misses >= 1);
}

TEST_CASE("Snapshot cache avoids stale reads after mutation") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.read<int>("/value");
    REQUIRE(before.has_value());
    CHECK(before.value() == 1);

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);
    auto after = cached.read<int>("/value");
    REQUIRE(after.has_value());
    CHECK(after.value() == 2);
}

TEST_CASE("Snapshot cache clamps maxDirtyRoots to 1") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/stable", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 0,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    CHECK(cached.insert("/a", 2).nbrValuesInserted == 1);
    auto hitRead = cached.read<int>("/stable");
    REQUIRE(hitRead.has_value());
    CHECK(hitRead.value() == 1);
    auto mid = cached.snapshotMetrics();
    CHECK(mid.hits >= before.hits + 1);

    CHECK(cached.insert("/b", 3).nbrValuesInserted == 1);
    auto missRead = cached.read<int>("/stable");
    REQUIRE(missRead.has_value());
    CHECK(missRead.value() == 1);
    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= mid.misses + 1);
}

TEST_CASE("Snapshot cache isolates dirty roots from clean paths") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/churn", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/churn", 2).nbrValuesInserted == 1);
    CHECK(cached.insert("/stable", 9).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();

    auto taken = cached.take<int>("/churn");
    REQUIRE(taken.has_value());
    CHECK(taken.value() == 1);

    auto churnRead = cached.read<int>("/churn");
    REQUIRE(churnRead.has_value());
    CHECK(churnRead.value() == 2);

    auto stableRead = cached.read<int>("/stable");
    REQUIRE(stableRead.has_value());
    CHECK(stableRead.value() == 9);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits >= before.hits + 1);
}

TEST_CASE("Snapshot cache dirty root covers child path") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/child", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/other", 2).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/root", 5).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto childRead = cached.read<int>("/root/child");
    REQUIRE(childRead.has_value());
    CHECK(childRead.value() == 1);

    auto otherRead = cached.read<int>("/other");
    REQUIRE(otherRead.has_value());
    CHECK(otherRead.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits >= before.hits + 1);
}

TEST_CASE("Snapshot cache dirty root does not match partial component") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/value", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/rooted/value", 2).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/root", 5).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto rootedRead = cached.read<int>("/rooted/value");
    REQUIRE(rootedRead.has_value());
    CHECK(rootedRead.value() == 2);

    auto afterRooted = cached.snapshotMetrics();
    CHECK(afterRooted.hits >= before.hits + 1);

    auto rootRead = cached.read<int>("/root/value");
    REQUIRE(rootRead.has_value());
    CHECK(rootRead.value() == 1);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= afterRooted.misses + 1);
}

TEST_CASE("Snapshot cache dirty root does not block other branch") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/child", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/root/other", 5).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/root/child", 2, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto otherRead = cached.read<int>("/root/other");
    REQUIRE(otherRead.has_value());
    CHECK(otherRead.value() == 5);

    auto mid = cached.snapshotMetrics();
    CHECK(mid.hits >= before.hits + 1);

    auto childRead = cached.read<int>("/root/child");
    REQUIRE(childRead.has_value());
    CHECK(childRead.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= mid.misses + 1);
}

TEST_CASE("Snapshot cache dirty root does not match sibling prefix") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/child", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/root/childish", 9).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/root/child", 2, ReplaceExisting{}).nbrValuesInserted == 1);
    CHECK(backing->insert("/root/childish", 10, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto childRead = cached.read<int>("/root/child");
    REQUIRE(childRead.has_value());
    CHECK(childRead.value() == 2);

    auto mid = cached.snapshotMetrics();
    CHECK(mid.misses >= before.misses + 1);

    auto siblingRead = cached.read<int>("/root/childish");
    REQUIRE(siblingRead.has_value());
    CHECK(siblingRead.value() == 9);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits >= mid.hits);
}

TEST_CASE("Snapshot cache widens dirty roots to ancestor on mutation") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/a/b", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/a/c", 2).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/a/b", 3, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readCleanSibling = cached.read<int>("/a/c");
    REQUIRE(readCleanSibling.has_value());
    CHECK(readCleanSibling.value() == 2);

    auto mid = cached.snapshotMetrics();
    CHECK(mid.hits >= before.hits + 1);

    CHECK(cached.insert("/a", 5).nbrValuesInserted == 1);
    auto readAfterAncestor = cached.read<int>("/a/c");
    REQUIRE(readAfterAncestor.has_value());
    CHECK(readAfterAncestor.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= mid.misses + 1);
}

TEST_CASE("Snapshot cache promotes to root dirty when maxDirtyRoots exceeded") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/a", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 2).nbrValuesInserted == 1);
    CHECK(cached.insert("/c", 3).nbrValuesInserted == 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 1,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();

    CHECK(cached.insert("/a", 10, ReplaceExisting{}).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 20, ReplaceExisting{}).nbrValuesInserted == 1);

    auto readC = cached.read<int>("/c");
    REQUIRE(readC.has_value());
    CHECK(readC.value() == 3);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache root dirty forces misses for multiple reads") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/a", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 1,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/a", 3, ReplaceExisting{}).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 4, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readA = cached.read<int>("/a");
    REQUIRE(readA.has_value());
    CHECK(readA.value() == 3);
    auto mid = cached.snapshotMetrics();
    auto readB = cached.read<int>("/b");
    REQUIRE(readB.has_value());
    CHECK(readB.value() == 4);
    auto after = cached.snapshotMetrics();

    CHECK(mid.misses >= before.misses + 1);
    CHECK(after.misses >= mid.misses + 1);
    CHECK(after.hits == before.hits);
}

TEST_CASE("Snapshot cache root dirty misses on missing path") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/a", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 1,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/a", 3, ReplaceExisting{}).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 4, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto missing = cached.read<int>("/missing");
    CHECK_FALSE(missing.has_value());
    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache dirty root matches exact path") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/root/value", 3, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/root/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 3);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits == before.hits);
}

TEST_CASE("Snapshot cache replacement under dirty root reads new value") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/root/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/root/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);
    auto readValue = cached.read<int>("/root/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 2);
}

TEST_CASE("Snapshot cache build failure keeps dirty flag (miss persists)") {
    struct FailingVisitSpace final : PathSpace {
        auto visit(PathVisitor const&, VisitOptions const&) -> Expected<void> override {
            return std::unexpected(Error{Error::Code::InvalidPermissions, "forced visit failure"});
        }
    };

    auto backing = std::make_shared<FailingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    cached.rebuildSnapshotNow();
    auto before = cached.snapshotMetrics();
    auto readMissing = cached.read<int>("/value");
    CHECK_FALSE(readMissing.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.misses == before.misses + 1);
    CHECK(after.rebuildFailures >= 1);
}

TEST_SUITE_END();

#include "SnapshotCachedPathSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.snapshot_cache.metrics");

TEST_CASE("Snapshot cache rebuild failures do not increment hits") {
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
    auto miss = cached.read<int>("/missing");
    CHECK_FALSE(miss.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits);
}

TEST_CASE("Snapshot cache miss count increments on repeated misses") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto miss1 = cached.read<int>("/missing");
    CHECK_FALSE(miss1.has_value());
    auto mid = cached.snapshotMetrics();
    auto miss2 = cached.read<int>("/missing");
    CHECK_FALSE(miss2.has_value());
    auto after = cached.snapshotMetrics();

    CHECK(mid.misses == before.misses + 1);
    CHECK(after.misses == mid.misses + 1);
}

TEST_CASE("Snapshot cache hit count increments on repeated hits") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 7).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto hit1 = cached.read<int>("/value");
    REQUIRE(hit1.has_value());
    auto mid = cached.snapshotMetrics();
    auto hit2 = cached.read<int>("/value");
    REQUIRE(hit2.has_value());
    auto after = cached.snapshotMetrics();

    CHECK(mid.hits == before.hits + 1);
    CHECK(after.hits == mid.hits + 1);
}

TEST_CASE("Snapshot cache bytes metric stays stable across reads") {
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
    auto hit = cached.read<int>("/value");
    REQUIRE(hit.has_value());
    auto miss = cached.read<int>("/missing");
    CHECK_FALSE(miss.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.bytes == before.bytes);
}

TEST_CASE("Snapshot cache bytes grow after rebuild with added values") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/first", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    REQUIRE(before.bytes > 0);

    CHECK(cached.insert("/second", 2).nbrValuesInserted == 1);
    cached.rebuildSnapshotNow();

    auto after = cached.snapshotMetrics();
    CHECK(after.bytes > before.bytes);
}

TEST_CASE("Snapshot cache bytes reset on reconfigure") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 8).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    REQUIRE(before.bytes > 0);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto after = cached.snapshotMetrics();
    CHECK(after.bytes == 0);
}

TEST_CASE("Snapshot cache bytes stay unchanged on rebuild failure") {
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

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.bytes == 0);
    CHECK(metrics.rebuildFailures >= 1);
}

TEST_CASE("Snapshot cache metrics reset on reconfigure") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto hit = cached.read<int>("/value");
    REQUIRE(hit.has_value());
    CHECK(hit.value() == 1);

    auto before = cached.snapshotMetrics();
    CHECK(before.hits >= 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == 0);
    CHECK(after.misses == 0);
    CHECK(after.rebuilds == 0);
}

TEST_CASE("Snapshot cache lastRebuildMs resets on reconfigure") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    CHECK(before.lastRebuildMs.count() >= 0);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto after = cached.snapshotMetrics();
    CHECK(after.lastRebuildMs.count() == 0);
}

TEST_CASE("Snapshot cache reports bytes and rebuilds after rebuild") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 123).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds >= 1);
    CHECK(metrics.bytes > 0);
}

TEST_CASE("Snapshot cache rebuild on empty space reports zero bytes") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds >= 1);
    CHECK(metrics.bytes == 0);
}

TEST_CASE("Snapshot cache rebuild failure increments failure metrics") {
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
    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuildFailures >= 1);
    CHECK(metrics.rebuilds == 0);
}

TEST_CASE("Snapshot cache rebuild failure with missing backing increments metrics") {
    SnapshotCachedPathSpace cached{std::shared_ptr<PathSpaceBase>{}};
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    cached.rebuildSnapshotNow();
    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuildFailures >= 1);
    CHECK(metrics.rebuilds == 0);
}

TEST_CASE("Snapshot cache metrics default to zero before configuration") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK_FALSE(cached.snapshotEnabled());
    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.hits == 0);
    CHECK(metrics.misses == 0);
    CHECK(metrics.rebuilds == 0);
    CHECK(metrics.rebuildFailures == 0);
    CHECK(metrics.bytes == 0);
    CHECK(metrics.lastRebuildMs.count() == 0);
}

TEST_CASE("Snapshot cache rebuild count increments on consecutive rebuilds") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 3).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    cached.rebuildSnapshotNow();
    auto first = cached.snapshotMetrics();
    cached.rebuildSnapshotNow();
    auto second = cached.snapshotMetrics();
    CHECK(second.rebuilds >= first.rebuilds + 1);
}

TEST_CASE("Snapshot cache rebuild failure increments on repeated attempts") {
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
    cached.rebuildSnapshotNow();
    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuildFailures >= 2);
    CHECK(metrics.rebuilds == 0);
}

TEST_CASE("Snapshot cache miss increments for missing path") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 5).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto missing = cached.read<int>("/missing");
    CHECK_FALSE(missing.has_value());

    auto after = cached.snapshotMetrics();
    CHECK(after.misses == before.misses + 1);
    CHECK(after.hits == before.hits);
}

TEST_CASE("Snapshot cache hit increments on clean read") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 9).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto hit = cached.read<int>("/value");
    REQUIRE(hit.has_value());
    CHECK(hit.value() == 9);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits == before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache miss increments when dirty root covers read") {
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
    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/root/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses == before.misses + 1);
}

TEST_SUITE_END();

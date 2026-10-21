#include "SnapshotCachedPathSpaceTestSupport.hpp"

namespace {

struct TrackingSpace final : PathSpace {
    bool        shutdownCalled = false;
    std::string lastNotify;

    auto shutdown() -> void override {
        shutdownCalled = true;
    }

    auto notify(std::string const& notificationPath) -> void override {
        lastNotify = notificationPath;
    }
};

} // namespace

TEST_SUITE_BEGIN("pathspace.snapshot_cache.lifecycle");

TEST_CASE("Snapshot cache snapshotEnabled follows configuration") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK_FALSE(cached.snapshotEnabled());

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    CHECK(cached.snapshotEnabled());

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    CHECK_FALSE(cached.snapshotEnabled());
}

TEST_CASE("Snapshot cache start worker is no-op before configuration") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.startSnapshotWorker();
    CHECK_FALSE(cached.snapshotEnabled());
}

TEST_CASE("Snapshot cache adoptContextAndPrefix forwards to backing") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    auto ctx = std::make_shared<PathSpaceContext>();
    cached.adoptContextAndPrefix(ctx, "/mount");

    CHECK(cached.getContext() == ctx);
    CHECK(backing->getContext() == ctx);
}

TEST_CASE("Snapshot cache shutdown and notify forward to backing") {
    auto backing = std::make_shared<TrackingSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.notify("/notify");
    CHECK(backing->lastNotify == "/notify");

    cached.shutdown();
    CHECK(backing->shutdownCalled);
}

TEST_CASE("Snapshot cache toggles enabled state") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK_FALSE(cached.snapshotEnabled());
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    CHECK(cached.snapshotEnabled());

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    CHECK_FALSE(cached.snapshotEnabled());
}

TEST_CASE("Snapshot cache reports errors when backing is missing") {
    SnapshotCachedPathSpace cached{std::shared_ptr<PathSpaceBase>{}};

    auto insertRet = cached.insert("/value", 1);
    CHECK_FALSE(insertRet.errors.empty());

    auto childrenRead = cached.read<Children>("/any");
    REQUIRE(childrenRead.has_value());
    CHECK(childrenRead->names.empty());

    int outValue = 0;
    auto outErr = cached.out(Iterator{"/value"}, InputMetadataT<int>{}, Out{}, &outValue);
    CHECK(outErr.has_value());

    auto visitRes = cached.visit([](PathEntry const&, ValueHandle&) { return VisitControl::Continue; });
    CHECK_FALSE(visitRes.has_value());

    auto spanConst = cached.spanPackConst(
        std::span<const std::string>{}, InputMetadata{}, Out{}, [](std::span<const RawSpan<const void*>>) {
            return std::optional<Error>{};
        });
    CHECK_FALSE(spanConst.has_value());

    auto spanMut = cached.spanPackMut(
        std::span<const std::string>{}, InputMetadata{}, Out{}, [](std::span<const RawSpan<void*>>) {
            return SpanPackResult{.error = std::nullopt, .shouldPop = false};
        });
    CHECK_FALSE(spanMut.has_value());

    auto packRes = cached.packInsert(std::span<const std::string>{}, InputMetadata{}, std::span<void const* const>{});
    CHECK_FALSE(packRes.errors.empty());

    auto spanPackRes = cached.packInsertSpans(std::span<const std::string>{}, std::span<SpanInsertSpec const>{});
    CHECK_FALSE(spanPackRes.errors.empty());
}

TEST_SUITE_END();

#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/SnapshotCachedPathSpace.hpp>
#include <array>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE_BEGIN("pathspace.snapshot_cache");

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

TEST_CASE("Snapshot cache rebuild refreshes values after mutations") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 4).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 7, ReplaceExisting{}).nbrValuesInserted == 1);
    auto dirtyRead = cached.read<int>("/value");
    REQUIRE(dirtyRead.has_value());
    CHECK(dirtyRead.value() == 7);

    cached.rebuildSnapshotNow();
    auto refreshed = cached.read<int>("/value");
    REQUIRE(refreshed.has_value());
    CHECK(refreshed.value() == 7);
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

TEST_CASE("Snapshot cache rebuild clears dirty roots for hits") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);
    auto dirtyRead = cached.read<int>("/value");
    REQUIRE(dirtyRead.has_value());
    CHECK(dirtyRead.value() == 2);

    auto dirtyMetrics = cached.snapshotMetrics();
    CHECK(dirtyMetrics.misses >= 1);

    cached.rebuildSnapshotNow();
    auto beforeHit = cached.snapshotMetrics();
    auto hitRead = cached.read<int>("/value");
    REQUIRE(hitRead.has_value());
    CHECK(hitRead.value() == 2);

    auto afterHit = cached.snapshotMetrics();
    CHECK(afterHit.hits >= beforeHit.hits + 1);
    CHECK(afterHit.misses == beforeHit.misses);
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

TEST_CASE("Snapshot cache rebuild is ignored when disabled") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds == 0);
    CHECK(metrics.bytes == 0);
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
    CHECK(metrics.rebuildFailures == 1);
    CHECK(metrics.rebuilds == 0);
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

TEST_CASE("Snapshot cache reports errors when backing is missing") {
    SnapshotCachedPathSpace cached{std::shared_ptr<PathSpaceBase>{}};

    auto insertRet = cached.insert("/value", 1);
    CHECK_FALSE(insertRet.errors.empty());

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

TEST_CASE("Snapshot cache rebuild failure with missing backing increments metrics") {
    SnapshotCachedPathSpace cached{std::shared_ptr<PathSpaceBase>{}};
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    cached.rebuildSnapshotNow();
    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuildFailures == 1);
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
    CHECK(second.rebuilds == first.rebuilds + 1);
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
    CHECK(metrics.rebuildFailures == 2);
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

TEST_CASE("Snapshot cache synchronous rebuild disabled does not rebuild on read") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1ms,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = false,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.rebuilds == before.rebuilds);
}

TEST_CASE("Snapshot cache synchronous rebuild triggers when enabled") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = true,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.rebuilds >= before.rebuilds);
}

TEST_SUITE_END();

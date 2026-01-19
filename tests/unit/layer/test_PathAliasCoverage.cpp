#include "layer/PathAlias.hpp"
#include "PathSpace.hpp"
#include "path/Iterator.hpp"
#include "type/InputMetadataT.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputData.hpp"
#include "third_party/doctest.h"
#include <span>
#include <vector>
#include "core/PathSpaceContext.hpp"
#include "core/NotificationSink.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>

using namespace SP;

TEST_SUITE("layer.pathalias.coverage") {
TEST_CASE("PathAlias rewrites inserts and reads via target prefix") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/upstream"};

    // Insert through alias, verify upstream value.
    auto ins = alias.in(Iterator{"/node"}, InputData{123});
    REQUIRE(ins.errors.empty());

    auto direct = upstream->read<int>("/upstream/node");
    CHECK(direct.has_value());
    CHECK(*direct == 123);

    // Read back through alias.
    auto viaAlias = alias.read<int>("/node");
    CHECK(viaAlias.has_value());
    CHECK(*viaAlias == 123);

    // Retarget and ensure new inserts go to the updated prefix.
    alias.setTargetPrefix("/newroot/");
    auto ins2 = alias.in(Iterator{"/second"}, InputData{321});
    REQUIRE(ins2.errors.empty());

    auto newVal = upstream->read<int>("/newroot/second");
    CHECK(newVal.has_value());
    CHECK(*newVal == 321);
}

TEST_CASE("PathAlias children listing and notify path mapping") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/mount"};

    upstream->insert("/mount/a", 1);
    upstream->insert("/mount/b", 2);

    auto children = alias.read<Children>("/");
    REQUIRE(children.has_value());
    CHECK(children->names.size() == 2);

    // Exercise notify mapping; no observable state change needed for coverage.
    alias.notify("/");
    alias.notify("/_system");
}

TEST_CASE("PathAlias surfaces errors when upstream is missing") {
    PathAlias alias{std::shared_ptr<PathSpaceBase>{}, "/missing"};

    auto insertResult = alias.in(Iterator{"/value"}, InputData{42});
    CHECK_FALSE(insertResult.errors.empty());

    int outValue = 0;
    auto outErr = alias.out(Iterator{"/value"}, InputMetadataT<int>{}, Out{}, &outValue);
    CHECK(outErr.has_value());

    auto spanConst = alias.spanPackConst(
        std::span<const std::string>{}, InputMetadata{}, Out{}, [](std::span<const RawSpan<const void*>>) {
            return std::optional<Error>{};
        });
    CHECK_FALSE(spanConst);

    auto spanMut = alias.spanPackMut(
        std::span<const std::string>{}, InputMetadata{}, Out{}, [](std::span<const RawSpan<void*>>) {
            return SpanPackResult{.error = std::nullopt, .shouldPop = false};
        });
    CHECK_FALSE(spanMut);

    auto packRes = alias.packInsert(std::span<const std::string>{}, InputMetadata{}, std::span<void const* const>{});
    CHECK_FALSE(packRes.errors.empty());

    auto visitRes = alias.visit([](PathEntry const&, ValueHandle&) { return VisitControl::Continue; });
    CHECK_FALSE(visitRes);
}

struct RecordingSink : NotificationSink {
    void notify(std::string const& notificationPath) override { paths.push_back(notificationPath); }
    std::vector<std::string> paths;
};

TEST_CASE("PathAlias retargeting notifies mount prefix when known") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "target///"};

    auto ctx  = std::make_shared<PathSpaceContext>();
    auto sink = std::make_shared<RecordingSink>();
    ctx->setSink(sink);

    // Mount the alias so retargeting can notify a concrete prefix.
    alias.adoptContextAndPrefix(ctx, "/alias/mount");

    std::atomic<std::cv_status> waitStatus{std::cv_status::timeout};
    std::thread waiter([&] {
        auto guard = ctx->wait("/alias/mount");
        waitStatus.store(guard.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(250)));
    });

    // Give the waiter time to register before retargeting.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Retarget with missing leading slash and trailing slashes to exercise normalization.
    alias.setTargetPrefix("newroot///");
    waiter.join();

    CHECK(alias.targetPrefix() == "/newroot");
    CHECK(waitStatus.load() == std::cv_status::no_timeout);
    REQUIRE_FALSE(sink->paths.empty());
    CHECK(sink->paths.back() == "/alias/mount");
}

TEST_CASE("PathAlias retargeting falls back to notifyAll when mount prefix is unknown") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/root"};

    auto ctx = std::make_shared<PathSpaceContext>();
    alias.adoptContextAndPrefix(ctx, "");

    std::atomic<int> woke{0};
    auto waiter = [&](std::string path) {
        auto guard = ctx->wait(path);
        if (guard.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(250)) == std::cv_status::no_timeout) {
            ++woke;
        }
    };

    std::thread w1(waiter, "/foo");
    std::thread w2(waiter, "/bar");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    alias.setTargetPrefix("/next");

    w1.join();
    w2.join();

    CHECK(woke.load() == 2);
}

TEST_CASE("PathAlias visit remaps target prefix to alias root") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/target"};

    REQUIRE(upstream->insert("/target/child", 5).errors.empty());
    REQUIRE(upstream->insert("/target/nested/grand", 7).errors.empty());

    std::vector<std::string> paths;
    auto visitRes = alias.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            paths.push_back(entry.path);
            if (entry.path == "/child") {
                auto val = handle.read<int>();
                REQUIRE(val.has_value());
                CHECK(*val == 5);
            }
            return VisitControl::Continue;
        });
    REQUIRE(visitRes);

    CHECK(paths.size() >= 3); // "/", "/child", "/nested", "/nested/grand"
    CHECK(paths.front() == "/");
    CHECK(std::find(paths.begin(), paths.end(), "/child") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/nested/grand") != paths.end());
}

TEST_CASE("PathAlias rejects glob paths for insert and read") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/root"};

    auto insertResult = alias.in(Iterator{"/*"}, InputData{99});
    CHECK_FALSE(insertResult.errors.empty());
    CHECK(insertResult.errors.front().code == Error::Code::InvalidPath);

    int value = 0;
    auto readErr = alias.out(Iterator{"/*"}, InputMetadataT<int>{}, Out{}, &value);
    CHECK(readErr.has_value());
    CHECK(readErr->code == Error::Code::InvalidPath);

    auto children = upstream->read<Children>("/root");
    REQUIRE(children.has_value());
    CHECK(children->names.empty()); // glob insert didn't create new nodes
}
}

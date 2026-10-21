#include "PathSpaceNestingTestSupport.hpp"

namespace {

struct CoordinatedVisitSpace : PathSpace {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool visitEntered = false;
        bool allowFinish = false;
    };

    explicit CoordinatedVisitSpace(std::shared_ptr<State> state) : state(std::move(state)) {}

    auto visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> override {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->visitEntered = true;
            state->cv.notify_all();
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&]() { return state->allowFinish; });
        return PathSpace::visit(visitor, options);
    }

private:
    std::shared_ptr<State> state;
};

struct CoordinatedListSpace : PathSpace {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool listingStarted = false;
        bool allowFinish = false;
        bool listingDone = false;
    };

    explicit CoordinatedListSpace(std::shared_ptr<State> state) : state(std::move(state)) {}

    auto listChildrenCanonical(std::string_view) const -> std::vector<std::string> override {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->listingStarted = true;
            state->cv.notify_all();
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&]() { return state->allowFinish; });
        state->listingDone = true;
        return {"child"};
    }

private:
    std::shared_ptr<State> state;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.nesting.indexed");

TEST_CASE("PathSpace Nesting/Glob insert applies to all nested instances") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto result = root.insert("/mount/*/value", 7);
    CHECK(result.errors.empty());
    CHECK(result.nbrValuesInserted == 2);

    auto firstVal = root.read<int>("/mount/space/value", Block{});
    auto secondVal = root.read<int>("/mount/space[1]/value", Block{});
    REQUIRE(firstVal.has_value());
    REQUIRE(secondVal.has_value());
    CHECK(firstVal.value() == 7);
    CHECK(secondVal.value() == 7);
}

TEST_CASE("PathSpace Nesting/indexed value insert is rejected") {
    PathSpace root;

    auto result = root.insert("/mount/space[1]", 5);

    CHECK(result.nbrValuesInserted == 0);
    CHECK(result.nbrSpacesInserted == 0);
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].code == Error::Code::InvalidPath);

    auto children = root.read<Children>("/");
    REQUIRE(children.has_value());
    CHECK(children->names.empty());
}

TEST_CASE("PathSpace Nesting/Extract nested space by explicit index") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(first->insert("/v", 10).nbrValuesInserted == 1);
    REQUIRE(second->insert("/v", 20).nbrValuesInserted == 1);

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto secondTaken = root.take<std::unique_ptr<PathSpace>>("/mount/space[1]", Block{200ms});
    REQUIRE(secondTaken.has_value());
    auto valSecond = secondTaken.value()->read<int>("/v", Block{200ms});
    REQUIRE(valSecond.has_value());
    CHECK(valSecond.value() == 20);

    auto remainingFront = root.read<int>("/mount/space/v", Block{200ms});
    REQUIRE(remainingFront.has_value());
    CHECK(remainingFront.value() == 10);

    auto missing = root.read<int>("/mount/space[1]/v", Block{200ms});
    CHECK(!missing.has_value());
}

TEST_CASE("PathSpace Nesting/second nested space adopts indexed mount prefix") {
    IntrospectablePathSpace root;
    auto first    = std::make_unique<IntrospectablePathSpace>();
    auto second   = std::make_unique<IntrospectablePathSpace>();
    auto* firstRaw  = first.get();
    auto* secondRaw = second.get();

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    CHECK(firstRaw->prefixStr() == "/mount/space");
    CHECK(secondRaw->prefixStr() == "/mount/space[1]");
}

TEST_CASE("PathSpace Nesting/listChildren honours explicit nested index") {
    PathSpace root;
    auto first  = std::make_unique<PathSpace>();
    auto second = std::make_unique<PathSpace>();
    REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
    REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto merged = root.read<Children>("/mount/space");
    REQUIRE(merged.has_value());
    INFO("merged children: " << [&]() {
             std::string s;
             for (auto const& c : merged->names) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());

    auto children = root.read<Children>("/mount/space[1]");
    REQUIRE(children.has_value());
    std::sort(children->names.begin(), children->names.end());
    INFO("children: " << [&]() {
             std::string s;
             for (auto const& c : children->names) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());
    CHECK(std::find(children->names.begin(), children->names.end(), "b") != children->names.end());
    CHECK(std::find(children->names.begin(), children->names.end(), "a") == children->names.end());

    auto value = root.read<int>("/mount/space[1]/b", Block{});
    CHECK_MESSAGE(value.has_value(),
                  "nested index read failed code=" << static_cast<int>(value.error().code));
    if (value.has_value()) {
        CHECK(value.value() == 2);
    }
}

TEST_CASE("PathSpace Nesting/visit rooted at indexed nested space stays isolated") {
    PathSpace root;
    auto first  = std::make_unique<PathSpace>();
    auto second = std::make_unique<PathSpace>();
    REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
    REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    std::vector<std::string> visited;
    VisitOptions opts;
    opts.root                = "/mount/space[1]";
    opts.includeNestedSpaces = true;
    root.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.emplace_back(entry.path);
        return VisitControl::Continue;
    }, opts);

    INFO("visited paths: " << [&]() {
             std::string s;
             for (auto const& p : visited) {
                 if (!s.empty()) s.append(",");
                 s.append(p);
             }
             return s;
         }());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space[1]") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space[1]/b") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space/a") == visited.end());
}

TEST_CASE("PathSpace Nesting/cloned nested space preserves indexed notifications") {
    PathSpace root;
    REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    PathSpace clone = root.clone();
    auto* mountNode = PathSpaceTestHelper::root(clone)->getChild("mount");
    REQUIRE(mountNode != nullptr);
    auto* spaceNode = mountNode->getChild("space");
    REQUIRE(spaceNode != nullptr);
    REQUIRE(spaceNode->data);
    CHECK(spaceNode->data->nestedCount() == 2);

    std::atomic<bool> done{false};
    std::atomic<bool> insertOk{false};
    std::thread writer([&]() {
        std::this_thread::sleep_for(50ms);
        auto res = clone.insert("/mount/space[1]/value", 99);
        INFO("insert counts values=" << res.nbrValuesInserted << " spaces=" << res.nbrSpacesInserted
             << " tasks=" << res.nbrTasksInserted << " errors=" << res.errors.size());
        if (!res.errors.empty()) {
            INFO("insert error code=" << static_cast<int>(res.errors.front().code)
                 << " msg=" << res.errors.front().message.value_or(""));
        }
        insertOk = res.errors.empty() && res.nbrValuesInserted == 1;
        done     = true;
    });

    auto result = clone.read<int>("/mount/space[1]/value", Block{200ms});
    writer.join();

    CHECK(done.load());
    CHECK(insertOk.load());
    INFO("read error code=" << (result.has_value() ? 0 : static_cast<int>(result.error().code)));
    REQUIRE(result.has_value());
    CHECK(result.value() == 99);
}

TEST_CASE("PathSpace Nesting/clone supports indexed insert and read") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(first->insert("/value0", 10).nbrValuesInserted == 1);
    REQUIRE(second->insert("/value1", 20).nbrValuesInserted == 1);

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    PathSpace clone = root.clone();

    auto insertRes = clone.insert("/mount/space[1]/value2", 99);
    CHECK(insertRes.errors.empty());
    CHECK(insertRes.nbrValuesInserted == 1);

    auto v0 = clone.read<int>("/mount/space/value0");
    auto v1 = clone.read<int>("/mount/space[1]/value1");
    auto v2 = clone.read<int>("/mount/space[1]/value2");
    REQUIRE(v0.has_value());
    REQUIRE(v1.has_value());
    REQUIRE(v2.has_value());
    CHECK(v0.value() == 10);
    CHECK(v1.value() == 20);
    CHECK(v2.value() == 99);

    // Source tree should remain unchanged by clone edits
    auto missing = root.read<int>("/mount/space[1]/value2");
    CHECK_FALSE(missing.has_value());
}

TEST_CASE("PathSpace Nesting/clone lists and visits indexed nested children") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
    REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);
    REQUIRE(second->insert("/c", 3).nbrValuesInserted == 1);

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    PathSpace clone = root.clone();

    auto children = clone.read<Children>("/mount/space[1]");
    REQUIRE(children.has_value());
    CHECK(std::find(children->names.begin(), children->names.end(), "b") != children->names.end());
    CHECK(std::find(children->names.begin(), children->names.end(), "c") != children->names.end());
    CHECK(std::find(children->names.begin(), children->names.end(), "a") == children->names.end());

    std::vector<std::string> visited;
    VisitOptions             opts;
    opts.root                = "/mount/space[1]";
    opts.includeNestedSpaces = true;
    opts.includeValues       = true;

    auto visitRes = clone.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);
    CHECK(visitRes.has_value());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space[1]") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space[1]/b") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space[1]/c") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space/a") == visited.end());
}

TEST_CASE("PathSpace Nesting/clone retains nested spaces when snapshot is unavailable") {
    PathSpace source;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/value", 17).nbrValuesInserted == 1);
    REQUIRE(source.insert("/mount/space", std::move(nested)).nbrSpacesInserted == 1);

    auto taskInsert = source.insert("/mount/space", []() { return 3; });
    CHECK(taskInsert.errors.empty());

    PathSpace clone = source.clone();
    auto      value = clone.read<int>("/mount/space/value", Block{});
    CHECK_MESSAGE(value.has_value(),
                  "nested value should survive clone even when snapshot is unavailable");
    if (value.has_value()) {
        CHECK(value.value() == 17);
    }
}

TEST_CASE("PathSpace Nesting/reading value with explicit index on non-nested node fails") {
    PathSpace root;
    REQUIRE(root.insert("/value", 5).nbrValuesInserted == 1);
    auto result = root.read<int>("/value[1]", Block{200ms});
    CHECK_FALSE(result.has_value());
    auto code = result.error().code;
    CHECK(((code == Error::Code::NoSuchPath) || (code == Error::Code::Timeout)));
}

TEST_CASE("PathSpace Nesting/listChildren returns empty for missing nested index") {
    PathSpace root;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/a", 1).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(nested)).nbrSpacesInserted == 1);

    auto children = root.read<Children>("/mount/space[1]");
    REQUIRE(children.has_value());
    CHECK(children->names.empty());
}

TEST_CASE("PathSpace Nesting/listChildren holds nested alive during concurrent take") {
    {
        PathSpace root;
        auto listState = std::make_shared<CoordinatedListSpace::State>();
        auto slow = std::make_unique<CoordinatedListSpace>(listState);
        REQUIRE(root.insert("/mount/slow", std::move(slow)).nbrSpacesInserted == 1);

        std::atomic<bool> listerHasValue{false};
        std::atomic<bool> listerOk{false};
        std::atomic<bool> takeOk{false};
        std::atomic<bool> takeNotNull{false};
        std::thread lister([&]() {
            auto children = root.read<Children>("/mount/slow");
            listerHasValue.store(children.has_value());
            if (children.has_value()) {
                listerOk.store(std::find(children->names.begin(), children->names.end(), "child") != children->names.end());
            }
        });

        {
            std::unique_lock<std::mutex> lock(listState->mutex);
            REQUIRE(listState->cv.wait_for(lock, 200ms, [&]() { return listState->listingStarted; }));
        }

        std::promise<void> takeDone;
        auto takeFuture = takeDone.get_future();
        std::thread taker([&]() {
            auto taken = root.take<std::unique_ptr<PathSpace>>("/mount/slow", Block{});
            takeOk.store(taken.has_value());
            if (taken.has_value()) {
                takeNotNull.store(taken.value() != nullptr);
            }
            auto owned = taken.has_value() ? std::move(taken.value()) : nullptr;
            owned.reset(); // destroy nested space after borrow holders release
            takeDone.set_value();
        });

        CHECK(takeFuture.wait_for(20ms) == std::future_status::timeout);
        {
            std::lock_guard<std::mutex> lock(listState->mutex);
            listState->allowFinish = true;
        }
        listState->cv.notify_all();

        taker.join();
        lister.join();
        CHECK(listerHasValue.load());
        CHECK(listerOk.load());
        CHECK(takeOk.load());
        CHECK(takeNotNull.load());
        CHECK(listState->listingDone);
    }
}

TEST_CASE("PathSpace Nesting/visit holds nested alive during concurrent take") {
    PathSpace root;
    auto visitState = std::make_shared<CoordinatedVisitSpace::State>();
    auto slow       = std::make_unique<CoordinatedVisitSpace>(visitState);
    REQUIRE(root.insert("/mount/slow", std::move(slow)).nbrSpacesInserted == 1);

    std::atomic<bool> visitorOk{false};
    std::atomic<bool> takeOk{false};
    std::thread visitor([&]() {
        VisitOptions opts;
        opts.root                = "/";
        opts.includeNestedSpaces = true;
        auto result = root.visit(
            [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; }, opts);
        visitorOk.store(result.has_value());
    });

    {
        std::unique_lock<std::mutex> lock(visitState->mutex);
        REQUIRE(visitState->cv.wait_for(lock, 200ms, [&]() { return visitState->visitEntered; }));
    }

    std::promise<void> takeDone;
    auto takeFuture = takeDone.get_future();
    std::thread taker([&]() {
        auto taken = root.take<std::unique_ptr<PathSpace>>("/mount/slow", Block{});
        takeOk.store(taken.has_value());
        if (taken.has_value()) {
            auto owned = std::move(*taken);
            owned.reset();
        }
        takeDone.set_value();
    });

    CHECK(takeFuture.wait_for(20ms) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(visitState->mutex);
        visitState->allowFinish = true;
    }
    visitState->cv.notify_all();

    taker.join();
    visitor.join();
    CHECK(visitorOk.load());
    CHECK(takeOk.load());
}

TEST_CASE("PathSpace Nesting/blocking read waits for indexed nested space arrival") {
    PathSpace root;
    REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    std::atomic<bool> inserted{false};
    std::thread writer([&]() {
        std::this_thread::sleep_for(50ms);
        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/v", 7).nbrValuesInserted == 1);
        auto res = root.insert("/mount/space", std::move(nested));
        inserted = res.errors.empty();
    });

    auto result = root.read<int>("/mount/space[1]/v", Block{200ms});
    writer.join();

    CHECK(inserted.load());
    REQUIRE(result.has_value());
    CHECK(result.value() == 7);
}

TEST_SUITE_END();

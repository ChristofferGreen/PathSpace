#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include "core/PathSpaceContext.hpp"
#include "core/Node.hpp"
#include "tools/PathSpaceJsonExporter.hpp"
#include "task/Task.hpp"
#include "PathSpaceTestHelper.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>
#include <mutex>
#include <memory>
#include <string>
#include "task/TaskPool.hpp"

using namespace SP;
using namespace std::chrono_literals;

namespace {
struct IntrospectablePathSpace : PathSpace {
    using PathSpace::PathSpace;
    using PathSpace::shutdown;
    std::string prefixStr() const { return this->prefix; }
    std::shared_ptr<PathSpaceContext> contextPtr() const { return this->getContext(); }
    Node* rootNodePtr() { return this->getRootNode(); }
};

struct LockAwareSpace : PathSpace {
    using PathSpace::PathSpace;
    void setParentMutex(std::mutex* m) { parentMutex = m; }
    bool lastTryLockSucceeded() const { return lastTryLockSuccess; }
    auto listChildrenCanonical(std::string_view) const -> std::vector<std::string> override {
        if (parentMutex) {
            if (parentMutex->try_lock()) {
                lastTryLockSuccess = true;
                parentMutex->unlock();
            } else {
                lastTryLockSuccess = false;
            }
        }
        return {"child"};
    }

private:
    mutable bool     lastTryLockSuccess = false;
    std::mutex*      parentMutex        = nullptr;
};

struct CountingPathSpace : PathSpace {
    using PathSpace::PathSpace;
    void resetCount() { adoptCalls.store(0); }
    int  count() const { return adoptCalls.load(); }
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        adoptCalls.fetch_add(1);
        PathSpace::adoptContextAndPrefix(std::move(context), std::move(prefix));
    }

private:
    std::atomic<int> adoptCalls{0};
};

struct SlowSpace : PathSpace {
    using PathSpace::PathSpace;
    explicit SlowSpace(std::atomic<bool>* destroyedFlag,
                       std::atomic<bool>* listingDoneFlag,
                       std::atomic<bool>* borrowedFlag = nullptr)
        : destroyed(destroyedFlag), listingDone(listingDoneFlag), borrowed(borrowedFlag) {}
    ~SlowSpace() override {
        if (destroyed) {
            destroyed->store(true);
        }
    }
    auto listChildrenCanonical(std::string_view) const -> std::vector<std::string> override {
        if (borrowed) {
            borrowed->store(true);
        }
        std::this_thread::sleep_for(50ms);
        if (listingDone) {
            listingDone->store(true);
        }
        return {"child"};
    }

private:
    std::atomic<bool>* destroyed   = nullptr;
    std::atomic<bool>* listingDone = nullptr;
    std::atomic<bool>* borrowed    = nullptr;
};

struct SlowNestedSpace : PathSpace {
    using PathSpace::PathSpace;
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return PathSpace::in(path, data);
    }
};
} // namespace

TEST_SUITE_BEGIN("pathspace.nesting.basic");

TEST_CASE("PathSpace Nesting/Basic Nested PathSpace Operations") {
    PathSpace outerSpace;
    auto      innerSpace = std::make_unique<PathSpace>();

    // Add some data to inner space
    REQUIRE(innerSpace->insert("/test", 42).nbrValuesInserted == 1);
    REQUIRE(innerSpace->insert("/nested/value", "hello").nbrValuesInserted == 1);

    // Insert inner space into outer space
    REQUIRE(outerSpace.insert("/inner", std::move(innerSpace)).nbrSpacesInserted == 1);

    // Verify we can read values through the nested path
    auto result1 = outerSpace.read<int>("/inner/test", Block{});
    REQUIRE(result1.has_value());
    CHECK(result1.value() == 42);

    auto result2 = outerSpace.read<std::string>("/inner/nested/value", Block{});
    REQUIRE(result2.has_value());
    CHECK(result2.value() == "hello");
}

TEST_CASE("PathSpace Nesting/Take and reinsert nested PathSpace") {
    PathSpace root;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/payload", 123).nbrValuesInserted == 1);
    REQUIRE(root.insert("/watchlists/foo/space", std::move(nested)).nbrSpacesInserted == 1);

    auto extracted = root.take<std::unique_ptr<PathSpace>>("/watchlists/foo/space");
    REQUIRE(extracted.has_value());
    auto owned = std::move(*extracted);
    auto value = owned->read<int>("/payload", Block{});
    REQUIRE(value.has_value());
    CHECK(value.value() == 123);

    REQUIRE(root.insert("/trash/foo/space", std::move(owned)).nbrSpacesInserted == 1);
    auto movedValue = root.read<int>("/trash/foo/space/payload", Block{});
    REQUIRE(movedValue.has_value());
    CHECK(movedValue.value() == 123);
}

TEST_CASE("PathSpace Nesting/Deep Nesting") {
    PathSpace level1;
    auto      level2 = std::make_unique<PathSpace>();
    auto      level3 = std::make_unique<PathSpace>();

    // Add data at each level
    REQUIRE(level3->insert("/data", 100).nbrValuesInserted == 1);
    REQUIRE(level2->insert("/l3", std::move(level3)).nbrSpacesInserted == 1);
    REQUIRE(level1.insert("/l2", std::move(level2)).nbrSpacesInserted == 1);

    // Verify deep access
    auto result = level1.read<int>("/l2/l3/data", Block{});
    REQUIRE(result.has_value());
    CHECK(result.value() == 100);
}

TEST_CASE("PathSpace Nesting/Multiple Nested Spaces") {
    PathSpace root;
    auto      space1 = std::make_unique<PathSpace>();
    auto      space2 = std::make_unique<PathSpace>();

    // Add data to each space
    REQUIRE(space1->insert("/data", 1).nbrValuesInserted == 1);
    REQUIRE(space2->insert("/data", 2).nbrValuesInserted == 1);

    // Insert both spaces
    REQUIRE(root.insert("/space1", std::move(space1)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/space2", std::move(space2)).nbrSpacesInserted == 1);

    // Verify access to both spaces
    auto result1 = root.read<int>("/space1/data", Block{});
    REQUIRE(result1.has_value());
    CHECK(result1.value() == 1);

    auto result2 = root.read<int>("/space2/data", Block{});
    REQUIRE(result2.has_value());
    CHECK(result2.value() == 2);
}

TEST_CASE("PathSpace Nesting/listChildren isolates indexed nested space") {
    PathSpace root;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/nestedValue", 7).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/hostValue", 9).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    auto combined = root.listChildren("/mount");
    CHECK(std::find(combined.begin(), combined.end(), "hostValue") != combined.end());
    CHECK(std::find(combined.begin(), combined.end(), "nestedValue") != combined.end());

    auto indexed = root.listChildren("/mount[0]");
    INFO("indexed: " << [&]() {
             std::string s;
             for (auto const& c : indexed) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());
    CHECK(std::find(indexed.begin(), indexed.end(), "nestedValue") != indexed.end());
    CHECK(std::find(indexed.begin(), indexed.end(), "hostValue") == indexed.end());
}

TEST_CASE("PathSpace Nesting/Nested Space with Functions") {
    PathSpace root;
    auto      subspace = std::make_unique<PathSpace>();

    // Add a function to the subspace
    auto func = []() -> int { return 42; };
    REQUIRE(subspace->insert("/func", func, In{.executionCategory = ExecutionCategory::Lazy}).nbrTasksInserted == 1);

    // Insert subspace
    REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

    // Execute function through nested path
    auto result = root.read<int>("/sub/func", Block{});
    CHECK_MESSAGE(result.has_value(),
                  "nested func read error code=" << static_cast<int>(result.error().code)
                                                 << " msg=" << result.error().message.value_or(""));
    if (result.has_value()) {
        CHECK(result.value() == 42);
    }

}

TEST_CASE("PathSpace Nesting/Multiple nested spaces at same path with indexing") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(first->insert("/v", 1).nbrValuesInserted == 1);
    REQUIRE(second->insert("/v", 2).nbrValuesInserted == 1);

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto mountChildren = root.listChildren("/mount");
    std::string childrenStr;
    for (auto const& c : mountChildren) {
        if (!childrenStr.empty()) childrenStr.append(",");
        childrenStr.append(c);
    }
    INFO("mount children: " << childrenStr);
    CHECK(std::find(mountChildren.begin(), mountChildren.end(), "space") != mountChildren.end());

    auto frontRead = root.read<int>("/mount/space/v", Block{200ms});
    REQUIRE(frontRead.has_value());
    CHECK(frontRead.value() == 1);

    auto secondRead = root.read<int>("/mount/space[1]/v", Block{200ms});
    CHECK_MESSAGE(secondRead.has_value(),
                  "secondRead error code=" << static_cast<int>(secondRead.error().code)
                                           << " msg=" << secondRead.error().message.value_or(""));
    if (secondRead.has_value()) {
        CHECK(secondRead.value() == 2);
    }

    std::vector<std::string> visited;
    VisitOptions opts;
    opts.includeNestedSpaces = true;
    root.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.emplace_back(entry.path);
        return VisitControl::Continue;
    }, opts);
    std::string visitedStr;
    for (auto const& p : visited) {
        if (!visitedStr.empty()) visitedStr.append(",");
        visitedStr.append(p);
    }
    INFO("visited paths size=" << visited.size() << " paths=" << visitedStr);
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space/v") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space[1]/v") != visited.end());

    auto takenFront = root.take<std::unique_ptr<PathSpace>>("/mount/space", Block{200ms});
    REQUIRE(takenFront.has_value());

    auto remainingFront = root.read<int>("/mount/space/v", Block{200ms});
    REQUIRE(remainingFront.has_value());
    CHECK(remainingFront.value() == 2);

    auto missing = root.read<int>("/mount/space[1]/v", Block{200ms});
    CHECK(!missing.has_value());
}

TEST_CASE("PathSpace Nesting/listChildren releases parent lock before nested traversal") {
    IntrospectablePathSpace root;
    auto      nested   = std::make_unique<LockAwareSpace>();
    auto*     nestedRaw = nested.get();

    REQUIRE(root.insert("/mount/space", std::move(nested)).nbrSpacesInserted == 1);

    Node* mountNode = root.rootNodePtr()->getChild("mount");
    REQUIRE(mountNode != nullptr);
    Node* spaceNode = mountNode->getChild("space");
    REQUIRE(spaceNode != nullptr);
    nestedRaw->setParentMutex(&spaceNode->payloadMutex);

    auto children = root.listChildren("/mount/space");
    CHECK(std::find(children.begin(), children.end(), "child") != children.end());
    CHECK(nestedRaw->lastTryLockSucceeded());
}

TEST_SUITE_END();
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

    auto children = root.listChildren("/");
    CHECK(children.empty());
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

    auto merged = root.listChildren("/mount/space");
    INFO("merged children: " << [&]() {
             std::string s;
             for (auto const& c : merged) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());

    auto children = root.listChildren("/mount/space[1]");
    std::sort(children.begin(), children.end());
    INFO("children: " << [&]() {
             std::string s;
             for (auto const& c : children) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());
    CHECK(std::find(children.begin(), children.end(), "b") != children.end());
    CHECK(std::find(children.begin(), children.end(), "a") == children.end());

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

    auto children = clone.listChildren("/mount/space[1]");
    CHECK(std::find(children.begin(), children.end(), "b") != children.end());
    CHECK(std::find(children.begin(), children.end(), "c") != children.end());
    CHECK(std::find(children.begin(), children.end(), "a") == children.end());

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

    auto children = root.listChildren("/mount/space[1]");
    CHECK(children.empty());
}

TEST_CASE("PathSpace Nesting/listChildren holds nested alive during concurrent take") {
    std::atomic<bool> listingDone{false};
    std::atomic<bool> borrowed{false};
    {
        PathSpace root;
        auto slow = std::make_unique<SlowSpace>(nullptr, &listingDone, &borrowed);
        REQUIRE(root.insert("/mount/slow", std::move(slow)).nbrSpacesInserted == 1);

        std::thread lister([&]() {
            auto children = root.listChildren("/mount/slow");
            CHECK(std::find(children.begin(), children.end(), "child") != children.end());
        });

        for (int i = 0; i < 10 && !borrowed.load(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        CHECK(borrowed.load()); // ensure listChildren obtained borrow before take
        auto start = std::chrono::steady_clock::now();
        auto taken = root.take<std::unique_ptr<PathSpace>>("/mount/slow", Block{});
        REQUIRE(taken.has_value());
        REQUIRE(taken.value() != nullptr);
        auto owned = std::move(taken.value());
        owned.reset(); // destroy nested space after borrow holders release

        lister.join();
        CHECK(listingDone.load()); // ensure listChildren finished
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        CHECK(elapsed.count() >= 40); // take should have waited for listChildren borrow to release
    }
}

TEST_CASE("PathSpace Nesting/visit holds nested alive during concurrent take") {
    std::atomic<bool> visitStarted{false};
    std::atomic<bool> visitFinished{false};

    struct SlowVisitSpace : PathSpace {
        using PathSpace::PathSpace;
        SlowVisitSpace(std::atomic<bool>* started, std::atomic<bool>* finished)
            : started(started), finished(finished) {}
        auto visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> override {
            if (started) {
                started->store(true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            auto res = PathSpace::visit(visitor, options);
            if (finished) {
                finished->store(true);
            }
            return res;
        }
        std::atomic<bool>* started;
        std::atomic<bool>* finished;
    };

    PathSpace root;
    auto      slow = std::make_unique<SlowVisitSpace>(&visitStarted, &visitFinished);
    REQUIRE(root.insert("/mount/slow", std::move(slow)).nbrSpacesInserted == 1);

    std::thread visitor([&]() {
        VisitOptions opts;
        opts.root                = "/";
        opts.includeNestedSpaces = true;
        auto result = root.visit(
            [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; }, opts);
        CHECK(result.has_value());
    });

    for (int i = 0; i < 10 && !visitStarted.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    auto start = std::chrono::steady_clock::now();
    auto taken = root.take<std::unique_ptr<PathSpace>>("/mount/slow", Block{});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    REQUIRE(taken.has_value());
    auto owned = std::move(*taken);
    owned.reset();

    visitor.join();
    CHECK(visitFinished.load());
    CHECK(elapsed.count() >= 40);
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
TEST_SUITE_BEGIN("pathspace.nesting.lifecycle");

TEST_CASE("PathSpace Nesting/shutdown clears tree even when context is shared by nested space") {
    IntrospectablePathSpace root;
    REQUIRE(root.insert("/v", 1).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    // Create a second shared reference to the context via another nested insert
    REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    root.shutdown();
    auto children = root.listChildren("/");
    CHECK(children.empty());
}

TEST_CASE("PathSpace Nesting/listChildren merges multiple nested spaces") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
    REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto children = root.listChildren("/mount/space");
    std::sort(children.begin(), children.end());
    INFO("children: " << [&]() {
             std::string s;
             for (auto const& c : children) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());
    CHECK(std::find(children.begin(), children.end(), "a") != children.end());
    CHECK(std::find(children.begin(), children.end(), "b[1]") != children.end());
}

TEST_CASE("PathSpace Nesting/visit includes parent node when starting at nested path") {
    PathSpace root;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child", 7).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(nested)).nbrSpacesInserted == 1);

    std::vector<std::string> visited;
    VisitOptions opts;
    opts.root                = "/mount/space";
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
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space/child") != visited.end());
}

TEST_CASE("PathSpace Nesting/adoptContextAndPrefix remounts nested spaces with full path") {
    IntrospectablePathSpace root;
    auto level1 = std::make_unique<IntrospectablePathSpace>();
    auto level2 = std::make_unique<IntrospectablePathSpace>();
    auto level2Raw = level2.get();

    REQUIRE(level2->insert("/deep/value", 9).nbrValuesInserted == 1);
    REQUIRE(level1->insert("/nested", std::move(level2)).nbrSpacesInserted == 1);
    CHECK(level2Raw->prefixStr() == "/nested");
    REQUIRE(root.insert("/mount/space", std::move(level1)).nbrSpacesInserted == 1);

    INFO("prefix=" << level2Raw->prefixStr());
    CHECK(level2Raw->prefixStr() == "/mount/space/nested");
}

TEST_CASE("PathSpace Nesting/nested insert does not retarget unchanged nested spaces") {
    PathSpace root;
    auto first = std::make_unique<CountingPathSpace>();
    auto second = std::make_unique<CountingPathSpace>();
    auto* firstRaw  = first.get();
    auto* secondRaw = second.get();

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    firstRaw->resetCount();

    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    CHECK(firstRaw->count() == 0);
    CHECK(secondRaw->count() >= 1);
}

TEST_CASE("PathSpace Nesting/destructor shuts down shared context even with nested spaces") {
    auto ctx = std::make_shared<PathSpaceContext>();
    {
        PathSpace root{ctx};
        REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
        CHECK_FALSE(ctx->isShuttingDown());
    }
    CHECK(ctx->isShuttingDown());
}

TEST_CASE("PathSpace Nesting/clone preserves nested/value queue ordering at same node") {
    PathSpace root;
    REQUIRE(root.insert("/node", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/node", 5).nbrValuesInserted == 1);

    PathSpace clone = root.clone();

    auto taken = clone.take<std::unique_ptr<PathSpace>>("/node", Block{});
    REQUIRE(taken.has_value());

    auto value = clone.take<int>("/node", Block{});
    CHECK_MESSAGE(value.has_value(),
                  "value take failed code=" << static_cast<int>(value.error().code));
    if (value.has_value()) {
        CHECK(value.value() == 5);
    }
}

TEST_CASE("PathSpace Nesting/remaining nested space retargets prefix after removal") {
    IntrospectablePathSpace root;
    auto first  = std::make_unique<IntrospectablePathSpace>();
    auto second = std::make_unique<IntrospectablePathSpace>();

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto taken = root.take<std::unique_ptr<PathSpace>>("/mount/space", Block{});
    REQUIRE(taken.has_value());

    auto ctx     = root.contextPtr();
    auto guard   = ctx->wait("/mount/space/value");
    auto deadline = std::chrono::system_clock::now() + 200ms;
    bool insertOk = false;

    std::thread writer([&]() {
        std::this_thread::sleep_for(10ms);
        auto insertResult = root.insert("/mount/space/value", 99);
        insertOk = insertResult.errors.empty();
    });

    auto status = guard.wait_until(deadline);
    writer.join();

    REQUIRE(insertOk);
    CHECK(status == std::cv_status::no_timeout);

    auto value = root.read<int>("/mount/space/value", Block{});
    REQUIRE(value.has_value());
    CHECK(value.value() == 99);
}

TEST_CASE("PathSpace Nesting/json export keeps values after nested placeholder") {
    PathSpace root;
    REQUIRE(root.insert("/node", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/node", 5).nbrValuesInserted == 1);

    PathSpaceJsonExporter exporter;
    PathSpaceJsonOptions  opts;
    opts.flatPaths = true;
    auto exported = exporter.Export(root, opts);
    REQUIRE(exported);
    INFO(*exported);

    auto json = nlohmann::json::parse(*exported);
    REQUIRE(json.contains("/node"));
    auto values = json["/node"];
    bool found  = false;
    auto visitValue = [&](nlohmann::json const& v) {
        if (v.is_object() && v.contains("value") && v["value"].is_number_integer() && v["value"] == 5) {
            found = true;
        } else if (v.is_number_integer() && v == 5) {
            found = true;
        }
    };
    if (values.is_array()) {
        for (auto const& v : values) {
            visitValue(v);
        }
    } else {
        visitValue(values);
    }
    CHECK(found);
}

TEST_CASE("PathSpace Nesting/escaped bracket path is treated literally") {
    PathSpace root;
    auto insert = root.insert("/node\\[1\\]", 5);
    CHECK(insert.errors.empty());

    auto value = root.read<int>("/node\\[1\\]", Block{});
    CHECK_MESSAGE(value.has_value(),
                  "escaped bracket read failed code=" << static_cast<int>(value.error().code));
    if (value.has_value()) {
        CHECK(value.value() == 5);
    }
}

TEST_CASE("PathSpace Nesting/clear without context is safe") {
    PathSpace rootWithContext;
    REQUIRE(rootWithContext.insert("/v", 1).nbrValuesInserted == 1);

    PathSpace nullContextSpace{std::shared_ptr<PathSpaceContext>{}, "/pref"};
    CHECK_NOTHROW(nullContextSpace.clear());
}

TEST_CASE("PathSpace Nesting/context constructor retains executor/pool") {
    TaskPool customPool{1};
    auto     ctx = std::make_shared<PathSpaceContext>(&customPool);
    PathSpace space{ctx, "/pref"};

    CHECK(PathSpaceTestHelper::pool(space) == &customPool);
    CHECK(PathSpaceTestHelper::executor(space) == &customPool);

    PathSpace clone = space.clone();
    CHECK(PathSpaceTestHelper::pool(clone) == &customPool);
    CHECK(PathSpaceTestHelper::executor(clone) == &customPool);
}

TEST_CASE("PathSpace Nesting/copyNodeRecursive clones nested ordering with placeholders") {
    PathSpace source;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/v", 7).nbrValuesInserted == 1);
    REQUIRE(source.insert("/node", std::move(nested)).nbrSpacesInserted == 1);
    REQUIRE(source.insert("/node", 5).nbrValuesInserted == 1);

    PathSpace dest{source.sharedContext(), PathSpaceTestHelper::prefix(source)};
    PathSpace::CopyStats stats{};
    PathSpaceTestHelper::copyNode(source, dest, source.sharedContext(), PathSpaceTestHelper::prefix(source), "/", stats);

    auto value = dest.read<int>("/node", Block{});
    CHECK(value.has_value());
    CHECK(value.value() == 5);

    auto nestedVal = dest.read<int>("/node/v", Block{});
    CHECK(nestedVal.has_value());
    CHECK(nestedVal.value() == 7);
    CHECK(stats.nestedSpacesCopied == 1);
}

TEST_SUITE_END();
TEST_SUITE_BEGIN("pathspace.nesting.concurrent");

TEST_CASE("PathSpace Nesting/Nested Space with Blocking Operations") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Insert subspace first
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             // Start a thread that will write to the nested space after a delay
             std::thread writer([&root]() {
                 std::this_thread::sleep_for(100ms);
                 auto result = root.insert("/sub/delayed", 42);
                 REQUIRE(result.nbrValuesInserted == 1);
             });

             // Try to read with blocking
             auto result = root.read<int>("/sub/delayed", Block{200ms});
             writer.join();

             REQUIRE(result.has_value());
             CHECK(result.value() == 42);
}

TEST_CASE("PathSpace Nesting/Nested Space Extraction") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Add data to subspace
             REQUIRE(subspace->insert("/data", 42).nbrValuesInserted == 1);
             REQUIRE(subspace->insert("/data", 43).nbrValuesInserted == 1);

             // Insert subspace
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             // Test extraction through nested path
             auto result1 = root.take<int>("/sub/data", Block{});
             REQUIRE(result1.has_value());
             CHECK(result1.value() == 42);

             auto result2 = root.take<int>("/sub/data", Block{});
             REQUIRE(result2.has_value());
             CHECK(result2.value() == 43);

             // Verify no more data
             auto result3 = root.read<int>("/sub/data");
             CHECK(!result3.has_value());
}

TEST_CASE("PathSpace Nesting/Concurrent Access to Nested Space") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Insert subspace
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             constexpr int NUM_THREADS    = 10;
             constexpr int OPS_PER_THREAD = 100;

             std::atomic<int> insertCount{0};
             std::atomic<int> readCount{0};

             // Create writer threads
             std::vector<std::thread> writers;
             for (int i = 0; i < NUM_THREADS; ++i) {
                 writers.emplace_back([&root, i, &insertCount]() {
                     for (int j = 0; j < OPS_PER_THREAD; ++j) {
                         auto result = root.insert("/sub/data", i * OPS_PER_THREAD + j);
                         if (result.nbrValuesInserted == 1) {
                             insertCount++;
                         }
                     }
                 });
             }

             // Create reader threads
             std::vector<std::thread> readers;
             for (int i = 0; i < NUM_THREADS; ++i) {
                 readers.emplace_back([&root, &readCount]() {
                     for (int j = 0; j < OPS_PER_THREAD; ++j) {
                         auto result = root.read<int>("/sub/data", Block{10ms});
                         if (result.has_value()) {
                             readCount++;
                         }
                     }
                 });
             }

             // Join all threads
             for (auto& w : writers)
                 w.join();
             for (auto& r : readers)
                 r.join();

             // Verify operations
             CHECK(insertCount > 0);
             CHECK(readCount > 0);
             CHECK(insertCount + readCount > 0);
}

TEST_CASE("PathSpace Nesting/Nested Space Clear Operations") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Add data to subspace
             REQUIRE(subspace->insert("/data1", 42).nbrValuesInserted == 1);
             REQUIRE(subspace->insert("/data2", "test").nbrValuesInserted == 1);

             // Insert subspace
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             // Clear root space
             root.clear();

             // Verify all data is cleared
             auto result1 = root.read<int>("/sub/data1");
             CHECK(!result1.has_value());

             auto result2 = root.read<std::string>("/sub/data2");
             CHECK(!result2.has_value());
}

TEST_CASE("PathSpace Nesting/Invalid Nested Space Operations") {
             PathSpace root;

             // Try to insert nullptr
             std::unique_ptr<PathSpace> nullspace = nullptr;
             auto                       result    = root.insert("/null", std::move(nullspace));
             CHECK(result.errors.size() > 0);

             // Try to insert into non-existent nested space
             auto result2 = root.insert("/nonexistent/data", 42);
             CHECK(result2.errors.empty());
             CHECK(result2.nbrValuesInserted == 1);
}

TEST_CASE("PathSpace Nesting/Nested Space Path Validation") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Test invalid paths
             auto result1 = root.insert("invalid", std::move(subspace));
             CHECK(result1.errors.size() > 0);

             auto subspace2 = std::make_unique<PathSpace>();
             auto result2   = root.insert("/sub//invalid", std::move(subspace2));
             CHECK(result2.errors.size() > 0);
}

TEST_CASE("PathSpace Nesting/take blocks while nested operation in-flight") {
             using namespace std::chrono_literals;
             PathSpace root;
             auto nested = std::make_unique<SlowNestedSpace>();
             REQUIRE(root.insert("/ns", std::move(nested)).nbrSpacesInserted == 1);

             std::atomic<bool> insertDone{false};

             std::thread inserter([&]() {
                 auto ret = root.insert("/ns/value", 7);
                 REQUIRE(ret.errors.empty());
                 insertDone.store(true);
             });

             std::this_thread::sleep_for(5ms);
             auto start = std::chrono::steady_clock::now();
             auto taken = root.take<std::unique_ptr<PathSpace>>("/ns", Block{200ms});
             auto end   = std::chrono::steady_clock::now();

             inserter.join();
             REQUIRE(taken.has_value());
             CHECK(insertDone.load());
             auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
             CHECK(elapsed.count() >= 30);
}

TEST_SUITE_END();

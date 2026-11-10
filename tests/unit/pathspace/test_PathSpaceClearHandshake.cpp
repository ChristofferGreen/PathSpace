#include "PathSpace.hpp"
#include "PathSpaceTestHelper.hpp"
#include "third_party/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <span>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

namespace {

// Holds the first operation that passes the clear() gate until the test releases it.
struct AdmissionGate {
    std::promise<void>       admitted;
    std::promise<void>       release;
    std::shared_future<void> released{release.get_future().share()};
    std::atomic<bool>        used{false};
};

std::atomic<AdmissionGate*> activeGate{nullptr};

void holdAdmittedOperation() {
    auto* gate = activeGate.load(std::memory_order_acquire);
    if (!gate || gate->used.exchange(true)) {
        return;
    }
    gate->admitted.set_value();
    gate->released.wait();
}

struct AdmissionHookScope {
    explicit AdmissionHookScope(AdmissionGate& gate) {
        activeGate.store(&gate, std::memory_order_release);
        testing::SetPathSpaceAdmissionHook(&holdAdmittedOperation);
    }
    ~AdmissionHookScope() {
        testing::SetPathSpaceAdmissionHook(nullptr);
        activeGate.store(nullptr, std::memory_order_release);
    }
};

} // namespace

TEST_SUITE("pathspace.clear.handshake") {

TEST_CASE("clear waits for a read that was admitted before clear started") {
    PathSpace space;
    REQUIRE(space.insert("/value", 41).errors.empty());

    AdmissionGate      gate;
    AdmissionHookScope hookScope{gate};

    Expected<int> readResult = std::unexpected(Error{Error::Code::UnknownError, "not run"});
    std::thread   reader([&] { readResult = space.read<int>("/value"); });
    REQUIRE(gate.admitted.get_future().wait_for(5s) == std::future_status::ready);

    // The reader saw "not clearing" and is about to walk the tree. It has to be counted already,
    // otherwise a clear() starting now finds no reader and frees the nodes under it.
    CHECK(PathSpaceTestHelper::activeOut(space).load() == 1);

    std::promise<void> cleared;
    auto               clearDone = cleared.get_future();
    std::thread        clearer([&] {
        space.clear();
        cleared.set_value();
    });
    CHECK(clearDone.wait_for(60ms) == std::future_status::timeout);

    gate.release.set_value();
    reader.join();
    clearer.join();

    // The read was admitted first, so it observes the value; clear runs after it.
    REQUIRE(readResult.has_value());
    CHECK(*readResult == 41);
    CHECK_FALSE(space.read<int>("/value").has_value());
}

TEST_CASE("clear waits for an insert that was admitted before clear started") {
    PathSpace space;
    REQUIRE(space.insert("/seed", 1).errors.empty());

    AdmissionGate      gate;
    AdmissionHookScope hookScope{gate};

    InsertReturn insertResult;
    std::thread  writer([&] { insertResult = space.insert("/seed/child", 2); });
    REQUIRE(gate.admitted.get_future().wait_for(5s) == std::future_status::ready);

    std::promise<void> cleared;
    auto               clearDone = cleared.get_future();
    std::thread        clearer([&] {
        space.clear();
        cleared.set_value();
    });
    // An insert walks and creates nodes; clear() must not free the tree underneath it.
    CHECK(clearDone.wait_for(60ms) == std::future_status::timeout);

    gate.release.set_value();
    writer.join();
    clearer.join();

    CHECK(insertResult.errors.empty());
    CHECK_FALSE(space.read<int>("/seed/child").has_value());
}

TEST_CASE("an insert that arrives during clear lands in the cleared space") {
    PathSpace space;
    REQUIRE(space.insert("/old", 1).errors.empty());

    // Hold a reader in flight so clear() is parked between "clearing" and wiping the tree.
    AdmissionGate gate;
    std::thread   reader;
    std::thread   clearer;
    std::thread   writer;
    InsertReturn  insertResult;
    {
        AdmissionHookScope hookScope{gate};
        reader = std::thread([&] { (void)space.read<int>("/old"); });
        REQUIRE(gate.admitted.get_future().wait_for(5s) == std::future_status::ready);
    }
    clearer = std::thread([&] { space.clear(); });
    // Wait until clear() announced itself; it cannot finish while the reader is held.
    auto const announceDeadline = std::chrono::steady_clock::now() + 5s;
    while (!PathSpaceTestHelper::clearing(space).load() && std::chrono::steady_clock::now() < announceDeadline) {
        std::this_thread::yield();
    }
    CHECK(PathSpaceTestHelper::clearing(space).load());
    std::promise<void> inserted;
    auto               insertDone = inserted.get_future();
    writer = std::thread([&] {
        insertResult = space.insert("/new", 7);
        inserted.set_value();
    });
    // The insert must not run against the tree that is about to be wiped: it would be reported
    // as successful and then silently erased.
    CHECK(insertDone.wait_for(60ms) == std::future_status::timeout);

    gate.release.set_value();
    reader.join();
    clearer.join();
    writer.join();

    CHECK(insertResult.errors.empty());
    auto value = space.read<int>("/new");
    REQUIRE(value.has_value());
    CHECK(*value == 7);
    CHECK_FALSE(space.read<int>("/old").has_value());
}

TEST_CASE("an insert made from inside a read callback does not deadlock with a pending clear") {
    // The space and the shared state are kept alive by the threads in case the reader never returns.
    auto  spacePtr = std::make_shared<PathSpace>();
    auto& space    = *spacePtr;
    REQUIRE(space.insert("/ints", 1).errors.empty());

    struct Shared {
        std::promise<void>       inCallback;
        std::promise<void>       proceed;
        std::shared_future<void> proceeding{proceed.get_future().share()};
        std::promise<bool>       readerDone;
    };
    auto shared = std::make_shared<Shared>();

    std::thread reader([spacePtr, shared] {
        auto& space = *spacePtr;
        bool insertOk = false;
        auto result   = space.read("/ints", [&](std::span<const int>) {
            shared->inCallback.set_value();
            shared->proceeding.wait();
            // The read is in flight on this thread, so the tree cannot be wiped under this insert;
            // it must not wait for the clear that is waiting for the read.
            insertOk = space.insert("/fromCallback", 2).errors.empty();
        });
        shared->readerDone.set_value(result.has_value() && insertOk);
    });
    REQUIRE(shared->inCallback.get_future().wait_for(5s) == std::future_status::ready);

    std::thread clearer([spacePtr] { spacePtr->clear(); });
    auto const announceDeadline = std::chrono::steady_clock::now() + 5s;
    while (!PathSpaceTestHelper::clearing(space).load() && std::chrono::steady_clock::now() < announceDeadline) {
        std::this_thread::yield();
    }
    REQUIRE(PathSpaceTestHelper::clearing(space).load());

    shared->proceed.set_value();
    auto       readerDone = shared->readerDone.get_future();
    bool const returned   = readerDone.wait_for(5s) == std::future_status::ready;
    CHECK(returned);
    if (!returned) {
        // Deadlocked: the threads cannot be joined. Leave them behind so the suite can report.
        reader.detach();
        clearer.detach();
        return;
    }
    CHECK(readerDone.get());
    reader.join();
    clearer.join();
    CHECK_FALSE(space.read<int>("/fromCallback").has_value());
}

}

#include "task/IFutureAny.hpp"
#include "third_party/doctest.h"

#include <chrono>
#include <thread>

using namespace SP;

TEST_SUITE("task.future.any") {
TEST_CASE("Promise/FutureAny round trip") {
    PromiseT<int> promise;
    auto fut = promise.get_future();
    auto any = fut.to_any();

    CHECK(any.valid());
    CHECK_FALSE(any.ready());
    int out = -1;
    CHECK_FALSE(any.try_copy_to(&out));

    // Fulfill asynchronously.
    std::thread setter([&] { promise.set_value(9); });
    setter.join();

    CHECK(any.ready());
    CHECK(any.type() == typeid(int));
    CHECK(any.copy_to(&out));
    CHECK(out == 9);
}

TEST_CASE("FutureAny wait_until and invalid states") {
    FutureAny invalid;
    CHECK(invalid.valid() == false);
    CHECK(invalid.wait_until(std::chrono::steady_clock::now())); // returns true for invalid

    PromiseT<int> promise;
    auto fut = promise.get_future();
    auto any = fut.to_any();

    // wait_for should time out while not ready.
    auto before = std::chrono::steady_clock::now();
    CHECK_FALSE(any.wait_for(std::chrono::milliseconds{1}));
    promise.set_value(3);
    int value = 0;
    CHECK(any.try_copy_to(&value));
    CHECK(value == 3);
}

TEST_CASE("SharedState set_value is single-shot and copy_to validates destination") {
    auto state = std::make_shared<SharedState<int>>();
    CHECK_FALSE(state->ready());

    int pre = 0;
    CHECK_FALSE(state->copy_to(&pre));
    CHECK_FALSE(state->get(pre));

    CHECK(state->set_value(42));
    CHECK_FALSE(state->set_value(7));

    int out = 0;
    CHECK(state->copy_to(&out));
    CHECK(out == 42);

    CHECK_FALSE(state->copy_to(nullptr));
}

TEST_CASE("FutureAny constructor from typed future shares state") {
    PromiseT<int> promise;
    auto fut = promise.get_future();

    FutureAny any{fut};
    CHECK(any.valid());
    CHECK(any.type() == typeid(int));

    promise.set_value(55);
    int out = 0;
    CHECK(any.copy_to(&out));
    CHECK(out == 55);

    CHECK(any.shared_state() == fut.shared_state());
}

TEST_CASE("FutureT get/try_get fail on invalid futures") {
    FutureT<int> invalid;
    int out = 0;
    CHECK_FALSE(invalid.try_get(out));
    CHECK_FALSE(invalid.get(out));
}

TEST_CASE("FutureT and FutureAny handle invalid and ready transitions") {
    FutureT<int> invalidTyped;
    CHECK_FALSE(invalidTyped.valid());
    CHECK(invalidTyped.wait_until(std::chrono::steady_clock::now())); // invalid returns true

    FutureAny invalidAny;
    CHECK_FALSE(invalidAny.valid());
    CHECK(invalidAny.type() == typeid(void));
    CHECK_FALSE(invalidAny.try_copy_to(nullptr));
    CHECK_FALSE(invalidAny.copy_to(nullptr));

    PromiseT<int> promise;
    auto fut = promise.get_future();
    int out = 0;
    CHECK_FALSE(fut.try_get(out));

    std::thread setter([&] { promise.set_value(11); });
    setter.join();

    CHECK(fut.ready());
    CHECK(fut.get(out));
    CHECK(out == 11);

    auto any = fut.to_any();
    CHECK(any.ready());
    CHECK(any.type() == typeid(int));
    CHECK_FALSE(any.try_copy_to(nullptr));
    out = 0;
    CHECK(any.copy_to(&out));
    CHECK(out == 11);
}

TEST_CASE("SharedState wait and wait_until block until ready") {
    auto state = std::make_shared<SharedState<int>>();
    CHECK_FALSE(state->ready());

    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        state->set_value(21);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
    CHECK(state->wait_until(deadline));
    int out = 0;
    CHECK(state->get(out));
    CHECK(out == 21);

    setter.join();

    state->wait();
    CHECK(state->ready());
}

TEST_CASE("FutureAny wait_for and FutureT wait_for reflect readiness") {
    PromiseT<int> promise;
    auto future = promise.get_future();
    auto any = future.to_any();

    CHECK_FALSE(any.wait_for(std::chrono::milliseconds{1}));
    CHECK_FALSE(future.wait_for(std::chrono::milliseconds{1}));

    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        promise.set_value(8);
    });

    CHECK(any.wait_for(std::chrono::milliseconds{50}));
    CHECK(future.wait_for(std::chrono::milliseconds{50}));

    int out = 0;
    CHECK(future.get(out));
    CHECK(out == 8);

    setter.join();
}

TEST_CASE("PromiseT constructed from shared state preserves identity") {
    auto shared = std::make_shared<SharedState<int>>();
    PromiseT<int> promise(shared);
    auto fut = promise.get_future();

    CHECK(fut.shared_state() == shared);
    CHECK(promise.shared_state() == shared);

    CHECK(promise.set_value(17));
    int out = 0;
    CHECK(fut.get(out));
    CHECK(out == 17);
}
}

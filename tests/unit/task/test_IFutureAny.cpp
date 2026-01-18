#include "task/IFutureAny.hpp"
#include "third_party/doctest.h"

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
}

#include "type/InputData.hpp"
#include "core/PodPayload.hpp"
#include "third_party/doctest.h"

#include <functional>
#include <memory>
#include <string>

using namespace SP;

TEST_SUITE_BEGIN("type.inputdata");

TEST_CASE("Pod preferred types wire POD factory and point to object") {
    int value = 5;
    InputData data{value};

    CHECK(data.obj == &value);
    CHECK(data.metadata.podPreferred);
    REQUIRE(data.metadata.createPodPayload != nullptr);

    auto payload = data.metadata.createPodPayload();
    REQUIRE(payload != nullptr);
    CHECK(payload->matches(typeid(int)));
}

TEST_CASE("Function pointers are stored as callable addresses without POD fast path") {
    static int callCount = 0;
    auto fnImpl          = []() -> int {
        ++callCount;
        return 3;
    };
    using Fn      = int (*)();
    Fn fn         = fnImpl;

    InputData data{fn};
    auto recovered = reinterpret_cast<Fn>(data.obj);
    CHECK(recovered() == 3);
    CHECK(callCount == 1);
    CHECK_FALSE(data.metadata.podPreferred);
    CHECK(data.metadata.createPodPayload == nullptr);
}

TEST_CASE("Unique pointer inputs leave POD factory unset") {
    std::unique_ptr<int> ptr = std::make_unique<int>(7);
    InputData            data{ptr};

    CHECK(data.obj == &ptr);
    CHECK_FALSE(data.metadata.podPreferred);
    CHECK(data.metadata.createPodPayload == nullptr);
}

TEST_CASE("std::function inputs keep object address without POD fast path") {
    std::function<void()> fn = []() {};
    InputData               data{fn};

    auto* stored = static_cast<std::function<void()>*>(data.obj);
    REQUIRE(stored != nullptr);
    (*stored)();
    CHECK_FALSE(data.metadata.podPreferred);
    CHECK(data.metadata.createPodPayload == nullptr);
}

TEST_CASE("InputData tracks executor and replaceExistingPayload flag") {
    int         value = 9;
    InputData   data{value};
    SP::Executor* execPtr = reinterpret_cast<SP::Executor*>(0x1234);

    CHECK_FALSE(data.replaceExistingPayload);
    data.replaceExistingPayload = true;
    data.executor               = execPtr;

    CHECK(data.replaceExistingPayload);
    CHECK(data.executor == execPtr);
}

TEST_CASE("String literal inputs avoid POD factory and preserve pointer") {
    static char const literal[] = "hello";
    InputData          data{literal};

    CHECK(data.obj == literal);
    CHECK_FALSE(data.metadata.podPreferred);
    CHECK(data.metadata.createPodPayload == nullptr);
}

TEST_SUITE_END();

#include "third_party/doctest.h"
#include <pathspace/type/InputData.hpp>
#include <pathspace/task/Task.hpp>

#include <functional>
#include <memory>
#include <string>
#include <type_traits>

using namespace SP;

TEST_SUITE_BEGIN("type.inputdata");

TEST_CASE("InputData attaches pod payload factory for podPreferred types") {
    int value = 7;
    InputData data(value);

    CHECK(data.metadata.podPreferred);
    REQUIRE(data.metadata.createPodPayload != nullptr);

    auto payload = data.metadata.createPodPayload();
    REQUIRE(payload != nullptr);

    auto typed = std::dynamic_pointer_cast<PodPayload<int>>(payload);
    REQUIRE(typed != nullptr);
    CHECK(typed->push(value));

    int out = 0;
    CHECK_FALSE(typed->read(&out));
    CHECK(out == value);
}

TEST_CASE("InputData leaves non-POD types on the generic path") {
    std::string text{"alpha"};
    InputData   data(text);

    CHECK_FALSE(data.metadata.podPreferred);
    CHECK(data.metadata.createPodPayload == nullptr);
    CHECK(data.obj != nullptr);
}

int sampleFunction() { return 5; }

TEST_CASE("InputData captures function pointers for execution metadata") {
    InputData data(&sampleFunction);

    CHECK(data.obj != nullptr);
    CHECK(data.metadata.dataCategory == DataCategory::Execution);
    CHECK(data.metadata.functionCategory == FunctionCategory::FunctionPointer);
    CHECK(data.metadata.serialize == nullptr);
    CHECK(data.metadata.deserialize == nullptr);
}

TEST_CASE("InputData captures executor wiring for callable payloads") {
    InputData data(&sampleFunction);

    CHECK(data.obj != nullptr);
    CHECK(data.metadata.functionCategory == FunctionCategory::FunctionPointer);

    struct StubExecutor : Executor {
        std::weak_ptr<Task> captured;
        auto submit(std::weak_ptr<Task>&& t) -> std::optional<Error> override {
            captured = std::move(t);
            return std::nullopt;
        }
        auto shutdown() -> void override {}
        auto size() const -> size_t override { return 1; }
    } exec;

    data.executor = &exec;
    data.replaceExistingPayload = true;
    CHECK(data.executor == &exec);
    CHECK(data.replaceExistingPayload);
}

TEST_CASE("InputData handles std::function and unique_ptr without POD promotion") {
    std::function<int()> fn = [] { return 42; };
    InputData            funcData(fn);
    CHECK(funcData.metadata.dataCategory == DataCategory::Execution);
    CHECK(funcData.metadata.functionCategory == FunctionCategory::StdFunction);
    CHECK(funcData.metadata.serialize == nullptr);
    CHECK(funcData.metadata.deserialize == nullptr);
    CHECK(funcData.obj != nullptr);

    auto ptr = std::make_unique<int>(3);
    InputData uniqueData(ptr);
    CHECK(uniqueData.metadata.dataCategory == DataCategory::UniquePtr);
    CHECK_FALSE(uniqueData.metadata.podPreferred);
    CHECK(uniqueData.metadata.createPodPayload == nullptr);
    CHECK(uniqueData.obj == &ptr);
}

TEST_CASE("InputData copies metadata hooks when podPreferred requires populate") {
    struct Trivial {
        int x{0};
        auto operator==(Trivial const&) const -> bool = default;
    };

    InputData data(Trivial{});
    CHECK(data.metadata.podPreferred);
    CHECK(data.metadata.createPodPayload != nullptr);

    // createPodPayload should produce a payload matching the metadata type.
    auto payload = data.metadata.createPodPayload();
    REQUIRE(payload != nullptr);
    CHECK(payload->podMetadata().typeInfo == data.metadata.typeInfo);
}

TEST_CASE("InputData captures const and reference categories without aliasing the source") {
    const int original = 5;
    InputData constData(original);
    CHECK(constData.obj == &original);
    CHECK(constData.metadata.podPreferred);

    int copy = 0;
    auto payload = constData.metadata.createPodPayload();
    REQUIRE(payload != nullptr);
    auto typed = std::dynamic_pointer_cast<PodPayload<int>>(payload);
    REQUIRE(typed != nullptr);
    REQUIRE(typed->push(original));
    CHECK_FALSE(typed->read(&copy));
    CHECK(copy == original);
}

TEST_CASE("InputData handles non-copyable unique_ptr references without copying ownership") {
    auto unique = std::make_unique<int>(11);
    auto* raw   = unique.get();
    InputData data(unique);
    CHECK(data.obj == &unique);
    CHECK(data.metadata.dataCategory == DataCategory::UniquePtr);
    CHECK(data.metadata.typeInfo == &typeid(std::unique_ptr<int>));
    CHECK(static_cast<std::unique_ptr<int>*>(data.obj)->get() == raw);
}

TEST_CASE("InputData constructed with explicit metadata preserves provided hooks") {
    InputMetadata meta{InputMetadataT<int>{}};
    meta.createPodPayload = &PodPayload<int>::CreateShared;

    int value = 42;
    InputData data(&value, meta);

    CHECK(data.obj == &value);
    CHECK(data.metadata.typeInfo == meta.typeInfo);
    REQUIRE(data.metadata.createPodPayload != nullptr);
    auto payload = data.metadata.createPodPayload();
    REQUIRE(payload != nullptr);
    CHECK(payload->matches(typeid(int)));
}

TEST_SUITE_END();

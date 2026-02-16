#include "third_party/doctest.h"

#define private public
#include "core/Leaf.hpp"
#undef private

#include "core/InsertReturn.hpp"
#include "core/NodeData.hpp"
#include "path/Iterator.hpp"
#include "PathSpace.hpp"
#include "type/DataCategory.hpp"
#include "type/FunctionCategory.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>

using namespace SP;

namespace {
enum class MigrationFailureMode {
    TakeError,
    SerializeError
};

struct FailingPodPayload final : PodPayloadBase {
    explicit FailingPodPayload(MigrationFailureMode modeIn, int valueIn = 1)
        : mode(modeIn), value(valueIn), meta(InputMetadataT<int>{}) {
        if (mode == MigrationFailureMode::SerializeError) {
            meta.serialize = nullptr;
        }
    }

    std::type_info const& type() const noexcept override { return typeid(int); }
    bool matches(std::type_info const& ti) const noexcept override { return ti == typeid(int); }
    std::size_t size() const noexcept override { return produced ? 0U : 1U; }
    std::size_t elementSize() const noexcept override { return sizeof(int); }
    std::size_t headIndex() const noexcept override { return 0U; }
    std::size_t publishedTail() const noexcept override { return produced ? 1U : 0U; }
    InputMetadata const& podMetadata() const noexcept override { return meta; }
    bool pushValue(void const*) override { return false; }
    std::optional<Error> readTo(void* out) const override {
        if (out) {
            std::memcpy(out, &value, sizeof(int));
        }
        return std::nullopt;
    }
    std::optional<Error> takeTo(void* out) override {
        if (mode == MigrationFailureMode::TakeError) {
            return Error{Error::Code::UnknownError, "PodPayload take failure"};
        }
        if (produced) {
            return Error{Error::Code::NoObjectFound, "No data available"};
        }
        if (out) {
            std::memcpy(out, &value, sizeof(int));
        }
        produced = true;
        return std::nullopt;
    }
    std::optional<Error> withSpanRaw(std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(&value, 1);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRaw(std::function<void(void*, std::size_t)> const& fn) override {
        fn(&value, 1);
        return std::nullopt;
    }
    std::optional<Error> withSpanRawFrom(std::size_t,
                                         std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(&value, 1);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRawFrom(std::size_t,
                                                std::function<void(void*, std::size_t)> const& fn) override {
        fn(&value, 1);
        return std::nullopt;
    }
    std::optional<Error> popCount(std::size_t) override { return std::nullopt; }
    bool freezeForUpgrade() override { return true; }
    std::optional<Reservation> reserveOne() override { return std::nullopt; }
    void publishOne(std::size_t) override {}
    void rollbackOne(std::size_t) override {}
    std::optional<ReservationSpan> reserveSpan(std::size_t) override { return std::nullopt; }
    void publishSpan(std::size_t, std::size_t) override {}
    void rollbackSpan(std::size_t, std::size_t) override {}
    std::optional<std::size_t> packSpanStart() const noexcept override { return std::nullopt; }
    void markPackSpanStart(std::size_t) noexcept override {}

    MigrationFailureMode mode;
    int value = 0;
    bool produced = false;
    InputMetadata meta;
};

struct RejectingPodPayload final : PodPayloadBase {
    std::type_info const& type() const noexcept override { return typeid(int); }
    bool matches(std::type_info const& ti) const noexcept override { return ti == typeid(int); }
    std::size_t size() const noexcept override { return 0U; }
    std::size_t elementSize() const noexcept override { return sizeof(int); }
    std::size_t headIndex() const noexcept override { return 0U; }
    std::size_t publishedTail() const noexcept override { return 0U; }
    InputMetadata const& podMetadata() const noexcept override { return meta; }
    bool pushValue(void const*) override {
        pushCalled = true;
        return false;
    }
    std::optional<Error> readTo(void*) const override { return std::nullopt; }
    std::optional<Error> takeTo(void*) override {
        return Error{Error::Code::NoObjectFound, "No data available"};
    }
    std::optional<Error> withSpanRaw(std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRaw(std::function<void(void*, std::size_t)> const& fn) override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> withSpanRawFrom(std::size_t,
                                         std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRawFrom(std::size_t,
                                                std::function<void(void*, std::size_t)> const& fn) override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> popCount(std::size_t) override { return std::nullopt; }
    bool freezeForUpgrade() override { return true; }
    std::optional<Reservation> reserveOne() override { return std::nullopt; }
    void publishOne(std::size_t) override {}
    void rollbackOne(std::size_t) override {}
    std::optional<ReservationSpan> reserveSpan(std::size_t) override { return std::nullopt; }
    void publishSpan(std::size_t, std::size_t) override {}
    void rollbackSpan(std::size_t, std::size_t) override {}
    std::optional<std::size_t> packSpanStart() const noexcept override { return std::nullopt; }
    void markPackSpanStart(std::size_t) noexcept override {}

    bool pushCalled = false;
    InputMetadata meta{InputMetadataT<int>{}};
};

struct RetargetSpace final : PathSpaceBase {
    RetargetSpace() {
        ret.retargets.push_back(InsertReturn::RetargetRequest{this, "/child"});
    }

    auto in(Iterator const&, InputData const&) -> InsertReturn override { return ret; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "out not supported"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}

    InsertReturn ret;
};
} // namespace

TEST_SUITE_BEGIN("core.leaf.coverage");

TEST_CASE("Leaf extractSerialized uses lexicographic glob ordering") {
    Leaf         leaf;
    InsertReturn ret;

    int valueA = 1;
    int valueB = 2;
    InputMetadata meta{InputMetadataT<int>{}};

    NodeData payloadA;
    REQUIRE_FALSE(payloadA.serialize(InputData{&valueA, meta}).has_value());
    NodeData payloadB;
    REQUIRE_FALSE(payloadB.serialize(InputData{&valueB, meta}).has_value());

    leaf.insertSerialized(Iterator{"/root/b"}, payloadB, ret);
    leaf.insertSerialized(Iterator{"/root/a"}, payloadA, ret);
    REQUIRE(ret.errors.empty());

    NodeData extracted;
    auto err = leaf.extractSerialized(Iterator{"/root/*"}, extracted);
    CHECK_FALSE(err.has_value());

    int decoded = 0;
    REQUIRE_FALSE(extracted.deserialize(&decoded, meta).has_value());
    CHECK(decoded == 1);

    NodeData extractedSecond;
    auto errSecond = leaf.extractSerialized(Iterator{"/root/*"}, extractedSecond);
    CHECK_FALSE(errSecond.has_value());
    decoded = 0;
    REQUIRE_FALSE(extractedSecond.deserialize(&decoded, meta).has_value());
    CHECK(decoded == 2);

    NodeData missing;
    auto errMissing = leaf.extractSerialized(Iterator{"/root/*"}, missing);
    REQUIRE(errMissing.has_value());
    CHECK(errMissing->code == Error::Code::NoSuchPath);
}

TEST_CASE("Leaf insertSerialized rejects glob paths") {
    Leaf         leaf;
    InsertReturn ret;

    int          value = 5;
    InputMetadata meta{InputMetadataT<int>{}};
    NodeData payload;
    REQUIRE_FALSE(payload.serialize(InputData{&value, meta}).has_value());

    leaf.insertSerialized(Iterator{"/root/*"}, payload, ret);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidPath);
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Leaf spanPackConst rejects unsupported options and metadata") {
    Leaf leaf;

    std::array<std::string, 1> paths{"/a"};
    auto const callback = [](std::span<RawConstSpan const>) -> std::optional<Error> {
        return std::nullopt;
    };

    InputMetadata podMeta{InputMetadataT<int>{}};
    auto popResult = leaf.spanPackConst(std::span<const std::string>{paths}, podMeta, Out{} & Pop{}, callback);
    REQUIRE_FALSE(popResult.has_value());
    CHECK(popResult.error().code == Error::Code::NotSupported);

    InputMetadata nonPodMeta{InputMetadataT<std::string>{}};
    auto metaResult = leaf.spanPackConst(std::span<const std::string>{paths}, nonPodMeta, Out{}, callback);
    REQUIRE_FALSE(metaResult.has_value());
    CHECK(metaResult.error().code == Error::Code::NotSupported);
}

TEST_CASE("Leaf spanPackConst validates concrete paths") {
    Leaf leaf;
    InsertReturn ret;

    InputMetadata podMeta{InputMetadataT<int>{}};
    auto const callback = [](std::span<RawConstSpan const>) -> std::optional<Error> {
        return std::nullopt;
    };

    std::array<std::string, 1> badPaths{"alpha"};
    auto badResult = leaf.spanPackConst(std::span<const std::string>{badPaths}, podMeta, Out{}, callback);
    REQUIRE_FALSE(badResult.has_value());
    CHECK(badResult.error().code == Error::Code::InvalidPath);

    int value = 7;
    leaf.in(Iterator{"/a"}, InputData{value}, ret);
    REQUIRE(ret.errors.empty());

    std::array<std::string, 1> skippedPaths{"/a//b"};
    auto skippedResult = leaf.spanPackConst(std::span<const std::string>{skippedPaths}, podMeta, Out{}, callback);
    REQUIRE_FALSE(skippedResult.has_value());
    CHECK(skippedResult.error().code == Error::Code::NoSuchPath);
}

TEST_CASE("Leaf insertSerialized rejects when POD payload already exists") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 8;
    leaf.in(Iterator{"/root"}, InputData{value}, ret);
    REQUIRE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 1);

    InputMetadata meta{InputMetadataT<int>{}};
    NodeData payload;
    REQUIRE_FALSE(payload.serialize(InputData{&value, meta}).has_value());

    InsertReturn serializedRet;
    leaf.insertSerialized(Iterator{"/root"}, payload, serializedRet);
    REQUIRE_FALSE(serializedRet.errors.empty());
    CHECK(serializedRet.errors.front().code == Error::Code::InvalidType);
    CHECK(serializedRet.errors.front().message ==
          std::optional<std::string>{"Node already holds POD fast-path payload"});
    CHECK(serializedRet.nbrValuesInserted == 0);
}

TEST_CASE("Leaf pod insert reports missing input pointer") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 13;
    InputData input{value};
    input.obj = nullptr;

    leaf.in(Iterator{"/root"}, input, ret);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidType);
    CHECK(ret.errors.front().message == std::optional<std::string>{"Input pointer missing"});
}

TEST_CASE("Leaf insertSerialized ignores empty payloads") {
    Leaf         leaf;
    InsertReturn ret;

    NodeData emptyPayload;
    leaf.insertSerialized(Iterator{"/root"}, emptyPayload, ret);
    CHECK(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Leaf insertSerialized reports invalid paths") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 21;
    InputMetadata meta{InputMetadataT<int>{}};
    NodeData payload;
    REQUIRE_FALSE(payload.serialize(InputData{&value, meta}).has_value());

    leaf.insertSerialized(Iterator{"/bad//path"}, payload, ret);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidPath);
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Leaf insertSerialized rejects root path") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 34;
    InputMetadata meta{InputMetadataT<int>{}};
    NodeData payload;
    REQUIRE_FALSE(payload.serialize(InputData{&value, meta}).has_value());

    leaf.insertSerialized(Iterator{"/"}, payload, ret);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidPath);
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Leaf spanPackConst reports missing paths") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 55;
    leaf.in(Iterator{"/present"}, InputData{value}, ret);
    REQUIRE(ret.errors.empty());

    std::array<std::string, 2> paths{"/present", "/missing"};
    InputMetadata meta{InputMetadataT<int>{}};
    auto const callback = [](std::span<RawConstSpan const>) -> std::optional<Error> {
        return std::nullopt;
    };

    auto result = leaf.spanPackConst(std::span<const std::string>{paths}, meta, Out{}, callback);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NoSuchPath);
    CHECK(result.error().message == std::optional<std::string>{"Path not found"});
}

TEST_CASE("Leaf spanPackMut minimal requires POD payload") {
    Leaf         leaf;
    InsertReturn ret;

    std::string value = "payload";
    leaf.in(Iterator{"/node"}, InputData{value}, ret);
    REQUIRE(ret.errors.empty());

    std::array<std::string, 1> paths{"/node"};
    InputMetadata meta{InputMetadataT<int>{}};
    auto const callback = [](std::span<RawMutSpan const>) -> SpanPackResult {
        return SpanPackResult{.error = std::nullopt, .shouldPop = false};
    };

    auto result = leaf.spanPackMut(std::span<const std::string>{paths}, meta, Out{} & Minimal{}, callback);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidType);
    CHECK(result.error().message == std::optional<std::string>{"POD fast path unavailable"});
}

TEST_CASE("Leaf mergeInsertReturn aggregates retargets and errors") {
    InsertReturn into;
    InsertReturn from;

    from.nbrValuesInserted   = 2;
    from.nbrSpacesInserted   = 1;
    from.nbrTasksInserted    = 3;
    from.nbrValuesSuppressed = 4;
    from.retargets.push_back(InsertReturn::RetargetRequest{nullptr, "/mount"});
    from.errors.emplace_back(Error{Error::Code::InvalidPath, "bad"});

    Leaf::mergeInsertReturn(into, from);

    CHECK(into.nbrValuesInserted == 2);
    CHECK(into.nbrSpacesInserted == 1);
    CHECK(into.nbrTasksInserted == 3);
    CHECK(into.nbrValuesSuppressed == 4);
    CHECK(into.retargets.size() == 1);
    CHECK(into.retargets.front().mountPrefix == "/mount");
    CHECK(into.errors.size() == 1);
    CHECK(into.errors.front().code == Error::Code::InvalidPath);
}

TEST_CASE("Leaf in reports serialization missing for existing node data") {
    Leaf         leaf;
    InsertReturn ret;

    std::string payload = "alpha";
    leaf.in(Iterator{"/node"}, InputData{payload}, ret);
    REQUIRE(ret.errors.empty());

    InputMetadata badMeta{};
    badMeta.dataCategory     = DataCategory::Fundamental;
    badMeta.functionCategory = FunctionCategory::None;
    badMeta.typeInfo         = &typeid(int);
    badMeta.serialize        = nullptr;
    badMeta.deserialize      = nullptr;
    badMeta.deserializePop   = nullptr;
    badMeta.podPreferred     = false;

    int value = 7;
    InsertReturn badRet;
    leaf.in(Iterator{"/node"}, InputData{&value, badMeta}, badRet);
    REQUIRE_FALSE(badRet.errors.empty());
    CHECK(badRet.errors.front().code == Error::Code::SerializationFunctionMissing);
}

TEST_CASE("Leaf in rejects podPreferred metadata without factory") {
    Leaf         leaf;
    InsertReturn ret;

    InputMetadata meta{};
    meta.dataCategory     = DataCategory::Fundamental;
    meta.functionCategory = FunctionCategory::None;
    meta.typeInfo         = &typeid(int);
    meta.podPreferred     = true;
    meta.createPodPayload = nullptr;

    int value = 9;
    leaf.in(Iterator{"/pod"}, InputData{&value, meta}, ret);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidType);
    CHECK(ret.errors.front().message ==
          std::optional<std::string>{"POD fast path unavailable for this node"});
}

TEST_CASE("Leaf in reports POD migration failure on type mismatch") {
    Leaf         leaf;
    InsertReturn ret;

    auto& child = leaf.rootNode().getOrCreateChild("node");
    child.podPayload = std::make_shared<FailingPodPayload>(MigrationFailureMode::TakeError);

    double value = 1.0;
    leaf.in(Iterator{"/node"}, InputData{value}, ret);
    REQUIRE_FALSE(ret.errors.empty());
    bool sawUpgradeError = false;
    bool sawTakeError = false;
    for (auto const& err : ret.errors) {
        sawUpgradeError = sawUpgradeError || (err.code == Error::Code::InvalidType);
        sawTakeError = sawTakeError || (err.code == Error::Code::UnknownError);
    }
    CHECK(sawUpgradeError);
    CHECK(sawTakeError);
}

TEST_CASE("Leaf in falls back when POD payload rejects push") {
    Leaf         leaf;
    InsertReturn ret;

    auto payload = std::make_shared<RejectingPodPayload>();
    auto& child = leaf.rootNode().getOrCreateChild("node");
    child.podPayload = payload;

    int value = 7;
    leaf.in(Iterator{"/node"}, InputData{value}, ret);
    CHECK(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 1);
    CHECK(payload->pushCalled);

    std::lock_guard<std::mutex> guard(child.payloadMutex);
    CHECK(child.data != nullptr);
    CHECK(child.podPayload == nullptr);
}

TEST_CASE("Leaf out reports POD type mismatch") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 4;
    leaf.in(Iterator{"/node"}, InputData{value}, ret);
    REQUIRE(ret.errors.empty());

    double outValue = 0.0;
    InputMetadata mismatchMeta{InputMetadataT<double>{}};
    auto err = leaf.out(Iterator{"/node"}, mismatchMeta, &outValue, false);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidType);
}

TEST_CASE("Leaf out glob reports no matches") {
    Leaf         leaf;
    int          outValue = 0;
    InputMetadata meta{InputMetadataT<int>{}};

    auto err = leaf.out(Iterator{"/missing*"}, meta, &outValue, false);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoSuchPath);
}

TEST_CASE("Leaf out glob ignores empty matches") {
    Leaf         leaf;
    int          outValue = 0;
    InputMetadata meta{InputMetadataT<int>{}};

    leaf.rootNode().getOrCreateChild("empty");

    auto err = leaf.out(Iterator{"/empty*"}, meta, &outValue, false);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoSuchPath);
}

TEST_CASE("Leaf in reports missing nested index during traversal") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 3;
    leaf.in(Iterator{"/parent"}, InputData{value}, ret);
    REQUIRE(ret.errors.empty());

    InsertReturn nestedRet;
    leaf.in(Iterator{"/parent[1]/child"}, InputData{value}, nestedRet);
    REQUIRE_FALSE(nestedRet.errors.empty());
    CHECK(nestedRet.errors.front().code == Error::Code::NoSuchPath);
}

TEST_CASE("Leaf span out reports missing child") {
    Leaf         leaf;
    InputMetadata meta{InputMetadataT<int>{}};
    meta.spanReader = [](void const*, std::size_t) {};

    int outValue = 0;
    auto err = leaf.out(Iterator{"/missing"}, meta, &outValue, false);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoSuchPath);
}

TEST_CASE("Leaf indexed nested inserts are rejected") {
    Leaf         leaf;
    InsertReturn ret;

    auto nested = std::make_unique<PathSpace>();
    leaf.in(Iterator{"/mount[1]"}, InputData{nested}, ret);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidPath);
    CHECK(ret.errors.front().message ==
          std::optional<std::string>{"Indexed nested inserts are not supported"});
}

TEST_CASE("Leaf rebaseRetargets rewrites nested mount prefixes") {
    Leaf         leaf;
    InsertReturn ret;

    std::unique_ptr<PathSpaceBase> mount = std::make_unique<RetargetSpace>();
    leaf.in(Iterator{"/mount"}, InputData{mount}, ret);
    REQUIRE(ret.errors.empty());

    int value = 9;
    InsertReturn nestedRet;
    leaf.in(Iterator{"/mount/child"}, InputData{value}, nestedRet);
    REQUIRE(nestedRet.errors.empty());
    REQUIRE(nestedRet.retargets.size() == 1);
    CHECK(nestedRet.retargets.front().mountPrefix == "/mount/child");
}

TEST_CASE("Leaf insertSerialized rejects nested path targets") {
    Leaf         leaf;
    InsertReturn ret;

    auto nested = std::make_unique<PathSpace>();
    leaf.in(Iterator{"/mount"}, InputData{nested}, ret);
    REQUIRE(ret.errors.empty());

    int value = 11;
    NodeData payload;
    REQUIRE_FALSE(payload.serialize(InputData{value}).has_value());

    InsertReturn serializedRet;
    leaf.insertSerialized(Iterator{"/mount/child"}, payload, serializedRet);
    REQUIRE_FALSE(serializedRet.errors.empty());
    CHECK(serializedRet.errors.front().code == Error::Code::NotSupported);
    CHECK(serializedRet.errors.front().message ==
          std::optional<std::string>{"Serialized inserts cannot target nested PathSpaces yet"});
}

TEST_CASE("Leaf insertSerialized reports append failures from nested payloads") {
    Leaf         leaf;
    InsertReturn ret;

    int value = 3;
    NodeData payload;
    REQUIRE_FALSE(payload.serialize(InputData{value}).has_value());

    leaf.insertSerialized(Iterator{"/root"}, payload, ret);
    REQUIRE(ret.errors.empty());

    auto nested = std::make_unique<PathSpace>();
    InputData nestedInput{nested};
    NodeData nestedPayload;
    REQUIRE_FALSE(nestedPayload.serialize(nestedInput).has_value());

    InsertReturn nestedRet;
    leaf.insertSerialized(Iterator{"/root"}, nestedPayload, nestedRet);
    REQUIRE_FALSE(nestedRet.errors.empty());
    CHECK(nestedRet.errors.front().code == Error::Code::NotSupported);
    CHECK(nestedRet.errors.front().message ==
          std::optional<std::string>{"Nested PathSpaces cannot be serialized across mounts"});
}

TEST_CASE("Leaf ensureNodeData reports pod migration take failures") {
    Leaf         leaf;
    InsertReturn ret;

    leaf.rootNode().podPayload = std::make_shared<FailingPodPayload>(MigrationFailureMode::TakeError);

    int value = 5;
    auto* data = Leaf::ensureNodeData(leaf.rootNode(), InputData{value}, ret);
    CHECK(data == nullptr);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::UnknownError);
}

TEST_CASE("Leaf ensureNodeData reports pod migration serialization failures") {
    Leaf         leaf;
    InsertReturn ret;

    leaf.rootNode().podPayload = std::make_shared<FailingPodPayload>(MigrationFailureMode::SerializeError);

    int value = 6;
    auto* data = Leaf::ensureNodeData(leaf.rootNode(), InputData{value}, ret);
    CHECK(data == nullptr);
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::SerializationFunctionMissing);
}

TEST_SUITE_END();

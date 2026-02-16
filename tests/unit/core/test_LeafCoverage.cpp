#include "third_party/doctest.h"

#include "core/Leaf.hpp"
#include "core/InsertReturn.hpp"
#include "core/NodeData.hpp"
#include "path/Iterator.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>

using namespace SP;

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

TEST_SUITE_END();

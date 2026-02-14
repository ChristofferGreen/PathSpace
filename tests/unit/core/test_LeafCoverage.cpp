#include "third_party/doctest.h"

#include "core/Leaf.hpp"
#include "core/InsertReturn.hpp"
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

TEST_SUITE_END();

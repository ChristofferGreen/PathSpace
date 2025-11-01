#include "history/CowSubtreePrototype.hpp"

#include "third_party/doctest.h"

#include <initializer_list>
#include <vector>

using namespace SP::History;

namespace {
auto makePayload(std::initializer_list<std::uint8_t> init) -> CowSubtreePrototype::Payload {
    std::vector<std::uint8_t> bytes(init);
    return CowSubtreePrototype::Payload(std::move(bytes));
}

auto toMutation(std::string_view path, std::initializer_list<std::uint8_t> init)
    -> CowSubtreePrototype::Mutation {
    auto components = CowSubtreePrototype::parsePath(path);
    REQUIRE_MESSAGE(components.has_value(), "Path parsing failed for: " << path);
    return CowSubtreePrototype::Mutation{std::move(*components), makePayload(init)};
}

auto fetchNode(CowSubtreePrototype::Snapshot const& snapshot, std::string_view path)
    -> CowSubtreePrototype::NodePtr {
    if (!snapshot.root) {
        return {};
    }
    auto componentsOpt = CowSubtreePrototype::parsePath(path);
    REQUIRE_MESSAGE(componentsOpt.has_value(), "Path parsing failed for: " << path);
    auto const& components = *componentsOpt;
    CowSubtreePrototype::NodePtr current = snapshot.root;
    for (auto const& part : components) {
        if (!current) return {};
        auto it = current->children.find(part);
        if (it == current->children.end()) return {};
        current = it->second;
    }
    return current;
}
} // namespace

TEST_SUITE("CowSubtreePrototype") {
    TEST_CASE("parsePath rejects globs") {
        auto value = CowSubtreePrototype::parsePath("/widgets/*");
        CHECK_FALSE(value.has_value());
    }

    TEST_CASE("parsePath handles concrete path") {
        auto value = CowSubtreePrototype::parsePath("/widgets/a/state");
        REQUIRE(value.has_value());
        CHECK(value->size() == 3);
        CHECK((*value)[0] == "widgets");
        CHECK((*value)[1] == "a");
        CHECK((*value)[2] == "state");
    }

    TEST_CASE("apply clones modified branch only") {
        CowSubtreePrototype proto;

        auto base = proto.emptySnapshot();
        auto snapA =
            proto.apply(base, toMutation("/widgets/a/state", {0x01, 0x02, 0x03}));
        auto statsA = proto.analyze(snapA);
        CHECK(statsA.uniqueNodes == 4); // root + widgets + a + state
        CHECK(statsA.payloadBytes == 3);

        auto snapAB =
            proto.apply(snapA, toMutation("/widgets/b/state", {0x04, 0x05}));
        auto deltaAB = proto.analyzeDelta(snapA, snapAB);
        CHECK(deltaAB.newNodes == 4);
        CHECK(deltaAB.reusedNodes == 2);
        CHECK(deltaAB.removedNodes == 2);
        CHECK(deltaAB.newPayloadBytes == 2);
        CHECK(deltaAB.reusedPayloadBytes == 3);

        auto nodeAStateBefore = fetchNode(snapA, "/widgets/a/state");
        auto nodeBBefore      = fetchNode(snapAB, "/widgets/b");
        auto snapABUpdated =
            proto.apply(snapAB, toMutation("/widgets/a/state", {0x06}));
        auto nodeBBetween = fetchNode(snapABUpdated, "/widgets/b");
        REQUIRE(nodeBBefore);
        REQUIRE(nodeBBetween);
        CHECK(nodeBBefore.get() == nodeBBetween.get());

        auto deltaUpdate = proto.analyzeDelta(snapAB, snapABUpdated);
        CHECK(deltaUpdate.newNodes == 4);
        CHECK(deltaUpdate.reusedNodes == 2);
        CHECK(deltaUpdate.newPayloadBytes == 1);
        CHECK(deltaUpdate.reusedPayloadBytes == 2);
        CHECK(deltaUpdate.removedNodes == 4);

        auto nodeAStateAfter = fetchNode(snapABUpdated, "/widgets/a/state");
        REQUIRE(nodeAStateBefore);
        REQUIRE(nodeAStateAfter);
        CHECK(nodeAStateBefore.get() != nodeAStateAfter.get());
        CHECK(nodeAStateAfter->payload.size() == 1);
    }
}

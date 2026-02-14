#include "core/Node.hpp"

#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("core.node") {
TEST_CASE("operator new/delete handles size mismatches and nullptr") {
    void* raw = Node::operator new(sizeof(Node) + 8);
    REQUIRE(raw != nullptr);

    Node::operator delete(raw, sizeof(Node) + 8);
    Node::operator delete(nullptr);

    Node* node = new Node();
    REQUIRE(node != nullptr);
    Node::operator delete(node, sizeof(Node));
    Node::operator delete(nullptr, sizeof(Node));
}

TEST_CASE("getOrCreateChild registers and returns child") {
    Node node;
    CHECK_FALSE(node.hasChildren());

    Node& child = node.getOrCreateChild("alpha");
    CHECK(node.hasChildren());
    CHECK(node.getChild("alpha") == &child);

    Node const& constNode = node;
    CHECK(constNode.getChild("alpha") == &child);
}

TEST_CASE("Node child management and payload helpers") {
    Node node;
    CHECK(node.isLeaf());
    CHECK_FALSE(node.hasChildren());
    CHECK_FALSE(node.hasData());
    CHECK(node.getChild("missing") == nullptr);

    auto& child = node.getOrCreateChild("alpha");
    CHECK(&child != nullptr);
    CHECK(node.hasChildren());
    CHECK_FALSE(node.isLeaf());
    CHECK(node.getChild("alpha") == &child);

    std::vector<std::string> names;
    node.forEachChild([&](std::string_view name, Node&) { names.emplace_back(name); });
    REQUIRE(names.size() == 1);
    CHECK(names.front() == "alpha");

    CHECK(node.eraseChild("missing") == false);
    CHECK(node.eraseChild("alpha"));
    CHECK_FALSE(node.hasChildren());

    node.data = std::make_unique<NodeData>();
    CHECK(node.hasData());
    node.clearLocal();
    CHECK_FALSE(node.hasData());

    node.podPayload = PodPayload<int>::CreateShared();
    CHECK(node.hasData());
    node.clearLocal();
    CHECK_FALSE(node.hasData());

    node.getOrCreateChild("beta");
    node.data = std::make_unique<NodeData>();
    CHECK(node.hasChildren());
    CHECK(node.hasData());
    node.clearRecursive();
    CHECK_FALSE(node.hasChildren());
    CHECK_FALSE(node.hasData());
    CHECK(node.isLeaf());
}
}

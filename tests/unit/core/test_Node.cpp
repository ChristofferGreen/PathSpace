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
}

#include "third_party/doctest.h"

#include "core/Node.hpp"

using namespace SP;

TEST_SUITE_BEGIN("core.node.coverage");

TEST_CASE("Node getOrCreateChild returns stable child") {
    Node root;

    Node& first = root.getOrCreateChild("alpha");
    CHECK(root.getChild("alpha") == &first);

    Node& second = root.getOrCreateChild("alpha");
    CHECK(&first == &second);
}

TEST_SUITE_END();

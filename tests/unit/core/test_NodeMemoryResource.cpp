#include "core/Node.hpp"

#include "CountingMemoryResource.hpp"
#include "third_party/doctest.h"

#include <string>

using namespace SP;
using SP::testing::CountingMemoryResource;

TEST_SUITE("core.node.memory_resource") {
TEST_CASE("default node uses the default resource") {
    Node node;
    CHECK(node.memoryResource() == std::pmr::get_default_resource());
}

TEST_CASE("children map allocates from the node's resource and releases it") {
    CountingMemoryResource counting;
    {
        Node node{&counting};
        CHECK(node.memoryResource() == &counting);
        auto const baseline = counting.allocations();

        node.getOrCreateChild("alpha");
        CHECK(counting.allocations() > baseline); // the map node holding the entry
        CHECK(node.hasChildren());
    }
    CHECK(counting.liveAllocations() == 0);
    CHECK(counting.liveBytes() == 0);
}

TEST_CASE("children inherit the resource, including keys longer than the small-string buffer") {
    CountingMemoryResource counting;
    {
        Node        node{&counting};
        std::string longName(200, 'x');
        Node&       child      = node.getOrCreateChild(longName);
        Node&       grandchild = child.getOrCreateChild("leaf");
        CHECK(child.memoryResource() == &counting);
        CHECK(grandchild.memoryResource() == &counting);
        CHECK(node.getChild(longName) == &child);
        CHECK(node.getChild(std::string_view{longName}) == &child);

        std::size_t visited = 0;
        node.forEachChild([&](std::string_view name, Node const&) {
            CHECK(name == longName);
            ++visited;
        });
        CHECK(visited == 1);
        CHECK(node.eraseChild(longName));
    }
    CHECK(counting.liveAllocations() == 0);
}

TEST_CASE("clearRecursive returns child storage to the resource") {
    CountingMemoryResource counting;
    Node                   node{&counting};
    for (int i = 0; i < 64; ++i) {
        node.getOrCreateChild("child" + std::to_string(i)).getOrCreateChild("inner");
    }
    auto const liveBefore = counting.liveAllocations();
    CHECK(liveBefore > 0);
    node.clearRecursive();
    CHECK_FALSE(node.hasChildren());
    CHECK(counting.liveAllocations() < liveBefore);
}
}

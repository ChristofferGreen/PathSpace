#include <catch2/catch_test_macros.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/path/ConcretePathIterator.hpp>

#include <set>

using namespace SP;

TEST_CASE("ConcretePathIterator") {
    SECTION("Basic Iterator Begin", "[Path][ConcretePathIterator][ConcreteName]") {
        ConcretePathStringView path{"/a/b/c"};
        REQUIRE(*path.begin() == "a");
    }

    SECTION("Standard ForEach Name Iteration", "[Path][ConcretePathIterator][ConcreteName]") {
        ConcretePathStringView path{"/a/b/c"};
        REQUIRE(path.isValid());
        std::set<ConcreteName> s{"a", "b", "c"};
        for(auto const &name : path) {
            REQUIRE(s.contains(name));
            s.erase(name);
        }
        REQUIRE(s.empty());
    }
}

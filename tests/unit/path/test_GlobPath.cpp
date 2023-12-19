#include <catch2/catch_test_macros.hpp>
#include <pathspace/path/GlobPath.hpp>

#include <set>

using namespace SP;

TEST_CASE("GlobPath") {
    SECTION("Basic Iterator Begin", "[Path][GlobPath]") {
        GlobPathStringView path{"/a/b/c"};
        REQUIRE(*path.begin() == "a");
    }
}

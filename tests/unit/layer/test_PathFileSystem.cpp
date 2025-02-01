#include "ext/doctest.h"
#include <pathspace/layer/PathFileSystem.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathSpace FileSystem") {
    SUBCASE("Basic") {
        PathFileSystem space("../../tests/data/filesystem/");
        CHECK(space.read<"/a.txt", std::string>().has_value());
        CHECK(space.read<"/a.txt", std::string>().value() == "hello");

        CHECK(space.read<"/b.txt", std::string>().has_value());
        CHECK(space.read<"/b.txt", std::string>().value() == "world");

        CHECK(space.read<"/c/d.txt", std::string>().has_value());
        CHECK(space.read<"/c/d.txt", std::string>().value() == "!");
    }

    SUBCASE("Subspace") {
        /*PathSpace space;
        space.insert<"/fs">(std::make_unique<PathFileSystem>("../../tests/data/filesystem/"));

        CHECK(space.read<"/fs/a.txt", std::string>().has_value());
        CHECK(space.read<"/fs/a.txt", std::string>().value() == "hello");

        CHECK(space.read<"/fs/b.txt", std::string>().has_value());
        CHECK(space.read<"/fs/b.txt", std::string>().value() == "world");

        CHECK(space.read<"/fs/c/d.txt", std::string>().has_value());
        CHECK(space.read<"/fs/c/d.txt", std::string>().value() == "!");*/
    }
}

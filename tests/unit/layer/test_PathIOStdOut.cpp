#include "third_party/doctest.h"
#include <string>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/io/PathIOStdOut.hpp>

using namespace SP;

TEST_CASE("PathIOStdOut - direct usage accepts std::string") {
    PathIOStdOut out(/*addNewline=*/true, /*prefix=*/"[stdout] ");

    SUBCASE("Insert std::string prints and returns success") {
        auto ret = out.insert<"/print", std::string>(std::string("hello world"));
        CHECK(ret.errors.empty());
        CHECK(ret.nbrValuesInserted == 1);

        CHECK(ret.nbrTasksInserted == 0);
        CHECK(ret.nbrSpacesInserted == 0);
    }

    SUBCASE("Read is unsupported via base") {
        auto r = out.read<"/print", std::string>();
        CHECK_FALSE(r.has_value());
    }
}

TEST_CASE("PathIOStdOut - rejects non-string types") {
    PathIOStdOut out;

    SUBCASE("Insert int returns InvalidType error") {
        auto ret = out.insert<"/print", int>(123);
        CHECK_FALSE(ret.errors.empty());
        REQUIRE(ret.errors.size() == 1);
        CHECK(ret.errors[0].code == Error::Code::InvalidType);
        CHECK(ret.nbrValuesInserted == 0);
    }
}

TEST_CASE("PathIOStdOut - mounting under PathSpace forwards insert") {
    PathSpace space;

    SUBCASE("Mounted at /out, accepts std::string and prints") {
        space.insert<"/out">(std::make_unique<PathIOStdOut>(/*addNewline=*/true, /*prefix=*/"[test] "));
        auto ret = space.insert<"/out/anything", std::string>(std::string("mounted ok"));
        CHECK(ret.errors.empty());
        CHECK(ret.nbrValuesInserted == 1);
    }

    SUBCASE("Mounted at /out, read remains unsupported") {
        space.insert<"/out">(std::make_unique<PathIOStdOut>());
        auto r = space.read<"/out/anything", std::string>();
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("Mounted instance rejects non-string types") {
        space.insert<"/out">(std::make_unique<PathIOStdOut>());
        auto ret = space.insert<"/out/anything", int>(42);
        CHECK_FALSE(ret.errors.empty());
        REQUIRE(ret.errors.size() == 1);
        CHECK(ret.errors[0].code == Error::Code::InvalidType);
    }
}
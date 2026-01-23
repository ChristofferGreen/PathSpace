#include "third_party/doctest.h"

#include <pathspace/path/TransparentString.hpp>

#include <string>
#include <string_view>
#include <unordered_set>

using namespace SP;

TEST_SUITE_BEGIN("path.transparent_string");

TEST_CASE("TransparentStringHash hashes std::string, string_view, and C strings equally") {
    TransparentStringHash hasher;
    std::string           owned{"token"};
    std::string_view      view{owned};
    const char*           cstr = "token";

    auto hOwned = hasher(owned);
    CHECK(hOwned == hasher(view));
    CHECK(hOwned == hasher(cstr));
}

TEST_CASE("TransparentStringHash enables heterogeneous lookup") {
    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> values;
    values.insert("alpha");
    values.insert("beta");

    CHECK(values.find(std::string_view{"alpha"}) != values.end());
    CHECK(values.find("beta") != values.end());
    CHECK(values.find("gamma") == values.end());
}

TEST_SUITE_END();

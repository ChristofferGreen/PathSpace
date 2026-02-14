#include "path/ConcretePathIterator.hpp"
#include "third_party/doctest.h"

#include <string>

using namespace SP;

TEST_SUITE("path.concrete_iterator") {
TEST_CASE("ConcretePathIterator skips redundant slashes and stops at end") {
    std::string path = "///alpha//beta/";
    ConcretePathIterator<std::string> iter{path.begin(), path.end()};
    ConcretePathIterator<std::string> end{path.end(), path.end()};

    CHECK_FALSE(iter == end);
    CHECK((*iter).getName() == "alpha");

    ++iter;
    CHECK_FALSE(iter == end);
    CHECK((*iter).getName() == "beta");

    ++iter;
    CHECK(iter == end);
}

TEST_CASE("ConcretePathIterator equality compares current iterator position") {
    std::string path = "/alpha/beta";
    ConcretePathIterator<std::string> first{path.begin(), path.end()};
    ConcretePathIterator<std::string> second{path.begin(), path.end()};
    ConcretePathIterator<std::string> end{path.end(), path.end()};

    CHECK(first == second);
    CHECK_FALSE(first == end);

    ++second;
    CHECK_FALSE(first == second);
}

TEST_CASE("ConcretePathIterator supports string_view input and empty components") {
    std::string_view path = "////";
    ConcretePathIterator<std::string_view> iter{path.begin(), path.end()};
    ConcretePathIterator<std::string_view> end{path.end(), path.end()};
    CHECK(iter == end);

    std::string_view withNames = "/one/two";
    ConcretePathIterator<std::string_view> viewIter{withNames.begin(), withNames.end()};
    CHECK_FALSE(viewIter == end);
    CHECK((*viewIter).getName() == "one");
}

TEST_CASE("ConcretePathIterator handles relative paths without leading slashes") {
    std::string path = "alpha/beta";
    ConcretePathIterator<std::string> iter{path.begin(), path.end()};
    ConcretePathIterator<std::string> end{path.end(), path.end()};

    CHECK_FALSE(iter == end);
    CHECK((*iter).getName() == "alpha");
    ++iter;
    CHECK_FALSE(iter == end);
    CHECK((*iter).getName() == "beta");
    ++iter;
    CHECK(iter == end);
}
}

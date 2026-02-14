#include "third_party/doctest.h"

#include "path/Iterator.hpp"

#include <string>
#include <string_view>

using namespace SP;

namespace SP {
// Friend hook declared inside Iterator to reach the iterator-range constructor for coverage.
struct IteratorTestAccess {
    static Iterator fromRange(std::string path) {
        return Iterator(path.cbegin(), path.cend());
    }

    static Iterator fromRange(std::string path, std::size_t begin, std::size_t end) {
        auto first = path.cbegin() + static_cast<std::ptrdiff_t>(begin);
        auto last  = path.cbegin() + static_cast<std::ptrdiff_t>(end);
        return Iterator(first, last);
    }
};
} // namespace SP

TEST_SUITE_BEGIN("path.iterator.range");

TEST_CASE("range constructor skips leading separators and seeds first component") {
    std::string path = "///alpha//beta";
    auto        iter = IteratorTestAccess::fromRange(path);

    CHECK(iter.toStringView() == path);
    CHECK(iter.isAtStart());
    CHECK(iter.currentComponent() == "alpha");
    CHECK(iter.startToCurrent() == "/");
    CHECK(iter.currentToEnd() == "alpha//beta");

    ++iter;
    CHECK(iter.currentComponent() == "beta");
    CHECK(iter.isAtFinalComponent());
    CHECK_FALSE(iter.isAtEnd());

    ++iter;
    CHECK(iter.isAtEnd());
}

TEST_CASE("range constructor handles sub-range starting mid-path") {
    std::string path = "/root/child/leaf";
    auto first       = path.find("/child");
    REQUIRE(first != std::string::npos);

    auto iter = IteratorTestAccess::fromRange(path, first, path.size());

    CHECK(iter.toStringView() == "/child/leaf");
    CHECK(iter.isAtStart());
    CHECK(iter.currentComponent() == "child");
    CHECK(iter.startToCurrent().empty());
    CHECK(iter.currentToEnd() == "child/leaf");

    auto next = iter.next();
    CHECK(next.currentComponent() == "leaf");
    CHECK(next.isAtFinalComponent());
}

TEST_CASE("range constructor produces end iterator when only separators remain") {
    auto iter = IteratorTestAccess::fromRange(std::string{"////"});
    CHECK(iter.isAtEnd());
    CHECK(iter.currentComponent().empty());
}

TEST_CASE("range constructor handles empty input") {
    auto iter = IteratorTestAccess::fromRange(std::string{});
    CHECK(iter.isAtEnd());
    CHECK(iter.currentComponent().empty());
    CHECK(iter.toStringView().empty());
}

TEST_SUITE_END();

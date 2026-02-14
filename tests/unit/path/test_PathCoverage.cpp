#include "path/Path.hpp"
#include "path/ConcreteName.hpp"
#include "path/GlobPathIterator.hpp"

#include "third_party/doctest.h"

#include <string>
#include <vector>

using namespace SP;

TEST_SUITE("path.coverage") {
TEST_CASE("Path validity rejects missing slash and embedded relative segments") {
    Path<std::string> noSlash{"relative/path"};
    CHECK_FALSE(noSlash.isValid());

    Path<std::string> empty{""};
    CHECK_FALSE(empty.isValid()); // empty path should fail fast

    Path<std::string> root{"/"};
    CHECK(root.isValid());

    Path<std::string> embeddedDot{"/root/.hidden"};
    CHECK_FALSE(embeddedDot.isValid());

    Path<std::string> dotRoot{"/."};
    CHECK_FALSE(dotRoot.isValid());

    Path<std::string> dotDot{"/root/../child"};
    CHECK_FALSE(dotDot.isValid());

    Path<std::string> ok{"/root/child"};
    CHECK(ok.isValid());

    Path<std::string_view> viewOk{std::string_view{"/view/ok"}};
    CHECK(viewOk.isValid());
}

TEST_CASE("Path getPath returns underlying storage") {
    std::string backing = "/root/child";
    Path<std::string>   owned{backing};
    CHECK(owned.getPath() == backing);

    std::string_view view{"/view/path"};
    Path<std::string_view> viewPath{view};
    CHECK(viewPath.getPath() == view);
}

TEST_CASE("ConcreteName constructors and comparisons") {
    std::string backing = "segment";

    ConcreteName fromCStr{backing.c_str()};
    ConcreteName fromView{std::string_view{backing}};
    ConcreteName fromIter{backing.cbegin(), backing.cend()};
    std::string_view backingView{backing};
    ConcreteName fromViewIter{backingView.cbegin(), backingView.cend()};

    CHECK(fromCStr == fromView);
    CHECK(fromIter == fromViewIter);
    CHECK(fromCStr == backing.c_str());
    CHECK((fromIter <=> fromView) == std::strong_ordering::equal);
}

TEST_CASE("GlobPathIterator skips redundant slashes and supports postfix increment") {
    std::string glob = "//alpha//beta/gamma";

    std::vector<std::string> segments;
    auto iter = GlobPathIterator<std::string>{glob.begin(), glob.end()};
    auto end  = GlobPathIterator<std::string>{glob.end(), glob.end()};

    REQUIRE(iter != end);
    segments.emplace_back(std::string{(*iter).getName()}); // alpha

    auto prior = iter++; // exercise postfix increment
    CHECK(prior == GlobPathIterator<std::string>{glob.begin(), glob.end()});
    segments.emplace_back(std::string{(*iter).getName()}); // beta

    ++iter; // exercise prefix increment to reach gamma
    if (iter != end) {
        segments.emplace_back(std::string{(*iter).getName()});
    }

    CHECK(segments == std::vector<std::string>{"alpha", "beta", "gamma"});
}

TEST_CASE("GlobPathIterator works with string_view input") {
    std::string_view glob = "/a//b";
    auto iter = GlobPathIterator<std::string_view>{glob.begin(), glob.end()};
    auto end  = GlobPathIterator<std::string_view>{glob.end(), glob.end()};

    REQUIRE(iter != end);
    CHECK((*iter).getName() == "a");
    ++iter;
    REQUIRE(iter != end);
    CHECK((*iter).getName() == "b");
    ++iter;
    CHECK(iter == end);
}

TEST_CASE("GlobPathIterator treats empty or slash-only paths as end") {
    std::string_view glob = "////";
    auto iter = GlobPathIterator<std::string_view>{glob.begin(), glob.end()};
    auto end  = GlobPathIterator<std::string_view>{glob.end(), glob.end()};
    CHECK(iter == end);
}

TEST_CASE("GlobPathIterator handles relative paths without leading slashes") {
    std::string glob = "alpha/beta";
    auto iter = GlobPathIterator<std::string>{glob.begin(), glob.end()};
    auto end  = GlobPathIterator<std::string>{glob.end(), glob.end()};

    REQUIRE(iter != end);
    CHECK((*iter).getName() == "alpha");
    ++iter;
    REQUIRE(iter != end);
    CHECK((*iter).getName() == "beta");
    ++iter;
    CHECK(iter == end);
}
}

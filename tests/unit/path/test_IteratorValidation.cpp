#include "path/Iterator.hpp"
#include "path/validation.hpp"
#include "core/Error.hpp"
#include "third_party/doctest.h"

#include <string_view>

using namespace SP;

TEST_SUITE("path.iterator.validation") {
TEST_CASE("Iterator basic validation surfaces expected errors") {
    Iterator emptyIter{""};
    auto basicErr = emptyIter.validate(ValidationLevel::Basic);
    REQUIRE(basicErr.has_value());
    CHECK(basicErr->code == Error::Code::InvalidPath);
    CHECK(basicErr->message == std::optional<std::string>{"Empty path"});

    Iterator noSlash{"relative"};
    auto basicNoSlash = noSlash.validate(ValidationLevel::Basic);
    REQUIRE(basicNoSlash.has_value());
    CHECK(basicNoSlash->message == std::optional<std::string>{"Path must start with '/'"});    
    auto fullErr = noSlash.validate(ValidationLevel::Full);
    REQUIRE(fullErr.has_value());
    CHECK(fullErr->message == std::optional<std::string>{"Path must start with '/'"});

    Iterator trailing{"/path/"};
    auto basicTrailing = trailing.validate(ValidationLevel::Basic);
    REQUIRE(basicTrailing.has_value());
    CHECK(basicTrailing->message == std::optional<std::string>{"Path ends with slash"});
    auto trailingErr = trailing.validate(ValidationLevel::Full);
    REQUIRE(trailingErr.has_value());
    CHECK(trailingErr->message == std::optional<std::string>{"Path ends with slash"});

    Iterator emptyComponent{"/bad//path"};
    auto emptyCompErr = emptyComponent.validate(ValidationLevel::Full);
    REQUIRE(emptyCompErr.has_value());
    CHECK(emptyCompErr->message == std::optional<std::string>{"Empty path component"});

    Iterator relativeDots{"/./path"};
    auto relErr = relativeDots.validate(ValidationLevel::Full);
    REQUIRE(relErr.has_value());
    CHECK(relErr->message == std::optional<std::string>{"Relative paths not allowed"});

    Iterator nonNumericIndex{"/foo[abc]"};
    auto     indexErr = nonNumericIndex.validate(ValidationLevel::Full);
    CHECK_FALSE(indexErr.has_value()); // glob character classes are permitted at full validation

    Iterator noneLevel{"/ok/path"};
    CHECK_FALSE(noneLevel.validate(ValidationLevel::None).has_value());

    Iterator invalidNone{"/bad//path"};
    CHECK_FALSE(invalidNone.validate(ValidationLevel::None).has_value());

    Iterator root{"/"};
    CHECK_FALSE(root.validate(ValidationLevel::Basic).has_value());
    auto rootFull = root.validate(ValidationLevel::Full);
    REQUIRE(rootFull.has_value());
    CHECK(rootFull->message == std::optional<std::string>{"Empty path"});

    Iterator valid{"/ok"};
    auto invalidLevel = valid.validate(static_cast<ValidationLevel>(999));
    CHECK_FALSE(invalidLevel.has_value());

    auto validFull = valid.validate(ValidationLevel::Full);
    CHECK_FALSE(validFull.has_value());
}

TEST_CASE("Iterator iteration utilities expose start and end slices") {
    Iterator iter{"/a/b/c"};
    CHECK(iter.isAtStart());
    CHECK(iter.startToCurrent().empty());
    CHECK(iter.currentComponent() == "a");
    CHECK(iter.currentToEnd() == "a/b/c");

    ++iter;
    CHECK(iter.currentComponent() == "b");
    CHECK(iter.startToCurrent() == "a");
    CHECK(iter.currentToEnd() == "b/c");

    auto next = iter.next();
    CHECK(next.currentComponent() == "c"); // next() advances the copy
    CHECK(iter.currentComponent() == "b"); // original iterator unchanged

    ++iter;
    CHECK(iter.isAtFinalComponent());
    CHECK(iter.currentComponent() == "c");
    CHECK_FALSE(iter.isAtEnd());
    ++iter;
    CHECK(iter.isAtEnd());
}

TEST_CASE("Iterator slices handle relative paths without leading slash") {
    Iterator iter{"alpha/beta"};
    CHECK(iter.isAtStart());
    CHECK(iter.currentComponent() == "alpha");
    CHECK(iter.startToCurrent().empty());
    CHECK(iter.currentToEnd() == "alpha/beta");

    ++iter;
    CHECK(iter.currentComponent() == "beta");
    CHECK(iter.isAtFinalComponent());
    CHECK(iter.startToCurrent() == "alpha");
    CHECK(iter.currentToEnd() == "beta");
}

TEST_CASE("Iterator constructed from iterator range canonicalizes slashes and equality") {
    std::string pathStr{"//root//child"};
    Iterator    fromRange{pathStr};
    CHECK(fromRange.currentComponent() == "root");
    CHECK(fromRange.currentToEnd() == "root//child");
    CHECK(fromRange == fromRange); // self-equality is true
    auto copy = fromRange;
    CHECK(copy == fromRange); // copies compare equal by content/offset
    ++copy;
    CHECK_FALSE(copy == fromRange);
    CHECK(copy.currentComponent() == "child");
    CHECK(copy.isAtFinalComponent());
    ++copy;
    CHECK(copy.isAtEnd());

    Iterator unequalA{"/a"};
    Iterator unequalB{"/b"};
    CHECK_FALSE(unequalA == unequalB);
}

TEST_CASE("Iterator move assignment preserves component offsets") {
    Iterator source{"/alpha/beta"};
    Iterator target{"/other"};

    target = std::move(source);

    CHECK(target.currentComponent() == "alpha");
    ++target;
    CHECK(target.currentComponent() == "beta");
    CHECK(target.isAtFinalComponent());
    ++target;
    CHECK(target.isAtEnd());
}
}

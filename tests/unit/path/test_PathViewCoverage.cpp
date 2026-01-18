#include "path/ConcretePath.hpp"
#include "path/GlobPath.hpp"
#include "path/ConcretePathIterator.hpp"
#include "path/GlobPathIterator.hpp"
#include "path/ConcreteName.hpp"
#include "path/GlobName.hpp"
#include "layer/PathView.hpp"
#include "PathSpace.hpp"
#include "type/TypeMetadataRegistry.hpp"

#include "third_party/doctest.h"
#include <expected>
#include <string>
#include <vector>

using namespace SP;

TEST_SUITE("path.view.coverage") {
TEST_CASE("ConcreteName and GlobName basics") {
    ConcreteName cname{"alpha"};
    CHECK(cname == "alpha");
    CHECK(cname == ConcreteName{"alpha"});
    CHECK((cname <=> ConcreteName{"beta"}) == std::strong_ordering::less);

    GlobName gstar{"a*"};
    auto [match, super] = gstar.match(std::string_view{"alpha"});
    CHECK(match);
    CHECK_FALSE(super);

    GlobName gquestion{"a?c"};
    auto [match2, super2] = gquestion.match(std::string_view{"abc"});
    CHECK(match2);
    CHECK_FALSE(super2);

    GlobName gset{"b*d"};
    auto [match3, super3] = gset.match(ConcreteName{"bd"});
    CHECK(match3);
    CHECK_FALSE(super3);
}

TEST_CASE("ConcretePathIterator and GlobPathIterator iterate components") {
    ConcretePathString concrete{"/one/two"};
    std::vector<std::string> names;
    for (auto it = concrete.begin(); it != concrete.end(); ++it) {
        names.emplace_back(std::string((*it).getName()));
    }
    CHECK(names == std::vector<std::string>{"one", "two"});

    GlobPathString glob{"/o*/t?"};
    std::vector<std::string> globs;
    for (auto it = glob.begin(); it != glob.end(); ++it) {
        globs.emplace_back(std::string((*it).getName()));
    }
    CHECK(globs == std::vector<std::string>{"o*", "t?"});
}

TEST_CASE("Path validity checks") {
    Path<std::string> valid{"/root/child"};
    CHECK(valid.isValid());

    Path<std::string> noSlash{"relative"};
    CHECK_FALSE(noSlash.isValid());

    Path<std::string> dotPath{"/.hidden"};
    CHECK_FALSE(dotPath.isValid());
}

TEST_CASE("PathView respects permissions and root") {
    auto space = std::make_shared<PathSpace>();
    auto perm = [](Iterator const& iter) -> Permission {
        auto str = iter.toString();
        bool allowed = str.rfind("/allowed", 0) == 0;
        return Permission{.read = allowed, .write = allowed, .execute = allowed};
    };
    PathView view{space, perm, "/root"};

    // Denied write
    auto denied = view.in(Iterator{"/denied/value"}, InputData{42});
    CHECK_FALSE(denied.errors.empty());

    // Allowed write/read through view
    auto ok = view.in(Iterator{"/allowed/value"}, InputData{123});
    REQUIRE(ok.errors.empty());

    int outValue = 0;
    auto err = view.out(Iterator{"/allowed/value"}, InputMetadataT<int>{}, Out{}, &outValue);
    CHECK_FALSE(err.has_value());
    CHECK(outValue == 123);

    // Visit should re-root under view root
    int visitCount = 0;
    VisitOptions opts;
    opts.root = "/allowed";
    opts.includeValues = true;
    auto visitResult = view.visit(
        [&](PathEntry const& entry, ValueHandle& /*handle*/) {
            ++visitCount;
            CHECK_FALSE(entry.path.empty());
            return VisitControl::Continue;
        },
        opts); // explicit root to test joinCanonical
    CHECK(visitResult);
    CHECK(visitCount >= 1);
}

TEST_CASE("TypeMetadataRegistry custom registration and lookup") {
    struct CoverageType {
        int value = 7;
    };

    auto& registry = TypeMetadataRegistry::instance();
    std::string name = "CoverageType_" + std::to_string(reinterpret_cast<std::uintptr_t>(&registry));
    CHECK(registry.registerType<CoverageType>(name));
    CHECK_FALSE(registry.registerType<CoverageType>(name)); // duplicate should fail

    auto byName = registry.findByName(name);
    REQUIRE(byName.has_value());
    CHECK(byName->operations.size == sizeof(CoverageType));
    CHECK(byName->metadata.typeInfo != nullptr);

    auto byType = registry.findByType(typeid(CoverageType));
    REQUIRE(byType.has_value());
    CHECK(byType->type_name == name);
}

TEST_CASE("Expected shim emits what()") {
    std::expected<void, Error> exp = std::unexpected(Error{Error::Code::UnknownError, "boom"});
    try {
        exp.value();
        FAIL("expected to throw");
    } catch (std::bad_expected_access<Error> const& ex) {
        // Touch what() to cover the shim definition.
        CHECK(std::string{ex.what()}.find("bad access to std::expected") != std::string::npos);
    }

    // Explicitly exercise bad_expected_access<void> via a derived shim to hit the TU.
    struct BadVoid : std::bad_expected_access<void> {
        BadVoid() : std::bad_expected_access<void>() {}
        using std::bad_expected_access<void>::what;
    };
    BadVoid vex;
    CHECK(std::string{vex.what()}.find("bad access to std::expected") != std::string::npos);
}
}

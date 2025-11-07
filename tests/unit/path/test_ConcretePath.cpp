#include "third_party/doctest.h"

#include "path/ConcretePath.hpp"

using namespace SP;

TEST_SUITE("ConcretePath") {

TEST_CASE("canonicalized trims and ensures absolute root") {
    ConcretePathStringView canonical{"/widgets/panel"};
    auto normalized = canonical.canonicalized();
    REQUIRE(normalized.has_value());
    CHECK(normalized->getPath() == "/widgets/panel");

    ConcretePathStringView trailing{"/widgets/panel/"};
    auto trimmed = trailing.canonicalized();
    REQUIRE(trimmed.has_value());
    CHECK(trimmed->getPath() == "/widgets/panel");

    ConcretePathStringView missing{ "widgets/panel" };
    auto absolute = missing.canonicalized();
    REQUIRE(absolute.has_value());
    CHECK(absolute->getPath() == "/widgets/panel");

    ConcretePathStringView root{"/"};
    auto canonicalRoot = root.canonicalized();
    REQUIRE(canonicalRoot.has_value());
    CHECK(canonicalRoot->getPath() == "/");
}

TEST_CASE("canonicalized rejects invalid structures") {
    ConcretePathStringView doubleSlash{"/widgets//panel"};
    auto invalid = doubleSlash.canonicalized();
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == Error::Code::InvalidPathSubcomponent);

    ConcretePathStringView glob{"/widgets/*"};
    auto globResult = glob.canonicalized();
    REQUIRE_FALSE(globResult.has_value());
    CHECK(globResult.error().code == Error::Code::InvalidPathSubcomponent);

    ConcretePathStringView relative{"/widgets/../panel"};
    auto relativeResult = relative.canonicalized();
    REQUIRE_FALSE(relativeResult.has_value());
    CHECK(relativeResult.error().code == Error::Code::InvalidPathSubcomponent);
}

TEST_CASE("components extracts concrete names") {
    ConcretePathString path{"/widgets/panel/state"};
    auto components = path.components();
    REQUIRE(components.has_value());
    REQUIRE(components->size() == 3);
    CHECK((*components)[0] == "widgets");
    CHECK((*components)[1] == "panel");
    CHECK((*components)[2] == "state");

    ConcretePathStringView root{"/"};
    auto rootComponents = root.components();
    REQUIRE(rootComponents.has_value());
    CHECK(rootComponents->empty());
}

TEST_CASE("isPrefixOf matches canonical prefixes") {
    ConcretePathString root{"/"};
    auto rootPrefix = root.isPrefixOf(ConcretePathStringView{"/widgets/panel"});
    REQUIRE(rootPrefix.has_value());
    CHECK(rootPrefix.value());

    ConcretePathString widgets{"/widgets"};
    auto nested = widgets.isPrefixOf(ConcretePathStringView{"/widgets/panel"});
    REQUIRE(nested.has_value());
    CHECK(nested.value());

    auto same = widgets.isPrefixOf(ConcretePathStringView{"/widgets"});
    REQUIRE(same.has_value());
    CHECK(same.value());

    auto different = widgets.isPrefixOf(ConcretePathStringView{"/widget"});
    REQUIRE(different.has_value());
    CHECK_FALSE(different.value());

    ConcretePathStringView invalid{"/widgets/*"};
    auto error = widgets.isPrefixOf(invalid);
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().code == Error::Code::InvalidPathSubcomponent);
}

} // TEST_SUITE

#include "third_party/doctest.h"

#include "path/ConcretePath.hpp"

#include <optional>

using namespace SP;

TEST_SUITE("path.concrete") {

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

TEST_CASE("canonicalized reports specific error messages") {
    ConcretePathStringView doubleSlash{"/widgets//panel"};
    auto emptyComponent = doubleSlash.canonicalized();
    REQUIRE_FALSE(emptyComponent.has_value());
    CHECK(emptyComponent.error().message == std::optional<std::string>{"Empty path component"});

    ConcretePathStringView relative{"/widgets/../panel"};
    auto relativeResult = relative.canonicalized();
    REQUIRE_FALSE(relativeResult.has_value());
    CHECK(relativeResult.error().message == std::optional<std::string>{"Relative path components are not allowed"});

    ConcretePathStringView glob{"/widgets/*"};
    auto globResult = glob.canonicalized();
    REQUIRE_FALSE(globResult.has_value());
    CHECK(globResult.error().message == std::optional<std::string>{"Glob syntax is not allowed in concrete paths"});
}

TEST_CASE("components propagates parse errors") {
    ConcretePathStringView invalid{"/widgets/*"};
    auto components = invalid.components();
    REQUIRE_FALSE(components.has_value());
    CHECK(components.error().code == Error::Code::InvalidPathSubcomponent);
    CHECK(components.error().message == std::optional<std::string>{"Glob syntax is not allowed in concrete paths"});
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

    ConcretePathStringView empty{""};
    auto emptyComponents = empty.components();
    REQUIRE(emptyComponents.has_value());
    CHECK(emptyComponents->empty());

    ConcretePathStringView missingSlash{"widgets/panel"};
    auto missingComponents = missingSlash.components();
    REQUIRE(missingComponents.has_value());
    REQUIRE(missingComponents->size() == 2);
    CHECK((*missingComponents)[0] == "widgets");
    CHECK((*missingComponents)[1] == "panel");
}

TEST_CASE("components trims trailing slashes") {
    ConcretePathStringView trailing{"/widgets/panel/"};
    auto components = trailing.components();
    REQUIRE(components.has_value());
    REQUIRE(components->size() == 2);
    CHECK((*components)[0] == "widgets");
    CHECK((*components)[1] == "panel");
}

TEST_CASE("canonicalized allows indexed components and empty paths") {
    ConcretePathStringView empty{""};
    auto emptyNormalized = empty.canonicalized();
    REQUIRE(emptyNormalized.has_value());
    CHECK(emptyNormalized->getPath() == "/");

    ConcretePathStringView indexed{"/node[3]/child"};
    auto indexedNormalized = indexed.canonicalized();
    REQUIRE(indexedNormalized.has_value());
    CHECK(indexedNormalized->getPath() == "/node[3]/child");

    auto components = indexed.components();
    REQUIRE(components.has_value());
    REQUIRE(components->size() == 2);
    CHECK((*components)[0] == "node[3]");
    CHECK((*components)[1] == "child");
}

TEST_CASE("ConcretePath equality reports invalid paths as unequal") {
    ConcretePathStringView invalid{"/bad/.."};
    ConcretePathStringView valid{"/good/path"};

    CHECK_FALSE(invalid == "/bad/..");
    CHECK_FALSE(invalid == std::string_view{"/bad/.."});
    CHECK_FALSE(invalid == valid);
}

TEST_CASE("ConcretePath rejects malformed indexed components") {
    ConcretePathStringView badSuffix{"/node[3]x/child"};
    auto suffixResult = badSuffix.canonicalized();
    REQUIRE_FALSE(suffixResult.has_value());
    CHECK(suffixResult.error().code == Error::Code::InvalidPathSubcomponent);
    CHECK(suffixResult.error().message == std::optional<std::string>{"Glob syntax is not allowed in concrete paths"});

    ConcretePathStringView emptyIndex{"/node[]/child"};
    auto emptyResult = emptyIndex.canonicalized();
    REQUIRE_FALSE(emptyResult.has_value());
    CHECK(emptyResult.error().code == Error::Code::InvalidPathSubcomponent);
    CHECK(emptyResult.error().message == std::optional<std::string>{"Glob syntax is not allowed in concrete paths"});
}

TEST_CASE("ConcretePath equality returns false when component counts differ") {
    ConcretePathStringView longer{"/alpha/beta"};
    ConcretePathStringView shorter{"/alpha"};

    CHECK_FALSE(longer == shorter);
}

TEST_CASE("isPrefixOf reports invalid lhs") {
    ConcretePathStringView invalid{"/bad/./path"};
    auto result = invalid.isPrefixOf(ConcretePathStringView{"/bad/path"});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPathSubcomponent);
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

    ConcretePathString deeper{"/widgets/panel"};
    auto longer = deeper.isPrefixOf(ConcretePathStringView{"/widgets"});
    REQUIRE(longer.has_value());
    CHECK_FALSE(longer.value());

    ConcretePathStringView invalid{"/widgets/*"};
    auto error = widgets.isPrefixOf(invalid);
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().code == Error::Code::InvalidPathSubcomponent);
}

TEST_CASE("isPrefixOf reports invalid rhs") {
    ConcretePathString widgets{"/widgets"};
    ConcretePathStringView invalid{"/widgets/../panel"};

    auto result = widgets.isPrefixOf(invalid);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPathSubcomponent);
}

} // TEST_SUITE

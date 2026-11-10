#include "type/TypeMetadataRegistry.hpp"

#include "third_party/doctest.h"

#include <string>

using namespace SP;

namespace SP {
void RegisterBuiltinTypeMetadata(TypeMetadataRegistry& registry);
} // namespace SP

TEST_SUITE("type.metadata.bootstrap") {

TEST_CASE("builtin registration populates the registry it is given") {
    // The process-wide registry has been bootstrapped by now (or is here).
    REQUIRE(TypeMetadataRegistry::instance().findByType(typeid(int)).has_value());

    // "Already done" must be a property of the registry being filled, not of the process: a
    // second registry starts empty and needs the builtins as well.
    TypeMetadataRegistry local;
    REQUIRE_FALSE(local.findByType(typeid(int)).has_value());
    RegisterBuiltinTypeMetadata(local);
    CHECK(local.findByType(typeid(int)).has_value());
    CHECK(local.findByType(typeid(std::string)).has_value());

    // Registering again stays harmless.
    RegisterBuiltinTypeMetadata(local);
    CHECK(local.findByType(typeid(double)).has_value());
}

}

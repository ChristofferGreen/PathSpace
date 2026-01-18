#include "third_party/doctest.h"

#include <pathspace/type/TypeMetadataRegistry.hpp>

using namespace SP;

namespace {

struct RegistrySampleType {
    int value{0};
};

struct RuntimeRegisteredType {
    double value{0.0};
};

} // namespace

TEST_SUITE("type.metadata.registry") {
TEST_CASE("TypeMetadataRegistry registers template types once") {
    auto& registry = TypeMetadataRegistry::instance();
    CHECK(registry.registerType<RegistrySampleType>("app.RegistrySample"));
    CHECK_FALSE(registry.registerType<RegistrySampleType>("app.RegistrySample"));

    auto by_name = registry.findByName("app.RegistrySample");
    REQUIRE(by_name.has_value());
    CHECK_EQ(by_name->metadata.typeInfo, &typeid(RegistrySampleType));

    auto by_type = registry.findByType(std::type_index(typeid(RegistrySampleType)));
    REQUIRE(by_type.has_value());
    CHECK_EQ(&by_type->metadata, &by_name->metadata);
}

TEST_CASE("TypeMetadataRegistry surfaces registered type metadata for lookups") {
    auto& registry = TypeMetadataRegistry::instance();
    CHECK(registry.registerType<RuntimeRegisteredType>("app.RuntimeRegistered"));
    CHECK_FALSE(registry.registerType<RuntimeRegisteredType>("app.RuntimeRegistered"));

    auto by_name = registry.findByName("app.RuntimeRegistered");
    REQUIRE(by_name.has_value());
    CHECK_EQ(by_name->metadata.typeInfo, &typeid(RuntimeRegisteredType));
    CHECK_EQ(by_name->operations.size, sizeof(RuntimeRegisteredType));
}
}

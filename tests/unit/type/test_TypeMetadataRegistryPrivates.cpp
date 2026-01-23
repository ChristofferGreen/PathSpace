#define private public
#include "type/TypeMetadataRegistry.hpp"
#undef private

#include "third_party/doctest.h"

#include <typeindex>

using namespace SP;

TEST_SUITE("type.typemetadata.privates") {
TEST_CASE("make_view returns nullopt for missing entry") {
    CHECK_FALSE(TypeMetadataRegistry::make_view(nullptr).has_value());
}

TEST_CASE("registerEntry rejects missing metadata or empty names") {
    TypeMetadataRegistry registry;
    InputMetadata        missingMeta{}; // typeInfo == nullptr
    TypeOperations       ops{};

    CHECK_FALSE(registry.registerEntry(std::type_index(typeid(int)),
                                       "int",
                                       missingMeta,
                                       ops));

    InputMetadata validMeta{InputMetadataT<int>{}};
    CHECK_FALSE(registry.registerEntry(std::type_index(typeid(int)),
                                       "",
                                       validMeta,
                                       ops));
}
}

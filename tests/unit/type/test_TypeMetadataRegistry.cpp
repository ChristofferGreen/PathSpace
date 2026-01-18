#include "type/TypeMetadataRegistry.hpp"
#include <pathspace/PathSpace.hpp>

#include "third_party/doctest.h"

using namespace SP;

struct NoTypeInfo {};

namespace SP {
template <>
struct InputMetadataT<NoTypeInfo> {
    static constexpr DataCategory          dataCategory     = DataCategory::None;
    static constexpr FunctionCategory      functionCategory = FunctionCategory::None;
    static constexpr std::type_info const* typeInfo         = nullptr;
    static constexpr auto                  serialize        = nullptr;
    static constexpr auto                  deserialize      = nullptr;
    static constexpr auto                  deserializePop   = nullptr;
    static constexpr bool                  podPreferred     = false;
};

// Expose builtin registration guard for coverage.
void RegisterBuiltinTypeMetadata(TypeMetadataRegistry& registry);
} // namespace SP

namespace {
struct AggregateType {
    int    value{0};
    double weight{0.0};
    auto operator==(AggregateType const&) const -> bool = default;
};
} // namespace

TEST_SUITE("type.typemetadata.registry") {
TEST_CASE("registerType rejects null typeinfo metadata") {
    auto& registry = TypeMetadataRegistry::instance();
    CHECK_FALSE(registry.registerType<NoTypeInfo>(""));
}

TEST_CASE("registerType exposes operations and views") {
    auto& registry = TypeMetadataRegistry::instance();

    CHECK_FALSE(registry.findByName("aggregate_type").has_value());
    REQUIRE(registry.registerType<AggregateType>("aggregate_type"));
    CHECK_FALSE(registry.registerType<AggregateType>("aggregate_type"));

    auto view = registry.findByName("aggregate_type");
    REQUIRE(view.has_value());
    CHECK(view->type_name == "aggregate_type");
    CHECK(view->metadata.typeInfo == &typeid(AggregateType));
    CHECK(view->operations.size == sizeof(AggregateType));
    CHECK(view->operations.alignment == alignof(AggregateType));
    REQUIRE(view->operations.construct != nullptr);
    REQUIRE(view->operations.destroy != nullptr);

    // Placement-construct/destroy through registered operations.
    alignas(AggregateType) std::byte storage[sizeof(AggregateType)];
    view->operations.construct(storage);
    view->operations.destroy(storage);

    // Insert and take round-trip through registered callbacks.
    PathSpace space;
    AggregateType value{};
    value.value  = 42;
    value.weight = 1.5;
    auto insertResult = view->operations.insert(space, "/sample", &value, In{});
    REQUIRE(insertResult.has_value());
    CHECK(insertResult->nbrValuesInserted == 1);

    AggregateType out{};
    auto          takeResult = view->operations.take(space, "/sample", Out{}, &out);
    REQUIRE(takeResult.has_value());
    CHECK(out.value == 42);
    CHECK(out.weight == doctest::Approx(1.5));

    auto typeLookup = registry.findByType(typeid(AggregateType));
   REQUIRE(typeLookup.has_value());
   CHECK(typeLookup->type_name == "aggregate_type");
}

TEST_CASE("RegisterBuiltinTypeMetadata is idempotent and findByName handles misses") {
    auto& registry = TypeMetadataRegistry::instance();

    auto intView = registry.findByType(typeid(int));
    REQUIRE(intView.has_value()); // builtins registered on first instance() call
    RegisterBuiltinTypeMetadata(registry);
    auto intViewAgain = registry.findByType(typeid(int));
    REQUIRE(intViewAgain.has_value());
    CHECK(intView->type_name == intViewAgain->type_name);
    CHECK_FALSE(registry.registerType<int>("int")); // duplicate stays rejected

    auto missing = registry.findByName("pathspace::definitely_missing");
    CHECK_FALSE(missing.has_value());
}
}

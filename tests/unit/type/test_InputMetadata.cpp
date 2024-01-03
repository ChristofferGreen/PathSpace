#include <catch2/catch_test_macros.hpp>
#include <pathspace/type/InputMetadata.hpp>

#include <map>

using namespace SP;

struct CustomClass {};
struct Base { virtual ~Base() = default; template <typename A>void serialize(A &ar) {}};
struct Derived : Base {template <typename A>void serialize(A &ar) {}};
union MyUnion { int a; float b; template <typename A>void serialize(A &ar) {}};

TEST_CASE("InputMetadata", "[Type][InputMetadata]") {
    SECTION("Simple Construction", "[Type][InputMetadata]") {
        REQUIRE(InputMetadata{InputMetadataT<int>{}}.isTriviallyCopyable);
        REQUIRE(InputMetadata{InputMetadataT<int>{}}.isFundamental);
        REQUIRE_FALSE(InputMetadata{InputMetadataT<std::vector<int>>{}}.isTriviallyCopyable);
    }

    SECTION("Simple Construction Stack Variable", "[Type][InputMetadata]") {
        int a{23};
        REQUIRE(InputMetadata{InputMetadataT<decltype(a)>{}}.isTriviallyCopyable);
        REQUIRE(InputMetadata{InputMetadataT<decltype(a)>{}}.isFundamental);
        REQUIRE_FALSE(InputMetadata{InputMetadataT<std::vector<decltype(a)>>{}}.isTriviallyCopyable);
    }

    SECTION("Fundamental Type", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<int>{}};
        REQUIRE(metadata.isFundamental);
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isMoveable);
        REQUIRE(metadata.isCopyable);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isArray);
        REQUIRE(metadata.sizeOfType == sizeof(int));
    }

    SECTION("Custom Class Type", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<CustomClass>{}};
        REQUIRE_FALSE(metadata.isFundamental);
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isMoveable);
        REQUIRE(metadata.isCopyable);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
    }

    SECTION("Polymorphic Type", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<Derived>{}};
        REQUIRE_FALSE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isPolymorphic);
    }

    SECTION("Function Pointer", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<void (*)()>{}};
        REQUIRE(metadata.isCallable);
        REQUIRE(metadata.isFunctionPointer);
    }

    SECTION("std::function", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<std::function<void()>>{}};
        REQUIRE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isFunctionPointer);
    }

    SECTION("Lambda Type", "[Type][InputMetadata]") {
        auto lambda = [](){};
        InputMetadata metadata{InputMetadataT<typeof(lambda)>{}};
        REQUIRE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isFunctionPointer);
    }

    SECTION("Non-Movable Type", "[Type][InputMetadata]") {
        struct NonMovable {
            NonMovable() = default;
            NonMovable(const NonMovable&) = default;
            NonMovable& operator=(const NonMovable&) = default;
            NonMovable(NonMovable&&) = delete;
            NonMovable& operator=(NonMovable&&) = delete;
        };
        InputMetadata metadata{InputMetadataT<NonMovable>{}};
        REQUIRE_FALSE(metadata.isMoveable);
    }

    SECTION("Array Type", "[Type][InputMetadata]") {
        int arr[5];
        InputMetadata metadata{InputMetadataT<int[5]>{}};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isArray);
        REQUIRE(metadata.arraySize == 5);
        REQUIRE(metadata.sizeOfType == sizeof(arr));
    }

    SECTION("Pointer Type", "[Type][InputMetadata]") {
        int* ptr = nullptr;
        InputMetadata metadata{InputMetadataT<int*>{}};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.sizeOfType == sizeof(int*));
    }

    SECTION("Type with Explicit Constructor and Destructor", "[Type][InputMetadata]") {
        struct ExplicitType {
            ExplicitType() {}
            ~ExplicitType() {}
        };
        InputMetadata metadata{InputMetadataT<ExplicitType>{}};
        REQUIRE(metadata.isDefaultConstructible);
        REQUIRE(metadata.isDestructible);
    }

    SECTION("STL Vector Type", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<std::vector<int>>{}};
        REQUIRE_FALSE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isMoveable);
        REQUIRE(metadata.isCopyable);
        REQUIRE(metadata.isDefaultConstructible);
        REQUIRE(metadata.isDestructible);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isArray);
    }

    SECTION("STL Map Type", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<std::map<int, std::string>>{}};
        REQUIRE_FALSE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isMoveable);
        REQUIRE(metadata.isCopyable);
        REQUIRE(metadata.isDefaultConstructible);
        REQUIRE(metadata.isDestructible);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isArray);
    }

    SECTION("Base Class with Virtual Function", "[Type][InputMetadata]") {
        InputMetadata metadataBase{InputMetadataT<Base>{}};
        REQUIRE_FALSE(metadataBase.isTriviallyCopyable);
        REQUIRE(metadataBase.isPolymorphic);
    }

    SECTION("Derived Class", "[Type][InputMetadata]") {
        InputMetadata metadataDerived{InputMetadataT<Derived>{}};
        REQUIRE_FALSE(metadataDerived.isTriviallyCopyable);
        REQUIRE(metadataDerived.isPolymorphic);
    }

    SECTION("Union Type", "[Type][InputMetadata]") {
        InputMetadata metadata{InputMetadataT<MyUnion>{}};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isArray);
        REQUIRE(metadata.sizeOfType == sizeof(MyUnion));
    }
}
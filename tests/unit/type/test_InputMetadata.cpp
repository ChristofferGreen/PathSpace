#include "ext/doctest.h"
#include <pathspace/type/InputMetadata.hpp>

#include <map>

using namespace SP;

struct CustomClass {};
struct Base { virtual ~Base() = default; template <typename A>void serialize(A &ar) {}};
struct Derived : Base {template <typename A>void serialize(A &ar) {}};
union MyUnion { int a; float b; template <typename A>void serialize(A &ar) {}};

TEST_CASE("InputMetadata") {
    SUBCASE("Simple Construction") {
        REQUIRE(InputMetadata{InputMetadataT<int>{}}.isTriviallyCopyable);
        REQUIRE(InputMetadata{InputMetadataT<int>{}}.isFundamental);
        REQUIRE_FALSE(InputMetadata{InputMetadataT<std::vector<int>>{}}.isTriviallyCopyable);
    }

    SUBCASE("Simple Construction Stack Variable") {
        int a{23};
        REQUIRE(InputMetadata{InputMetadataT<decltype(a)>{}}.isTriviallyCopyable);
        REQUIRE(InputMetadata{InputMetadataT<decltype(a)>{}}.isFundamental);
        REQUIRE_FALSE(InputMetadata{InputMetadataT<std::vector<decltype(a)>>{}}.isTriviallyCopyable);
    }

    SUBCASE("Fundamental Type") {
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

    SUBCASE("Custom Class Type") {
        InputMetadata metadata{InputMetadataT<CustomClass>{}};
        REQUIRE_FALSE(metadata.isFundamental);
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isMoveable);
        REQUIRE(metadata.isCopyable);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
    }

    SUBCASE("Polymorphic Type") {
        InputMetadata metadata{InputMetadataT<Derived>{}};
        REQUIRE_FALSE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isPolymorphic);
    }

    SUBCASE("Function Pointer") {
        InputMetadata metadata{InputMetadataT<void (*)()>{}};
        REQUIRE(metadata.isCallable);
        REQUIRE(metadata.isFunctionPointer);
    }

    SUBCASE("std::function") {
        InputMetadata metadata{InputMetadataT<std::function<void()>>{}};
        REQUIRE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isFunctionPointer);
    }

    SUBCASE("Lambda Type") {
        auto lambda = [](){};
        InputMetadata metadata{InputMetadataT<typeof(lambda)>{}};
        REQUIRE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isFunctionPointer);
    }

    SUBCASE("Non-Movable Type") {
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

    SUBCASE("Array Type") {
        int arr[5];
        InputMetadata metadata{InputMetadataT<int[5]>{}};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isArray);
        REQUIRE(metadata.arraySize == 5);
        REQUIRE(metadata.sizeOfType == sizeof(arr));
    }

    SUBCASE("Pointer Type") {
        int* ptr = nullptr;
        InputMetadata metadata{InputMetadataT<int*>{}};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.sizeOfType == sizeof(int*));
    }

    SUBCASE("Type with Explicit Constructor and Destructor") {
        struct ExplicitType {
            ExplicitType() {}
            ~ExplicitType() {}
        };
        InputMetadata metadata{InputMetadataT<ExplicitType>{}};
        REQUIRE(metadata.isDefaultConstructible);
        REQUIRE(metadata.isDestructible);
    }

    SUBCASE("STL Vector Type") {
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

    SUBCASE("STL Map Type") {
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

    SUBCASE("Base Class with Virtual Function") {
        InputMetadata metadataBase{InputMetadataT<Base>{}};
        REQUIRE_FALSE(metadataBase.isTriviallyCopyable);
        REQUIRE(metadataBase.isPolymorphic);
    }

    SUBCASE("Derived Class") {
        InputMetadata metadataDerived{InputMetadataT<Derived>{}};
        REQUIRE_FALSE(metadataDerived.isTriviallyCopyable);
        REQUIRE(metadataDerived.isPolymorphic);
    }

    SUBCASE("Union Type") {
        InputMetadata metadata{InputMetadataT<MyUnion>{}};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isArray);
        REQUIRE(metadata.sizeOfType == sizeof(MyUnion));
    }
}
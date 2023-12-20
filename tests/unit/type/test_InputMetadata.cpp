#include <catch2/catch_test_macros.hpp>
#include <pathspace/type/InputMetadata.hpp>

#include <map>

using namespace SP;

TEST_CASE("InputMetadata", "[Type][InputMetadata]") {
    SECTION("Simple Construction", "[Type][InputMetadata]") {
        REQUIRE(InputMetadata(int{}).isTriviallyCopyable);
        REQUIRE_FALSE(InputMetadata(std::vector<int>{}).isTriviallyCopyable);
    }

    SECTION("Fundamental Type", "[Type][InputMetadata]") {
        InputMetadata metadata{int{}};
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
        struct CustomClass {};
        InputMetadata metadata{CustomClass{}};
        REQUIRE_FALSE(metadata.isFundamental);
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isMoveable);
        REQUIRE(metadata.isCopyable);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
    }

    SECTION("Polymorphic Type", "[Type][InputMetadata]") {
        struct Base { virtual ~Base() = default; };
        struct Derived : Base {};
        InputMetadata metadata{Derived{}};
        REQUIRE_FALSE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isPolymorphic);
    }

    SECTION("Function Pointer", "[Type][InputMetadata]") {
        void (*funcPtr)() = []() {};
        InputMetadata metadata{funcPtr};
        REQUIRE(metadata.isCallable);
        REQUIRE(metadata.isFunctionPointer);
    }

    SECTION("Lambda Type", "[Type][InputMetadata]") {
        auto lambda = []() {};
        InputMetadata metadata{lambda};
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
        InputMetadata metadata{NonMovable{}};
        REQUIRE_FALSE(metadata.isMoveable);
    }

    SECTION("Array Type", "[Type][InputMetadata]") {
        int arr[5];
        InputMetadata metadata{arr};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.isArray);
        REQUIRE(metadata.arraySize == 5);
        REQUIRE(metadata.sizeOfType == sizeof(arr));
    }

    SECTION("Pointer Type", "[Type][InputMetadata]") {
        int* ptr = nullptr;
        InputMetadata metadata{ptr};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE(metadata.sizeOfType == sizeof(int*));
    }

    SECTION("Type with Explicit Constructor and Destructor", "[Type][InputMetadata]") {
        struct ExplicitType {
            ExplicitType() {}
            ~ExplicitType() {}
        };
        InputMetadata metadata{ExplicitType{}};
        REQUIRE(metadata.isDefaultConstructible);
        REQUIRE(metadata.isDestructible);
    }

    SECTION("STL Vector Type", "[Type][InputMetadata]") {
        InputMetadata metadata{std::vector<int>{}};
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
        InputMetadata metadata{std::map<int, std::string>{}};
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
        struct Base { virtual void func() {} };
        InputMetadata metadataBase{Base{}};
        REQUIRE_FALSE(metadataBase.isTriviallyCopyable);
        REQUIRE(metadataBase.isPolymorphic);
    }

    SECTION("Derived Class", "[Type][InputMetadata]") {
        struct Base { virtual void func() {} };
        struct Derived : Base {};
        InputMetadata metadataDerived{Derived{}};
        REQUIRE_FALSE(metadataDerived.isTriviallyCopyable);
        REQUIRE(metadataDerived.isPolymorphic);
    }

    SECTION("Union Type", "[Type][InputMetadata]") {
        union MyUnion { int a; float b; };
        InputMetadata metadata{MyUnion{}};
        REQUIRE(metadata.isTriviallyCopyable);
        REQUIRE_FALSE(metadata.isPolymorphic);
        REQUIRE_FALSE(metadata.isCallable);
        REQUIRE_FALSE(metadata.isArray);
        REQUIRE(metadata.sizeOfType == sizeof(MyUnion));
    }
}
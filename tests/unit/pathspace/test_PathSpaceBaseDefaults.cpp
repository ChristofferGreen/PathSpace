#include "third_party/doctest.h"

#include <pathspace/PathSpaceBase.hpp>

#include <array>
#include <span>
#include <string>

using namespace SP;

namespace {

class BaseStub final : public PathSpaceBase {
public:
    using PathSpaceBase::insert;
    using PathSpaceBase::read;
    using PathSpaceBase::take;
    using PathSpaceBase::typedPeekFuture;

    auto exposeRoot() -> Node* { return getRootNode(); }
    auto exposeRootConst() const -> Node* { return getRootNode(); }

private:
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "BaseStub out"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
};

} // namespace

TEST_SUITE("pathspace.base.defaults") {
TEST_CASE("PathSpaceBase defaults return empty children and no futures") {
    BaseStub stub;

    auto children = stub.read<Children>(ConcretePathStringView{"/"});
    REQUIRE(children.has_value());
    CHECK(children->names.empty());

    CHECK_FALSE(stub.typedPeekFuture("/any").has_value());
    CHECK(stub.exposeRoot() == nullptr);

    auto const& constStub = stub;
    CHECK(constStub.exposeRootConst() == nullptr);
}

TEST_CASE("ValueHandle defaults are empty and report missing node") {
    ValueHandle handle{};
    CHECK_FALSE(handle.valid());
    CHECK_FALSE(handle.hasValues());
    CHECK(handle.queueDepth() == 0);

    auto copy = handle;
    CHECK_FALSE(copy.valid());
    auto moved = std::move(copy);
    CHECK_FALSE(moved.valid());

    auto snapshot = handle.snapshot();
    CHECK_FALSE(snapshot.has_value());
    CHECK(snapshot.error().code == Error::Code::UnknownError);
}

TEST_CASE("PathSpaceBase visit rejects empty visitors") {
    BaseStub stub;

    VisitOptions options;
    auto result = stub.visit(PathVisitor{}, options);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidType);
}

TEST_CASE("PathSpaceBase visit reports NotSupported when no root node exists") {
    BaseStub stub;

    VisitOptions options;
    auto result = stub.visit([](PathEntry const&, ValueHandle&) {
        return VisitControl::Continue;
    }, options);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpaceBase read rejects invalid concrete child paths") {
    BaseStub stub;

    auto invalid = stub.read<Children>(ConcretePathStringView{"/bad//path"});
    CHECK_FALSE(invalid.has_value());

    auto glob = stub.read<Children>(ConcretePathStringView{"/bad/*"});
    CHECK_FALSE(glob.has_value());
}

TEST_CASE("PathSpaceBase span-pack insert validates base path and component names") {
    BaseStub stub;
    std::array<int, 1> a{1};
    std::array<int, 1> b{2};

    auto badBase = stub.insert<"a", "b">("relative", std::span<const int>(a), std::span<const int>(b));
    CHECK_FALSE(badBase.errors.empty());

    auto badName = stub.insert<"..", "b">("/root", std::span<const int>(a), std::span<const int>(b));
    CHECK_FALSE(badName.errors.empty());
}

TEST_CASE("PathSpaceBase span-pack read validates paths and reports unsupported spans") {
    BaseStub stub;

    auto badBase = stub.read<"a", "b">("relative", [](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(badBase.has_value());

    auto badName = stub.read<"..", "b">("/root", [](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(badName.has_value());

    auto unsupported = stub.read<"a", "b">("/root", [](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(unsupported.has_value());
    CHECK(unsupported.error().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpaceBase span read validates single paths") {
    BaseStub stub;

    auto badRead = stub.read("relative", [](std::span<const int>) {});
    CHECK_FALSE(badRead.has_value());
}

TEST_CASE("PathSpaceBase span-pack take validates paths and reports unsupported spans") {
    BaseStub stub;

    auto badBase = stub.take<"a", "b">("relative", [](std::span<int>, std::span<int>) {});
    CHECK_FALSE(badBase.has_value());

    auto badName = stub.take<"..", "b">("/root", [](std::span<int>, std::span<int>) {});
    CHECK_FALSE(badName.has_value());

    auto unsupported = stub.take<"a", "b">("/root", [](std::span<int>, std::span<int>) {});
    CHECK_FALSE(unsupported.has_value());
    CHECK(unsupported.error().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpaceBase span take validates single paths") {
    BaseStub stub;

    auto badTake = stub.take("relative", [](std::span<int>) {});
    CHECK_FALSE(badTake.has_value());
}
}

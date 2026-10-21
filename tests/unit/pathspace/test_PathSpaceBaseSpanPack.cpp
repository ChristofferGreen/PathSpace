#include "third_party/doctest.h"

#include <pathspace/PathSpaceBase.hpp>

#include <span>

using namespace SP;

namespace {
struct DummySpace final : PathSpaceBase {
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "out not supported"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
};
} // namespace

TEST_SUITE("pathspace.base.span_pack") {
TEST_CASE("read span pack returns NotSupported by default") {
    DummySpace space;
    auto result = space.read<"alpha", "beta">(
        "/root",
        [](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotSupported);
}

TEST_CASE("take span pack returns NotSupported by default") {
    DummySpace space;
    auto result = space.take<"alpha", "beta">(
        "/root",
        [](std::span<int>, std::span<int>) {});
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotSupported);
}
} // TEST_SUITE

#include "third_party/doctest.h"

#define private public
#include <pathspace/PathSpaceBase.hpp>
#undef private

#include <array>
#include <memory>
#include <span>
#include <string>
#include "core/Node.hpp"
#include "core/PathSpaceContext.hpp"

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
    auto callSpanPackConst(std::span<const std::string> paths,
                           InputMetadata const& metadata,
                           Out const& options) const -> Expected<void> {
        return spanPackConst(paths, metadata, options, [](std::span<RawConstSpan const>) { return std::nullopt; });
    }
    auto callSpanPackMut(std::span<const std::string> paths,
                         InputMetadata const& metadata,
                         Out const& options) const -> Expected<void> {
        return spanPackMut(paths, metadata, options, [](std::span<RawMutSpan const>) {
            return SpanPackResult{.error = std::nullopt, .shouldPop = false};
        });
    }

private:
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "BaseStub out"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
};

class SinkProbe final : public PathSpaceBase {
public:
    using PathSpaceBase::adoptContextAndPrefix;
    using PathSpaceBase::getNotificationSink;

    int         notifyCount = 0;
    std::string lastNotification;

private:
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return std::nullopt;
    }
    auto shutdown() -> void override {}
    auto notify(std::string const& notificationPath) -> void override {
        ++notifyCount;
        lastNotification = notificationPath;
    }
};

struct RecordingSink final : NotificationSink {
    void notify(const std::string& notificationPath) override {
        lastNotification = notificationPath;
    }

    std::string lastNotification;
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

TEST_CASE("ValueHandle snapshot returns empty data for bare nodes") {
    BaseStub stub;
    Node     node;

    auto handle = stub.makeValueHandle(node, "/bare", true);
    auto snap = handle.snapshot();
    REQUIRE(snap);
    CHECK(snap->queueDepth == 0);
    CHECK(snap->types.empty());
    CHECK_FALSE(snap->hasExecutionPayload);
    CHECK_FALSE(snap->hasSerializedPayload);
    CHECK(snap->rawBufferBytes == 0);
}

TEST_CASE("PathSpaceBase creates default notification sink without context") {
    SinkProbe probe;

    auto weak = probe.getNotificationSink();
    auto sink = weak.lock();
    REQUIRE(sink);

    sink->notify("/ping");
    CHECK(probe.notifyCount == 1);
    CHECK(probe.lastNotification == "/ping");

    auto weakAgain = probe.getNotificationSink();
    CHECK(weakAgain.lock() == sink);
}

TEST_CASE("PathSpaceBase sink ignores notifications after owner destruction") {
    std::shared_ptr<NotificationSink> sink;
    {
        SinkProbe probe;
        auto weak = probe.getNotificationSink();
        sink = weak.lock();
        REQUIRE(sink);
    }

    CHECK_NOTHROW(sink->notify("/late"));
}

TEST_CASE("PathSpaceBase context sink ignores notifications after owner destruction") {
    std::shared_ptr<NotificationSink> sink;
    auto ctx = std::make_shared<PathSpaceContext>();
    {
        SinkProbe probe;
        probe.adoptContextAndPrefix(ctx, "/root");
        auto weak = probe.getNotificationSink();
        sink = weak.lock();
        REQUIRE(sink);
    }

    CHECK_NOTHROW(sink->notify("/late"));
}

TEST_CASE("PathSpaceBase installs default sink into shared context when needed") {
    SinkProbe probe;

    auto ctx = std::make_shared<PathSpaceContext>();
    probe.adoptContextAndPrefix(ctx, "/root");

    auto weak = probe.getNotificationSink();
    auto sink = weak.lock();
    REQUIRE(sink);

    sink->notify("/ctx");
    CHECK(probe.notifyCount == 1);
    CHECK(probe.lastNotification == "/ctx");

    auto weakAgain = probe.getNotificationSink();
    CHECK(weakAgain.lock() == sink);
}

TEST_CASE("PathSpaceBase reuses existing context sink") {
    SinkProbe probe;
    auto ctx = std::make_shared<PathSpaceContext>();
    auto recorder = std::make_shared<RecordingSink>();
    ctx->setSink(recorder);
    probe.adoptContextAndPrefix(ctx, "/root");

    auto weak = probe.getNotificationSink();
    auto sink = weak.lock();
    REQUIRE(sink);
    CHECK(sink.get() == recorder.get());

    sink->notify("/direct");
    CHECK(recorder->lastNotification == "/direct");
    CHECK(probe.notifyCount == 0);
}

TEST_CASE("PathSpaceBase visit rejects empty visitors") {
    BaseStub stub;

    VisitOptions options;
    auto result = stub.visit(PathVisitor{}, options);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidType);
}

TEST_CASE("PathSpaceBase span pack defaults report not supported") {
    BaseStub stub;

    auto readResult = stub.read<"alpha", "beta">("/root",
                                                 [](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(readResult.has_value());
    CHECK(readResult.error().code == Error::Code::NotSupported);

    auto takeResult = stub.take<"alpha", "beta">("/root",
                                                 [](std::span<int>, std::span<int>) {});
    CHECK_FALSE(takeResult.has_value());
    CHECK(takeResult.error().code == Error::Code::NotSupported);

    std::array<std::string, 2> paths{"/root/alpha", "/root/beta"};
    InputMetadata metadata{InputMetadataT<int>{}};
    auto directRead = stub.callSpanPackConst(std::span<const std::string>(paths.data(), paths.size()),
                                             metadata,
                                             Out{});
    CHECK_FALSE(directRead.has_value());
    CHECK(directRead.error().code == Error::Code::NotSupported);

    auto directTake = stub.callSpanPackMut(std::span<const std::string>(paths.data(), paths.size()),
                                           metadata,
                                           Out{});
    CHECK_FALSE(directTake.has_value());
    CHECK(directTake.error().code == Error::Code::NotSupported);
}

TEST_CASE("VisitOptions child limit helpers reflect settings") {
    VisitOptions defaults;
    CHECK(defaults.childLimitEnabled());
    CHECK(VisitOptions::isUnlimitedChildren(VisitOptions::UnlimitedChildren));

    VisitOptions unlimited = defaults;
    unlimited.maxChildren = VisitOptions::UnlimitedChildren;
    CHECK_FALSE(unlimited.childLimitEnabled());
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

TEST_CASE("PathSpaceBase span pack defaults report NotSupported") {
    BaseStub stub;

    InputMetadata meta{InputMetadataT<int>{}};
    std::array<std::string, 1> paths{"/alpha"};
    Out options;

    auto constResult = stub.spanPackConst(std::span<const std::string>(paths.data(), paths.size()),
                                          meta,
                                          options,
                                          [](std::span<RawConstSpan const>) -> std::optional<Error> {
                                              return std::nullopt;
                                          });
    CHECK_FALSE(constResult.has_value());
    CHECK(constResult.error().code == Error::Code::NotSupported);

    auto mutResult = stub.spanPackMut(std::span<const std::string>(paths.data(), paths.size()),
                                      meta,
                                      options,
                                      [](std::span<RawMutSpan const>) -> SpanPackResult {
                                          return SpanPackResult{.error = std::nullopt, .shouldPop = false};
                                      });
    CHECK_FALSE(mutResult.has_value());
    CHECK(mutResult.error().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpaceBase span-pack defaults report NotSupported") {
    BaseStub stub;

    std::array<std::string, 1> paths{"/a"};
    auto constResult = stub.spanPackConst(
        std::span<const std::string>(paths),
        InputMetadata{},
        Out{},
        [&](std::span<RawConstSpan const>) { return std::optional<Error>{}; });
    CHECK_FALSE(constResult);
    CHECK(constResult.error().code == Error::Code::NotSupported);

    auto mutResult = stub.spanPackMut(
        std::span<const std::string>(paths),
        InputMetadata{},
        Out{},
        [&](std::span<RawMutSpan const>) { return SpanPackResult{}; });
    CHECK_FALSE(mutResult);
    CHECK(mutResult.error().code == Error::Code::NotSupported);
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

TEST_CASE("PathSpaceBase span-pack insert rejects empty component names") {
    BaseStub stub;
    std::array<int, 1> a{1};
    std::array<int, 1> b{2};

    auto emptyName = stub.insert<"", "b">("/root", std::span<const int>(a), std::span<const int>(b));
    CHECK_FALSE(emptyName.errors.empty());
    CHECK(emptyName.errors.front().code == Error::Code::InvalidPath);
}

TEST_CASE("PathSpaceBase span-pack insert returns NotSupported by default") {
    BaseStub stub;
    std::array<int, 2> a{1, 2};
    std::array<int, 2> b{3, 4};

    auto result = stub.insert<"a", "b">("/root", std::span<const int>(a), std::span<const int>(b));
    CHECK_FALSE(result.errors.empty());
    CHECK(result.errors.front().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpaceBase pack insert returns NotSupported by default") {
    BaseStub stub;

    auto result = stub.insert<"/alpha", "/beta">(1, 2);
    CHECK_FALSE(result.errors.empty());
    CHECK(result.errors.front().code == Error::Code::NotSupported);
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

TEST_CASE("PathSpaceBase span-pack read rejects empty component names") {
    BaseStub stub;

    auto badName = stub.read<"", "b">("/root",
                                     [](std::span<const int>, std::span<const int>) {},
                                     OutFullValidation{});
    CHECK_FALSE(badName.has_value());
    CHECK(badName.error().code == Error::Code::InvalidPath);
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

TEST_CASE("PathSpaceBase span-pack take rejects empty component names") {
    BaseStub stub;

    auto badName = stub.take<"", "b">("/root",
                                     [](std::span<int>, std::span<int>) {},
                                     OutFullValidation{});
    CHECK_FALSE(badName.has_value());
    CHECK(badName.error().code == Error::Code::InvalidPath);
}

TEST_CASE("PathSpaceBase take<T> rejects invalid paths") {
    BaseStub stub;

    auto invalid = stub.take<int>("relative");
    CHECK_FALSE(invalid.has_value());
}

TEST_CASE("PathSpaceBase span take validates single paths") {
    BaseStub stub;

    auto badTake = stub.take("relative", [](std::span<int>) {});
    CHECK_FALSE(badTake.has_value());
}
}

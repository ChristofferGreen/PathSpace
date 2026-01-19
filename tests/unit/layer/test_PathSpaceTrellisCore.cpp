#include "layer/PathSpaceTrellis.hpp"
#include "PathSpace.hpp"
#include "path/Iterator.hpp"
#include "core/In.hpp"
#include "core/PathSpaceContext.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"
#include "type/InputData.hpp"
#include "third_party/doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using namespace SP;

TEST_SUITE("layer.pathspace.trellis.core") {
TEST_CASE("PathSpaceTrellis handles missing backing") {
    PathSpaceTrellis trellis{nullptr};

    auto insert = trellis.in(Iterator{"/value"}, InputData{42});
    REQUIRE_FALSE(insert.errors.empty());
    CHECK(insert.errors[0].code == Error::Code::InvalidPermissions);

    int outValue = 0;
    auto err = trellis.out(Iterator{"/value"}, InputMetadataT<int>{}, Out{}, &outValue);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidPermissions);
}

TEST_CASE("PathSpaceTrellis enable/disable and fan-out read") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Enable a source
    auto enable = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}});
    REQUIRE(enable.errors.empty());

    // Insert a value at the enabled source
    auto ins = backing->insert("/foo", 123);
    REQUIRE(ins.errors.empty());

    int outValue = 0;
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &outValue);
    CHECK_FALSE(err.has_value());
    CHECK(outValue == 123);

    // Disable the source, subsequent read should fail with NoObjectFound
    auto disable = trellis.in(Iterator{"/_system/disable"}, InputData{std::string{"/foo"}});
    REQUIRE(disable.errors.empty());

    err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &outValue);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("PathSpaceTrellis system command validation") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Missing command segment
    auto insert = trellis.in(Iterator{"/_system"}, InputData{std::string{"x"}});
    REQUIRE_FALSE(insert.errors.empty());
    CHECK(insert.errors[0].code == Error::Code::InvalidPath);

    // Unknown command
    auto unknown = trellis.in(Iterator{"/_system/reload"}, InputData{std::string{"/foo"}});
    REQUIRE_FALSE(unknown.errors.empty());
    CHECK(unknown.errors[0].code == Error::Code::InvalidPath);
}

TEST_CASE("PathSpaceTrellis rejects non-string system payloads and empty sources") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Root insert with no sources should surface NoObjectFound.
    auto rootIns = trellis.in(Iterator{"/"}, InputData{5});
    REQUIRE_FALSE(rootIns.errors.empty());
    CHECK(rootIns.errors[0].code == Error::Code::NoObjectFound);

    // System command with non-string payload is invalid.
    auto badEnable = trellis.in(Iterator{"/_system/enable"}, InputData{123});
    REQUIRE_FALSE(badEnable.errors.empty());
    CHECK((badEnable.errors[0].code == Error::Code::InvalidType || badEnable.errors[0].code == Error::Code::InvalidPath));
}

TEST_CASE("PathSpaceTrellis blocks span reads at root") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    InputMetadata meta{InputMetadataT<int>{}};
    meta.spanReader = [](void const*, std::size_t) {};

    auto err = trellis.out(Iterator{"/"}, meta, Out{}, nullptr);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NotSupported);
}

namespace {
struct RecordingPathSpaceCore : PathSpace {
    using PathSpace::PathSpace;
    void notify(std::string const& notificationPath) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            notifications.push_back(notificationPath);
        }
        PathSpace::notify(notificationPath);
    }
    std::vector<std::string> flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto copy = notifications;
        notifications.clear();
        return copy;
    }
    std::mutex              mutex_;
    std::vector<std::string> notifications;
};

struct AlwaysEmptyPathSpace : PathSpace {
    using PathSpace::PathSpace;
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NoObjectFound, "empty"};
    }
};

struct ScriptedOutPathSpace : PathSpace {
    using PathSpace::PathSpace;
    std::vector<std::optional<Error>> script;
    mutable std::size_t               cursor{0};

    auto out(Iterator const&,
             InputMetadata const&,
             Out const&,
             void*) -> std::optional<Error> override {
        if (cursor < script.size()) {
            return script[cursor++];
        }
        return Error{Error::Code::NoSuchPath, "script exhausted"};
    }
};

struct ScriptedInsertPathSpace : PathSpace {
    using PathSpace::PathSpace;
    InsertReturn               response;
    std::vector<std::string>   receivedPaths;

    auto in(Iterator const& path, InputData const&) -> InsertReturn override {
        receivedPaths.push_back(path.toString());
        return response;
    }
};

struct ScriptedVisitPathSpace : PathSpace {
    using PathSpace::PathSpace;
    std::vector<PathEntry> entries;

    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override {
        (void)options;
        for (auto const& entry : entries) {
            ValueHandle handle{};
            auto        control = visitor(entry, handle);
            if (control == VisitControl::Stop) {
                break;
            }
            if (control == VisitControl::SkipChildren) {
                continue;
            }
        }
        return {};
    }
};

struct NullFuturePathSpace : PathSpace {
    using PathSpace::PathSpace;
    auto typedPeekFuture(std::string_view) const -> std::optional<FutureAny> override { return std::nullopt; }
};

struct RecordingOutPathSpace : PathSpace {
    using PathSpace::PathSpace;
    std::vector<std::string> recorded;
    std::optional<Error>     result;

    auto out(Iterator const& path, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        recorded.push_back(path.toString());
        return result;
    }
};

struct FutureScriptedPathSpace : PathSpace {
    using PathSpace::PathSpace;
    std::vector<std::optional<FutureAny>> futures;
    mutable std::size_t                    cursor{0};

    auto typedPeekFuture(std::string_view) const -> std::optional<FutureAny> override {
        if (cursor >= futures.size()) {
            return std::nullopt;
        }
        return futures[cursor++];
    }
};

} // namespace

TEST_CASE("PathSpaceTrellis rejects nested system command path") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    auto ret = trellis.in(Iterator{"/_system/enable/extra"}, InputData{std::string{"/foo"}});
    REQUIRE_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidPath);
    CHECK(ret.nbrValuesInserted == 0);
    CHECK(ret.nbrSpacesInserted == 0);
}

TEST_CASE("PathSpaceTrellis root read without sources returns NoObjectFound") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    int out = 0;
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("PathSpaceTrellis move-only insert routes to single source") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/b"}});

    std::unique_ptr<PathSpaceBase> nested{std::make_unique<PathSpace>().release()};
    InputData nestedInput{nested};
    auto     ret = trellis.in(Iterator{"/"}, nestedInput);

    CHECK(ret.errors.empty());
    CHECK(ret.nbrSpacesInserted == 1);
    CHECK(nested == nullptr);

    auto kidsOpt = backing->read<Children>("/");
    REQUIRE(kidsOpt.has_value());
    auto kids = kidsOpt->names;
    std::sort(kids.begin(), kids.end());
    CHECK(kids.size() == 1);
    CHECK((kids[0] == "a" || kids[0] == "b"));
}

TEST_CASE("PathSpaceTrellis blocking root read exits on shutdown") {
    auto backing = std::make_shared<AlwaysEmptyPathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}});

    trellis.shutdown();

    int out = 0;
    Out blocking = Block(std::chrono::milliseconds{5});
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, blocking, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::Timeout);
    REQUIRE(err->message.has_value());
    CHECK(err->message->find("shutting down") != std::string::npos);
}

TEST_CASE("PathSpaceTrellis notify fan-out and system ignores") {
    auto backing = std::make_shared<RecordingPathSpaceCore>();
    PathSpaceTrellis trellis{backing};

    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/data/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/data/b"}});

    trellis.notify("/");
    trellis.notify("/_system");

    auto notes = backing->flush();
    std::sort(notes.begin(), notes.end());
    CHECK(notes == std::vector<std::string>{"/data/a", "/data/b"});
}

TEST_CASE("PathSpaceTrellis notify with empty path fans out to all sources") {
    auto backing = std::make_shared<RecordingPathSpaceCore>();
    PathSpaceTrellis trellis{backing};

    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/data/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/data/b"}});

    trellis.notify(""); // empty notification path should behave like root fan-out

    auto notes = backing->flush();
    std::sort(notes.begin(), notes.end());
    CHECK(notes == std::vector<std::string>{"/data/a", "/data/b"});
}

TEST_CASE("PathSpaceTrellis notify and join/strip helpers") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Register one source and verify notify fan-out ignores system paths.
    auto enable = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/mount/a"}}); // already canonical
    REQUIRE(enable.errors.empty());

    trellis.notify("/");
    trellis.notify("/_system"); // should be ignored silently
}

TEST_CASE("PathSpaceTrellis typedPeekFuture maps mount prefix and hides system paths") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}});

    std::atomic<int> executed{0};
    In options{.executionCategory = ExecutionCategory::Lazy};
    auto ret = backing->insert("/foo", [&]() {
        executed.fetch_add(1, std::memory_order_relaxed);
        return 9;
    }, options);
    REQUIRE(ret.errors.empty());

    auto futAny = trellis.read("/foo");
    REQUIRE(futAny.has_value());
    CHECK(futAny->valid());
    if (futAny->ready()) {
        int result = 0;
        REQUIRE(futAny->copy_to(&result));
        CHECK(result == 9);
    }
    CHECK(executed.load() >= 0);

    auto sysFuture = trellis.read("/_system");
    CHECK_FALSE(sysFuture.has_value());

    PathSpaceTrellis empty{backing};
    auto missing = empty.read("/");
    CHECK_FALSE(missing.has_value());
}

TEST_CASE("PathSpaceTrellis listChildren hides _system and joins mount") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.adoptContextAndPrefix(std::make_shared<PathSpaceContext>(), "/mounted");

    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/mounted/data"}});
    backing->insert("/mounted/data/a", 1);
    backing->insert("/mounted/data/b", 2);
    backing->insert("/mounted/_system/private", 3);

    auto kids = trellis.listChildrenCanonical("/data");
    std::sort(kids.begin(), kids.end());
    CHECK(kids == std::vector<std::string>{"a", "b"});

    auto sysKids = trellis.listChildrenCanonical("/_system");
    CHECK(sysKids.empty());

    int dummy = 0;
    auto err = trellis.out(Iterator{"/_system"}, InputMetadataT<int>{}, Out{}, &dummy);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidPermissions);
}

TEST_CASE("PathSpaceTrellis mount prefix handles trailing slashes and canonicalization") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.adoptContextAndPrefix(std::make_shared<PathSpaceContext>(), "/root/");

    // Enable source with extra slash to hit canonicalize() path.
    auto enable = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/root//data"}}); // malformed: double slash
    CHECK_FALSE(enable.errors.empty());
    CHECK(enable.errors.front().code == Error::Code::InvalidPath);
    CHECK(trellis.debugSources().empty());

    // Enable valid source and verify fan-out succeeds.
    auto enableGood = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/root/data"}});
    REQUIRE(enableGood.errors.empty());

    auto ins = backing->insert("/root/data", 321);
    REQUIRE(ins.errors.empty());

    int out = 0;
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &out);
    CHECK_FALSE(err.has_value());
    CHECK(out == 321);

    // Disable with malformed path should no-op but not crash.
    auto disableBad = trellis.in(Iterator{"/_system/disable"}, InputData{std::string{"relative/path"}});
    CHECK(disableBad.errors.empty());
    CHECK(trellis.debugSources() == std::vector<std::string>{"/root/data"});

    // Proper disable removes the source.
    auto disable = trellis.in(Iterator{"/_system/disable"}, InputData{std::string{"/root/data"}});
    CHECK(disable.errors.empty());
    CHECK(trellis.debugSources().empty());
}

TEST_CASE("PathSpaceTrellis visit rejects missing backing") {
    PathSpaceTrellis trellis{nullptr};
    auto result = trellis.visit([](PathEntry const&, ValueHandle&) { return VisitControl::Continue; });
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("PathSpaceTrellis duplicate enable and missing disable are no-ops") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    auto first = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}});
    REQUIRE(first.errors.empty());
    auto second = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}});
    CHECK(second.errors.empty());
    CHECK(trellis.debugSources() == std::vector<std::string>{"/foo"});

    auto missingDisable = trellis.in(Iterator{"/_system/disable"}, InputData{std::string{"/bar"}});
    CHECK(missingDisable.errors.empty());
    CHECK(trellis.debugSources() == std::vector<std::string>{"/foo"});
}

TEST_CASE("PathSpaceTrellis fan-out reports sources ready vs unavailable") {
    auto backing = std::make_shared<ScriptedOutPathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/b"}});

    backing->script = {Error{Error::Code::NoObjectFound, "empty"}, Error{Error::Code::Timeout, "slow"}};

    int outValue = 0;
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &outValue);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
    REQUIRE(err->message.has_value());
    CHECK(err->message->find("ready") != std::string::npos);
}

TEST_CASE("PathSpaceTrellis blocking fan-out times out without shutdown") {
    auto backing = std::make_shared<AlwaysEmptyPathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}}); // register one source

    int outValue = 0;
    Out blocking = Block(std::chrono::milliseconds{3});
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, blocking, &outValue);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::Timeout);
    REQUIRE(err->message.has_value());
    CHECK(err->message->find("timed out") != std::string::npos);
}

TEST_CASE("PathSpaceTrellis normalize mount prefix when joining notifications") {
    auto backing = std::make_shared<RecordingPathSpaceCore>();
    PathSpaceTrellis trellis{backing};
    trellis.adoptContextAndPrefix(std::make_shared<PathSpaceContext>(), "/root/");
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/root/child"}});

    trellis.notify("/child");
    auto notes = backing->flush();
    REQUIRE(notes.size() == 1);
    CHECK(notes.front() == "/root/child");
}

TEST_CASE("PathSpaceTrellis visit remaps prefix and skips system nodes") {
    auto backing = std::make_shared<ScriptedVisitPathSpace>();
    backing->entries = {
        PathEntry{"/root", true, false, false, 0, DataCategory::None},
        PathEntry{"/root/_system/private", true, false, false, 0, DataCategory::None},
        PathEntry{"/root/child", false, true, false, 0, DataCategory::Fundamental},
        PathEntry{"/rootchild", false, true, false, 0, DataCategory::Fundamental},
        PathEntry{"/other", false, true, false, 0, DataCategory::Fundamental},
    };

    PathSpaceTrellis trellis{backing};
    trellis.adoptContextAndPrefix(std::make_shared<PathSpaceContext>(), "/root");

    std::vector<std::string> visited;
    auto result = trellis.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.push_back(entry.path);
        return VisitControl::Continue;
    });

    REQUIRE(result.has_value());
    CHECK(visited == std::vector<std::string>{"/", "/child", "/child", "/other"});
}

TEST_CASE("PathSpaceTrellis notify is a no-op without backing") {
    PathSpaceTrellis trellis{nullptr};
    REQUIRE_NOTHROW(trellis.notify("/"));
    REQUIRE_NOTHROW(trellis.notify("/anything"));
}

TEST_CASE("PathSpaceTrellis system payload variants include null handling") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    InputData nullPayload{static_cast<void const*>(nullptr), InputMetadata{}};
    auto      nullResult = trellis.in(Iterator{"/_system/enable"}, nullPayload);
    REQUIRE_FALSE(nullResult.errors.empty());
    CHECK(nullResult.errors.front().code == Error::Code::InvalidType);

    char const* cstr = "/cstr";
    InputMetadata cstrMeta{};
    cstrMeta.typeInfo = &typeid(char const*);
    auto cstrRet = trellis.in(Iterator{"/_system/enable"}, InputData{static_cast<void const*>(cstr), cstrMeta});
    REQUIRE(cstrRet.errors.empty());

    std::string_view sv{"/sv"};
    InputMetadata svMeta{};
    svMeta.typeInfo = &typeid(std::string_view);
    auto svRet = trellis.in(Iterator{"/_system/enable"}, InputData{&sv, svMeta});
    REQUIRE(svRet.errors.empty());

    auto sources = trellis.debugSources();
    std::sort(sources.begin(), sources.end());
    CHECK(sources == std::vector<std::string>{"/cstr", "/sv"});
}

TEST_CASE("PathSpaceTrellis accepts mutable char* system payloads") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    char buffer[] = "/mutable";
    InputMetadata meta{};
    meta.typeInfo = &typeid(char*);
    auto ret      = trellis.in(Iterator{"/_system/enable"}, InputData{static_cast<void const*>(buffer), meta});
    REQUIRE(ret.errors.empty());

    auto sources = trellis.debugSources();
    REQUIRE(sources.size() == 1);
    CHECK(sources.front() == "/mutable");
}

TEST_CASE("PathSpaceTrellis root insert fans out and merges retargets/errors") {
    auto backing = std::make_shared<ScriptedInsertPathSpace>();
    backing->response.nbrValuesInserted = 1;
    backing->response.nbrSpacesInserted = 1;
    backing->response.retargets.push_back({nullptr, "/retarget"});
    backing->response.errors.push_back(Error{Error::Code::InvalidPath, "bad"});

    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/b"}});

    auto ret = trellis.in(Iterator{"/"}, InputData{42});
    CHECK(ret.nbrValuesInserted == 2);
    CHECK(ret.nbrSpacesInserted == 2);
    CHECK(ret.retargets.size() == 2);
    CHECK(ret.errors.size() == 2);

    auto paths = backing->receivedPaths;
    std::sort(paths.begin(), paths.end());
    CHECK(paths == std::vector<std::string>{"/a", "/b"});
}

TEST_CASE("PathSpaceTrellis blocking fan-out succeeds after retry") {
    auto backing = std::make_shared<ScriptedOutPathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/retry"}}); // single source

    backing->script = {Error{Error::Code::NoObjectFound, "empty"}, std::nullopt};

    int out = 0;
    Out blocking = Block(std::chrono::milliseconds{10});
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, blocking, &out);
    CHECK_FALSE(err.has_value());
}

TEST_CASE("PathSpaceTrellis blocking fan-out returns non-empty error after wait") {
    auto backing = std::make_shared<ScriptedOutPathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/retry"}}); // single source

    backing->script = {Error{Error::Code::NoObjectFound, "empty"}, Error{Error::Code::InvalidPermissions, "denied"}};

    int out = 0;
    Out blocking = Block(std::chrono::milliseconds{5});
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, blocking, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidPermissions);
}

TEST_CASE("PathSpaceTrellis blocking read surfaces immediate non-empty errors") {
    auto backing = std::make_shared<ScriptedOutPathSpace>();
    backing->script = {Error{Error::Code::InvalidType, "bad"}};

    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/source"}});

    int out = 0;
    Out blocking = Block(std::chrono::milliseconds{5});
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, blocking, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidType);
}

TEST_CASE("PathSpaceTrellis tryFanOutFuture scans multiple sources") {
    PromiseT<int> promise;
    promise.set_value(7);
    FutureAny futureAny{promise.get_future()};

    auto backing = std::make_shared<FutureScriptedPathSpace>();
    backing->futures = {std::nullopt, futureAny};

    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/b"}});

    auto fut = trellis.read("/", Out{});
    REQUIRE(fut.has_value());
    int value = 0;
    REQUIRE(fut->copy_to(&value));
    CHECK(value == 7);
}

TEST_CASE("PathSpaceTrellis typed peek without backing yields no future") {
    PathSpaceTrellis trellis{nullptr};
    auto fut = trellis.read("/", Out{});
    REQUIRE_FALSE(fut.has_value());
    CHECK(fut.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("PathSpaceTrellis fan-out future returns missing when sources idle") {
    auto backing = std::make_shared<NullFuturePathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/b"}});

    auto fut = trellis.read("/", Out{});
    REQUIRE_FALSE(fut.has_value());
    CHECK(fut.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("PathSpaceTrellis joinWithMount normalizes double and missing slashes") {
    auto backing = std::make_shared<RecordingOutPathSpace>();
    PathSpaceTrellis trellisSlash{backing};
    trellisSlash.adoptContextAndPrefix(std::make_shared<PathSpaceContext>(), "/root/");

    int dummy = 0;
    auto errSlash = trellisSlash.out(Iterator{"/child"}, InputMetadataT<int>{}, Out{}, &dummy);
    CHECK_FALSE(errSlash.has_value());
    CHECK(backing->recorded.back() == "/root/child");

    PathSpaceTrellis trellisNoSlash{backing};
    trellisNoSlash.adoptContextAndPrefix(std::make_shared<PathSpaceContext>(), "/root");
    auto errNoSlash = trellisNoSlash.out(Iterator{"child"}, InputMetadataT<int>{}, Out{}, &dummy);
    CHECK_FALSE(errNoSlash.has_value());
    CHECK(backing->recorded.back() == "/root/child");
}

TEST_CASE("PathSpaceTrellis listChildrenCanonical handles empty tail with mount prefix") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.adoptContextAndPrefix(std::make_shared<PathSpaceContext>(), "/root");

    auto kids = trellis.listChildrenCanonical("");
    CHECK(kids.empty());
}

TEST_CASE("PathSpaceTrellis handles fallback string payloads with unknown type info") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    InputMetadata meta{};
    meta.typeInfo = &typeid(int); // forces extractStringPayload fallback branch

    auto ret = trellis.in(Iterator{"/_system/enable"}, InputData{static_cast<void const*>("/fallback"), meta});
    REQUIRE(ret.errors.empty());

    auto sources = trellis.debugSources();
    REQUIRE(sources.size() == 1);
    CHECK(sources.front() == "/fallback");
}

TEST_CASE("PathSpaceTrellis listChildrenCanonical tolerates invalid canonical paths") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Relative path should fail canonicalization inside the backing read and return empty.
    auto kids = trellis.listChildrenCanonical("relative/path");
    CHECK(kids.empty());
}

TEST_CASE("PathSpaceTrellis fan-out future returns root future via read") {
    auto backing = std::make_shared<FutureScriptedPathSpace>();
    PromiseT<int> promise;
    promise.set_value(5);
    backing->futures = {FutureAny{promise.get_future()}};

    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}});

    auto fut = trellis.read("/", Out{});
    REQUIRE(fut.has_value());
    int value = 0;
    REQUIRE(fut->copy_to(&value));
    CHECK(value == 5);
}

TEST_CASE("PathSpaceTrellis listChildrenCanonical handles missing backing and system paths") {
    PathSpaceTrellis missing{nullptr};
    auto noBacking = missing.listChildrenCanonical("/anything");
    CHECK(noBacking.empty());

    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/data"}}); // register one source
    auto sysKids = trellis.listChildrenCanonical("/_system");
    CHECK(sysKids.empty());
}
}

#include "PathSpacePodFastpathTestSupport.hpp"

namespace {

struct RecordingSink : NotificationSink {
    void notify(const std::string& notificationPath) override {
        std::lock_guard<std::mutex> lg(mutex);
        paths.push_back(notificationPath);
    }
    std::vector<std::string> paths;
    std::mutex               mutex;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.pod.fastpath.pack_insert");

TEST_CASE("Pack insert enqueues all paths atomically") {
    PathSpace space;
    CHECK(space.insert("/ints/x", 1).errors.empty());
    CHECK(space.insert("/ints/y", 1).errors.empty());

    std::atomic<bool> mismatch{false};
    std::thread reader([&]() {
        for (int i = 0; i < 32; ++i) {
            auto ret = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
                if (xs.size() != ys.size()) {
                    mismatch.store(true, std::memory_order_release);
                }
            });
            REQUIRE(ret.has_value());
        }
    });

    std::thread writer([&]() {
        auto ret = space.insert<"/ints/x","/ints/y">(2, 3);
        CHECK(ret.errors.empty());
    });

    writer.join();
    reader.join();

    CHECK_FALSE(mismatch.load(std::memory_order_acquire));

    auto final = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == 2);
        REQUIRE(ys.size() == 2);
        CHECK(xs.back() == 2);
        CHECK(ys.back() == 3);
    });
    REQUIRE(final.has_value());
}

TEST_CASE("Pack insert span take returns full queue without minimal slicing") {
    PathSpace space;

    auto ins1 = space.insert<"/ints/x","/ints/y">(1, 10);
    REQUIRE(ins1.errors.empty());
    auto ins2 = space.insert<"/ints/x","/ints/y">(2, 20);
    REQUIRE(ins2.errors.empty());
    auto ins3 = space.insert<"/ints/x","/ints/y">(3, 30);
    REQUIRE(ins3.errors.empty());

    auto ret = space.take<"x","y">("/ints", [&](std::span<int> xs, std::span<int> ys) {
        REQUIRE(xs.size() == 3);
        REQUIRE(ys.size() == 3);
        CHECK(xs[0] == 1);
        CHECK(xs[1] == 2);
        CHECK(xs[2] == 3);
        CHECK(ys[0] == 10);
        CHECK(ys[1] == 20);
        CHECK(ys[2] == 30);
    });

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Pack insert span take minimal slices to newest batch only") {
    PathSpace space;
    // Seed existing entries so minimal mode should skip them.
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/y", 2).errors.empty());

    // Insert a new aligned batch.
    auto ins = space.insert<"/ints/x","/ints/y">(3, 4);
    REQUIRE(ins.errors.empty());

    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int> xs, std::span<int> ys) {
                                       REQUIRE(xs.size() == 1);
                                       REQUIRE(ys.size() == 1);
                                       CHECK(xs[0] == 3);
                                       CHECK(ys[0] == 4);
                                   },
                                   Out{} & Minimal{});

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Pack insert rollback clears partial reservations on failure") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/y", 1).errors.empty());

    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    Node* yNode = lookup(root, {"ints", "y"});
    REQUIRE(yNode != nullptr);
    REQUIRE(yNode->podPayload);
    // Force reservation failure on lane y
    CHECK(yNode->podPayload->freezeForUpgrade());

    auto ret = space.insert<"/ints/x","/ints/y">(2, 2);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);

    auto spans = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == 1);
        REQUIRE(ys.size() == 1);
        CHECK(xs[0] == 1); // lane x reservation rolled back
        CHECK(ys[0] == 1);
    });
    REQUIRE(spans.has_value());
}

TEST_CASE("Failed pack insert does not hide existing data in minimal take") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 1).errors.empty());

    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    Node* yNode = lookup(root, {"ints", "y"});
    REQUIRE(yNode != nullptr);
    REQUIRE(yNode->podPayload);
    CHECK(yNode->podPayload->freezeForUpgrade());

    auto ret = space.insert<"/ints/x","/ints/y">(2, 2);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);

    bool invoked = false;
    auto take = space.take<"x","y">("/ints",
                                    [&](std::span<int> xs, std::span<int> ys) {
                                        invoked = true;
                                        REQUIRE(xs.size() == 1);
                                        REQUIRE(ys.size() == 1);
                                        CHECK(xs[0] == 1);
                                        CHECK(ys[0] == 1);
                                    },
                                    Out{} & Minimal{});
    REQUIRE(take.has_value());
    CHECK(invoked);
}

TEST_CASE("Pack insert rejects glob paths") {
    PathSpace space;
    auto ret = space.insert<"/ints/*","/ints/y">(1, 2);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Pack insert notifies all affected paths") {
    PathSpace space;
    std::promise<Expected<int>> px;
    std::promise<Expected<int>> py;
    std::atomic<bool>           rx{false};
    std::atomic<bool>           ry{false};

    std::thread tx([&] {
        rx.store(true, std::memory_order_release);
        auto res = space.read<int>("/ints/x", Out{} & Block{std::chrono::milliseconds{200}});
        px.set_value(std::move(res));
    });
    std::thread ty([&] {
        ry.store(true, std::memory_order_release);
        auto res = space.read<int>("/ints/y", Out{} & Block{std::chrono::milliseconds{200}});
        py.set_value(std::move(res));
    });

    while (!rx.load(std::memory_order_acquire) || !ry.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto ret = space.insert<"/ints/x","/ints/y">(5, 6);
    REQUIRE(ret.errors.empty());

    auto fx = px.get_future();
    auto fy = py.get_future();
    REQUIRE(fx.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    REQUIRE(fy.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto vx = fx.get();
    auto vy = fy.get();
    REQUIRE(vx.has_value());
    REQUIRE(vy.has_value());
    CHECK(vx.value() == 5);
    CHECK(vy.value() == 6);

    tx.join();
    ty.join();
}

TEST_CASE("Pack insert notifies only touched lanes, not nested spaces") {
    auto context = std::make_shared<PathSpaceContext>();
    auto sink    = std::make_shared<RecordingSink>();
    context->setSink(sink);

    PathSpace parent{context};
    auto child = std::make_unique<PathSpace>(context, "/nested");
    REQUIRE(parent.insert("/nested", std::move(child)).errors.empty());

    {
        std::lock_guard<std::mutex> lg(sink->mutex);
        sink->paths.clear();
    }

    auto ret = parent.insert<"/ints/x","/ints/y">(5, 6);
    REQUIRE(ret.errors.empty());

    std::vector<std::string> pathsCopy;
    {
        std::lock_guard<std::mutex> lg(sink->mutex);
        pathsCopy = sink->paths;
    }
    std::unordered_set<std::string> expected{"/ints/x", "/ints/y"};
    CHECK(pathsCopy.size() == expected.size());
    for (auto const& p : pathsCopy) {
        CHECK(expected.contains(p));
    }
}

TEST_CASE("Pack insert detects lane length drift after single-lane insert") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 1).errors.empty());
    REQUIRE(space.insert("/ints/x", 99).errors.empty()); // skew lane x only

    auto ret = space.read<"x","y">("/ints", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);
}

TEST_CASE("Pack insert notifies blocking reads") {
    PathSpace space;
    std::promise<Expected<int>> resultPromise;
    std::atomic<bool>           readerStarted{false};

    std::thread reader([&]() {
        readerStarted.store(true, std::memory_order_release);
        auto res = space.read<int>("/ints/x", Out{} & Block{std::chrono::milliseconds{200}});
        resultPromise.set_value(std::move(res));
    });

    while (!readerStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto ret = space.insert<"/ints/x","/ints/y">(123, 456);
    REQUIRE(ret.errors.empty());

    auto future = resultPromise.get_future();
    auto status = future.wait_for(std::chrono::seconds{1});
    REQUIRE(status == std::future_status::ready);
    auto res = future.get();
    REQUIRE(res.has_value());
    CHECK(res.value() == 123);

    auto y = space.read<int>("/ints/y");
    REQUIRE(y.has_value());
    CHECK(y.value() == 456);

    reader.join();
}

TEST_CASE("Pack insert rejects glob and indexed paths") {
    PathSpace space;
    auto glob = space.insert<"/ints/*","/ints/y">(1, 2);
    CHECK_FALSE(glob.errors.empty());
    CHECK(glob.errors.front().code == Error::Code::InvalidPath);
    CHECK(glob.nbrValuesInserted == 0);

    auto indexed = space.insert<"/ints/values[0]","/ints/values[1]">(3, 4);
    CHECK_FALSE(indexed.errors.empty());
    CHECK(indexed.errors.front().code == Error::Code::InvalidPath);
    CHECK(indexed.nbrValuesInserted == 0);
}

TEST_CASE("Pack insert rejects mixed existing POD types and preserves data") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1.0f).errors.empty());
    REQUIRE(space.insert("/ints/y", 2.0f).errors.empty());

    auto ret = space.insert<"/ints/x","/ints/y">(5, 6);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidType);
    CHECK(ret.nbrValuesInserted == 0);

    auto verify = space.read<"x","y">("/ints", [&](std::span<const float> xs, std::span<const float> ys) {
        REQUIRE(xs.size() == 1);
        REQUIRE(ys.size() == 1);
        CHECK(xs[0] == 1.0f);
        CHECK(ys[0] == 2.0f);
    });
    REQUIRE(verify.has_value());
}

TEST_CASE("Pack insert rejects arity mismatch") {
    SP::Leaf leaf;
    std::array<std::string, 2> paths{"/a", "/b"};
    int                         value = 5;
    InputMetadata               md{InputMetadataT<int>{}};
    md.createPodPayload = &PodPayload<int>::CreateShared;

    std::array<void const*, 1> values{&value};
    auto ret = leaf.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                               md,
                               std::span<void const* const>(values.data(), values.size()));
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidType);
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Pack insert rejects non-POD metadata") {
    SP::Leaf leaf;
    std::array<std::string, 1> paths{"/a"};
    std::string                value{"hello"};
    InputMetadata              md{InputMetadataT<std::string>{}}; // podPreferred == false

    std::array<void const*, 1> values{&value};
    auto ret = leaf.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                               md,
                               std::span<void const* const>(values.data(), values.size()));
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::NotSupported);
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_SUITE_END();

#include "PathSpaceNestingTestSupport.hpp"

namespace {

struct CoordinatedNestedSpace : PathSpace {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool inStarted = false;
        bool allowContinue = false;
    };

    explicit CoordinatedNestedSpace(std::shared_ptr<State> state) : state(std::move(state)) {}

    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->inStarted = true;
            state->cv.notify_all();
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&]() { return state->allowContinue; });
        return PathSpace::in(path, data);
    }

private:
    std::shared_ptr<State> state;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.nesting.concurrent");

TEST_CASE("PathSpace Nesting/Nested Space with Blocking Operations") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Insert subspace first
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             // Start a thread that will write to the nested space after a delay
             std::thread writer([&root]() {
                 std::this_thread::sleep_for(100ms);
                 auto result = root.insert("/sub/delayed", 42);
                 REQUIRE(result.nbrValuesInserted == 1);
             });

             // Try to read with blocking
             auto result = root.read<int>("/sub/delayed", Block{200ms});
             writer.join();

             REQUIRE(result.has_value());
             CHECK(result.value() == 42);
}

TEST_CASE("PathSpace Nesting/Nested Space Extraction") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Add data to subspace
             REQUIRE(subspace->insert("/data", 42).nbrValuesInserted == 1);
             REQUIRE(subspace->insert("/data", 43).nbrValuesInserted == 1);

             // Insert subspace
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             // Test extraction through nested path
             auto result1 = root.take<int>("/sub/data", Block{});
             REQUIRE(result1.has_value());
             CHECK(result1.value() == 42);

             auto result2 = root.take<int>("/sub/data", Block{});
             REQUIRE(result2.has_value());
             CHECK(result2.value() == 43);

             // Verify no more data
             auto result3 = root.read<int>("/sub/data");
             CHECK(!result3.has_value());
}

TEST_CASE("PathSpace Nesting/Concurrent Access to Nested Space") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Insert subspace
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             constexpr int NUM_THREADS    = 10;
             constexpr int OPS_PER_THREAD = 100;

             std::atomic<int> insertCount{0};
             std::atomic<int> readCount{0};

             // Create writer threads
             std::vector<std::thread> writers;
             for (int i = 0; i < NUM_THREADS; ++i) {
                 writers.emplace_back([&root, i, &insertCount]() {
                     for (int j = 0; j < OPS_PER_THREAD; ++j) {
                         auto result = root.insert("/sub/data", i * OPS_PER_THREAD + j);
                         if (result.nbrValuesInserted == 1) {
                             insertCount++;
                         }
                     }
                 });
             }

             // Create reader threads
             std::vector<std::thread> readers;
             for (int i = 0; i < NUM_THREADS; ++i) {
                 readers.emplace_back([&root, &readCount]() {
                     for (int j = 0; j < OPS_PER_THREAD; ++j) {
                         auto result = root.read<int>("/sub/data", Block{10ms});
                         if (result.has_value()) {
                             readCount++;
                         }
                     }
                 });
             }

             // Join all threads
             for (auto& w : writers)
                 w.join();
             for (auto& r : readers)
                 r.join();

             // Verify operations
             CHECK(insertCount > 0);
             CHECK(readCount > 0);
             CHECK(insertCount + readCount > 0);
}

TEST_CASE("PathSpace Nesting/Nested Space Clear Operations") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Add data to subspace
             REQUIRE(subspace->insert("/data1", 42).nbrValuesInserted == 1);
             REQUIRE(subspace->insert("/data2", "test").nbrValuesInserted == 1);

             // Insert subspace
             REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

             // Clear root space
             root.clear();

             // Verify all data is cleared
             auto result1 = root.read<int>("/sub/data1");
             CHECK(!result1.has_value());

             auto result2 = root.read<std::string>("/sub/data2");
             CHECK(!result2.has_value());
}

TEST_CASE("PathSpace Nesting/Invalid Nested Space Operations") {
             PathSpace root;

             // Try to insert nullptr
             std::unique_ptr<PathSpace> nullspace = nullptr;
             auto                       result    = root.insert("/null", std::move(nullspace));
             CHECK(result.errors.size() > 0);

             // Try to insert into non-existent nested space
             auto result2 = root.insert("/nonexistent/data", 42);
             CHECK(result2.errors.empty());
             CHECK(result2.nbrValuesInserted == 1);
}

TEST_CASE("PathSpace Nesting/Nested Space Path Validation") {
             PathSpace root;
             auto      subspace = std::make_unique<PathSpace>();

             // Test invalid paths
             auto result1 = root.insert("invalid", std::move(subspace));
             CHECK(result1.errors.size() > 0);

             auto subspace2 = std::make_unique<PathSpace>();
             auto result2   = root.insert("/sub//invalid", std::move(subspace2));
             CHECK(result2.errors.size() > 0);
}

TEST_CASE("PathSpace Nesting/take blocks while nested operation in-flight") {
             using namespace std::chrono_literals;
             PathSpace root;
             auto state  = std::make_shared<CoordinatedNestedSpace::State>();
             auto nested = std::make_unique<CoordinatedNestedSpace>(state);
             REQUIRE(root.insert("/ns", std::move(nested)).nbrSpacesInserted == 1);

             std::atomic<bool> insertDone{false};
             std::atomic<bool> insertOk{false};
             std::atomic<bool> takeOk{false};

             std::thread inserter([&]() {
                 auto ret = root.insert("/ns/value", 7);
                 insertOk.store(ret.errors.empty());
                 insertDone.store(true);
             });

             {
                 std::unique_lock<std::mutex> lock(state->mutex);
                 REQUIRE(state->cv.wait_for(lock, 200ms, [&]() { return state->inStarted; }));
             }

             std::promise<void> takeDone;
             auto takeFuture = takeDone.get_future();
             std::thread taker([&]() {
                 auto taken = root.take<std::unique_ptr<PathSpace>>("/ns", Block{200ms});
                 takeOk.store(taken.has_value());
                 takeDone.set_value();
             });

             CHECK(takeFuture.wait_for(20ms) == std::future_status::timeout);
             {
                 std::lock_guard<std::mutex> lock(state->mutex);
                 state->allowContinue = true;
             }
             state->cv.notify_all();

             taker.join();
             inserter.join();
             CHECK(insertDone.load());
             CHECK(insertOk.load());
             CHECK(takeOk.load());
}

TEST_SUITE_END();

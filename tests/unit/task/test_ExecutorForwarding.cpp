#include "task/Executor.hpp"
#include "task/Task.hpp"

#include "third_party/doctest.h"

#include <memory>

using namespace SP;

namespace {
struct RecordingExecutor : Executor {
    std::weak_ptr<Task> captured;
    int                 weakCalls{0};
    bool                shutdownCalled{false};

    auto submit(std::weak_ptr<Task>&& task) -> std::optional<Error> override {
        ++weakCalls;
        captured = std::move(task);
        return std::nullopt;
    }

    auto shutdown() -> void override { shutdownCalled = true; }

    [[nodiscard]] auto size() const -> size_t override { return 1; }
};
} // namespace

TEST_SUITE("task.executor.forwarding") {
TEST_CASE("submit(shared_ptr) forwards through weak_ptr overload") {
    RecordingExecutor exec;
    auto task = Task::Create([](Task&, bool) {});

    auto err = exec.submit(task);
    CHECK_FALSE(err.has_value());
    CHECK(exec.weakCalls == 1);

    auto locked = exec.captured.lock();
    REQUIRE(locked);
    CHECK(locked == task);

    exec.shutdown();
    CHECK(exec.shutdownCalled);
}
}

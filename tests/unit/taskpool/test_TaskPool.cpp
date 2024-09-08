#include "ext/doctest.h"
#include "path/ConcretePath.hpp"
#include <pathspace/PathSpace.hpp>
#include <pathspace/taskpool/TaskPool.hpp>

using namespace SP;

auto a() -> int {
    return 36;
}

struct TaskPoolTester : public TaskPool {
    std::set<ConcretePathString> paths;
};

TEST_CASE("Task Pool") {
    PathSpace pspace;
    // TaskPool tpool;
    SUBCASE("Simple Task Pool") {
        // tpool.scheduleTask(a);
    }

    SUBCASE("Path Construction") {
        TaskPoolTester pool;
        pool.paths.insert("/DoesNotExist");
        PathSpace space(&pool);
        space.insert("/a/b/c", &a);
        // CHECK(pool.paths.contains(ConcretePathString{"/a/b/c"}));
    }
}

#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <pathspace/taskpool/TaskPool.hpp>

using namespace SP;

void a() {
}

TEST_CASE("Task Pool") {
    PathSpace pspace;
    TaskPool tpool;
    SUBCASE("Simple Task Pool") {
        // tpool.scheduleTask(a);
    }
}
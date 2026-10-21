#pragma once

#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include "core/PathSpaceContext.hpp"
#include "core/Node.hpp"
#include "tools/PathSpaceJsonExporter.hpp"
#include "task/Task.hpp"
#include "PathSpaceTestHelper.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>
#include <mutex>
#include <memory>
#include <string>
#include "task/TaskPool.hpp"

using namespace SP;
using namespace std::chrono_literals;

namespace {

struct IntrospectablePathSpace : PathSpace {
    using PathSpace::PathSpace;
    using PathSpace::shutdown;
    void rehome(std::shared_ptr<PathSpaceContext> ctx, std::string prefix) {
        this->adoptContextAndPrefix(std::move(ctx), std::move(prefix));
    }
    std::string prefixStr() const { return this->prefix; }
    std::shared_ptr<PathSpaceContext> contextPtr() const { return this->getContext(); }
    Node* rootNodePtr() { return this->getRootNode(); }
};

} // namespace

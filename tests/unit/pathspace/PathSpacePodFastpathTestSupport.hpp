#pragma once

#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <pathspace/core/NotificationSink.hpp>
#include <pathspace/core/PathSpaceContext.hpp>
#include "PathSpaceTestHelper.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <chrono>
#include <core/Leaf.hpp>
#include <optional>
#include <span>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_set>
#include <type_traits>
#include <future>
#include <array>

using namespace SP;

namespace {

[[maybe_unused]] static Node* lookup(Node* root, std::initializer_list<std::string_view> components) {
    Node* cur = root;
    for (auto c : components) {
        if (!cur) return nullptr;
        cur = cur->getChild(c);
    }
    return cur;
}

} // namespace

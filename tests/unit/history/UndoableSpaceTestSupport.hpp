#pragma once

#include "history/UndoableSpace.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "PathSpace.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"
#include <array>
#include <deque>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

using namespace SP;
using namespace SP::History;
using SP::ConcretePathStringView;

namespace {

auto makeUndoableSpace(HistoryOptions defaults = {}) -> std::unique_ptr<UndoableSpace> {
    auto inner = std::make_unique<PathSpace>();
    return std::make_unique<UndoableSpace>(std::move(inner), defaults);
}

} // namespace

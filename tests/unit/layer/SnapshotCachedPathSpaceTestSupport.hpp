#pragma once

#include "third_party/doctest.h"
#include "core/PathSpaceContext.hpp"
#define private public
#define protected public
#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/SnapshotCachedPathSpace.hpp>
#undef protected
#undef private
#include <pathspace/type/InputData.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

#pragma once
#include <parallel_hashmap/phmap.h>

#include "pathspace/core/NodeData.hpp"

namespace SP {

struct PathSpace;
struct ConcreteName;
using NodeDataHashMap = phmap::parallel_flat_hash_map<SP::ConcreteName, std::variant<SP::NodeData, std::unique_ptr<SP::PathSpace>>>;

} // namespace SP

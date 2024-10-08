#pragma once
#include "pathspace/core/NodeData.hpp"
#include <parallel_hashmap/phmap.h>

namespace SP {
class PathSpaceLeaf;
class PathSpace;
struct ConcreteName;
using NodeDataHashMap = phmap::parallel_flat_hash_map<SP::ConcreteName, std::variant<SP::NodeData, std::unique_ptr<SP::PathSpaceLeaf>>>;

} // namespace SP

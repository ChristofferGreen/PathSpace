#pragma once
#include "core/NodeData.hpp"
#include "path/ConcreteName.hpp"

#include <parallel_hashmap/phmap.h>

namespace SP {
class PathSpaceLeaf;
class PathSpace;
using NodeDataHashMap
        = phmap::parallel_node_hash_map<SP::ConcreteNameString, std::variant<SP::NodeData, std::unique_ptr<SP::PathSpaceLeaf>>>;

} // namespace SP

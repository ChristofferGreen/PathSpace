#pragma once
#include "PathSpaceBase.hpp"
#include "core/NodeData.hpp"
#include "path/TransparentString.hpp"

#include <parallel_hashmap/phmap.h>

namespace SP {
class Leaf;
class PathSpace;
using NodeDataHashMap = phmap::parallel_node_hash_map<std::string,
                                                      std::variant<SP::NodeData, std::unique_ptr<SP::Leaf>, std::unique_ptr<PathSpaceBase>>,
                                                      TransparentStringHash,
                                                      std::equal_to<>,
                                                      std::allocator<std::pair<const std::string, std::variant<SP::NodeData, std::unique_ptr<SP::Leaf>, std::unique_ptr<PathSpaceBase>>>>,
                                                      12, // Number of submaps
                                                      std::mutex>;

} // namespace SP

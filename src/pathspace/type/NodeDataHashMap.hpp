#pragma once
#include "core/NodeData.hpp"
#include "utils/transparent_string.hpp"

#include <parallel_hashmap/phmap.h>

namespace SP {
class PathSpaceLeaf;
class PathSpace;
using NodeDataHashMap = phmap::parallel_node_hash_map<std::string,
                                                      std::variant<SP::NodeData, std::unique_ptr<SP::PathSpaceLeaf>>,
                                                      transparent_string_hash,
                                                      std::equal_to<>,
                                                      std::allocator<std::pair<const std::string, std::variant<SP::NodeData, std::unique_ptr<SP::PathSpaceLeaf>>>>,
                                                      12, // Number of submaps
                                                      std::mutex>;

} // namespace SP

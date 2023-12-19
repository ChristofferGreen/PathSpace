#pragma once
#include <memory>
#include <unordered_map>
#include <vector>

namespace SP2 {

struct PathNode {
    std::unordered_map<std::string, std::shared_ptr<PathNode>> children;
    std::vector<std::byte> data;
};

}
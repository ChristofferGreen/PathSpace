#pragma once

#define private public
#include "core/NodeData.hpp"
#undef private
#include "core/Node.hpp"
#include "third_party/doctest.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <pathspace/PathSpaceBase.hpp>
#include <pathspace/PathSpace.hpp>
#include <pathspace/tools/PathSpaceJsonExporter.hpp>
#include <pathspace/tools/PathSpaceJsonConverters.hpp>
#include <pathspace/type/InputData.hpp>
#include <pathspace/type/DataCategory.hpp>
#include "../PathSpaceTestHelper.hpp"

using Json = nlohmann::json;
using namespace SP;

struct CustomType {
    int value;
};

namespace {

auto dump(PathSpace& space, PathSpaceJsonOptions options) -> Json {
    auto result = space.toJSON(options);
    REQUIRE(result);
    return Json::parse(*result);
}

auto split_path(std::string const& path) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < path.size() && path[pos] == '/') {
        ++pos;
    }
    while (pos < path.size()) {
        auto next = path.find('/', pos);
        auto end  = next == std::string::npos ? path.size() : next;
        parts.emplace_back(path.substr(pos, end - pos));
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return parts;
}

auto findNode(Json const& doc, std::string const& rootPath, std::string const& path) -> Json {
    REQUIRE(doc.contains(rootPath));
    auto rootParts   = split_path(rootPath);
    auto targetParts = split_path(path);
    REQUIRE(targetParts.size() >= rootParts.size());
    REQUIRE(std::equal(rootParts.begin(), rootParts.end(), targetParts.begin()));

    Json node = doc.at(rootPath);
    for (std::size_t idx = rootParts.size(); idx < targetParts.size(); ++idx) {
        node = node.at("children").at(targetParts[idx]);
    }
    return node;
}

} // namespace

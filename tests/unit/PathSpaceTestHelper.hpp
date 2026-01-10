#pragma once

#include <pathspace/PathSpace.hpp>
#include "core/PathSpaceContext.hpp"
#include "core/Node.hpp"
#include "task/TaskPool.hpp"

namespace SP {
struct PathSpaceTestHelper {
    static TaskPool* pool(PathSpace& ps) { return ps.pool; }
    static Executor* executor(PathSpace& ps) { return ps.getExecutor(); }
    static Node* root(PathSpace& ps) { return ps.getRootNode(); }
    static std::string prefix(PathSpace const& ps) { return ps.prefix; }
    static void copyNode(PathSpace const& src,
                         PathSpace& dst,
                         std::shared_ptr<PathSpaceContext> const& ctx,
                         std::string const& basePrefix,
                         std::string const& currentPath,
                         PathSpace::CopyStats& stats) {
        PathSpace::copyNodeRecursive(src.leaf.rootNode(), dst.leaf.rootNode(), ctx, basePrefix, currentPath, stats);
    }

};
} // namespace SP

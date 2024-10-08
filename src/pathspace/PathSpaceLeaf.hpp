#pragma once

#include "PathSpaceBase.hpp"

namespace SP {

class PathSpaceLeaf : public PathSpaceBase {
public:
    explicit PathSpaceLeaf(TaskPool* pool = nullptr) : PathSpaceBase::PathSpaceBase(pool) {};
};

} // namespace SP
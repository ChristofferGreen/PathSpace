#pragma once
#include "PathSpaceBase.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/Leaf.hpp"
#include "core/Out.hpp"
#include "core/WaitMap.hpp"
#include "log/TaggedLogger.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "path/validation.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"

namespace SP {

struct TaskPool;
class PathSpace : public PathSpaceBase {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr);
    ~PathSpace();

    virtual auto clear() -> void;

protected:
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error>;
    virtual auto shutdown() -> void;
    virtual auto notify(std::string const& notificationPath) -> void;

    TaskPool* pool = nullptr;
    WaitMap   waitMap;
    Leaf      root;
};

} // namespace SP
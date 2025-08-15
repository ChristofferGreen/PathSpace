#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "type/SlidingBuffer.hpp"
#include "task/Future.hpp"
#ifdef TYPED_TASKS
#include "task/IFutureAny.hpp"
#endif

#include <deque>
#include <memory>
#include <optional>

namespace SP {
class InsertReturn;
class InputMetadata;
class InputData;
class Task;

struct NodeData {
    NodeData() = default;
    NodeData(InputData const& inputData);

    auto serialize(const InputData& inputData) -> std::optional<Error>;
    auto deserialize(void* obj, const InputMetadata& inputMetadata) const -> std::optional<Error>;
    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> std::optional<Error>;
    auto empty() const -> bool;
#ifdef TYPED_TASKS
    // Return a type-erased future aligned with the front typed task (if present)
    auto peekAnyFuture() const -> std::optional<FutureAny>;
#endif

private:
    auto popType() -> void;
    auto pushType(InputMetadata const& meta) -> void;
    auto validateInputs(const InputMetadata& inputMetadata) -> std::optional<Error>;
    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;
    auto deserializeData(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;
    auto deserializeExecution(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;

    SP::SlidingBuffer                  data;
    std::deque<ElementType>            types;
    std::deque<std::shared_ptr<Task>>  tasks;   // NodeData is the primary owner of tasks
    std::deque<Future>                 futures; // Aligned with tasks; lightweight handles for result readiness
#ifdef TYPED_TASKS
    std::deque<FutureAny>              anyFutures; // Aligned with typed tasks; type-erased future handles
#endif
};

} // namespace SP
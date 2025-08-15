#include "NodeData.hpp"
#include "task/TaskPool.hpp"

namespace SP {

NodeData::NodeData(InputData const& inputData) {
    sp_log("NodeData::NodeData", "Function Called");
    this->serialize(inputData);
}

auto NodeData::serialize(const InputData& inputData) -> std::optional<Error> {
    sp_log("NodeData::serialize", "Function Called");
    sp_log("Serializing data of type: " + std::string(inputData.metadata.typeInfo->name()), "NodeData");
    if (inputData.task) {
        // Store task and aligned future handle
        this->tasks.push_back(std::move(inputData.task));
        this->futures.push_back(Future::FromShared(this->tasks.back()));

        bool const isImmediateExecution = (*this->tasks.rbegin())->category() == ExecutionCategory::Immediate;
        if (isImmediateExecution) {
            // Prefer injected executor when available; fall back to TaskPool singleton
            if (inputData.executor) {
                if (auto const ret = inputData.executor->submit(this->tasks.back()))
                    return ret;
            } else {
                if (auto const ret = TaskPool::Instance().submit(this->tasks.back()))
                    return ret;
            }
        }
    } else {
        if (!inputData.metadata.serialize)
            return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};
        size_t oldSize = data.size();
        inputData.metadata.serialize(inputData.obj, data);
        sp_log("Buffer size before: " + std::to_string(oldSize) + ", after: " + std::to_string(data.size()), "NodeData");
    }

    pushType(inputData.metadata);
    return std::nullopt;
}

auto NodeData::deserialize(void* obj, const InputMetadata& inputMetadata) const -> std::optional<Error> {
    sp_log("NodeData::deserialize", "Function Called");
    return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, false);
}

auto NodeData::deserializePop(void* obj, const InputMetadata& inputMetadata) -> std::optional<Error> {
    sp_log("NodeData::deserializePop", "Function Called");
    return this->deserializeImpl(obj, inputMetadata, true);
}

auto NodeData::deserializeImpl(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error> {
    sp_log("NodeData::deserializeImpl", "Function Called");

    if (auto validationResult = validateInputs(inputMetadata))
        return validationResult;

    if (this->types.front().category == DataCategory::Execution) {
        assert(!this->tasks.empty());
        return this->deserializeExecution(obj, inputMetadata, doPop);
    } else {
        return this->deserializeData(obj, inputMetadata, doPop);
    }
}

auto NodeData::validateInputs(const InputMetadata& inputMetadata) -> std::optional<Error> {
    sp_log("NodeData::validateInputs", "Function Called");

    if (this->types.empty())
        return Error{Error::Code::NoObjectFound, "No data available for deserialization"};

    if (!this->types.empty() && this->types.front().typeInfo != inputMetadata.typeInfo)
        return Error{Error::Code::InvalidType, "Type mismatch during deserialization"};

    return std::nullopt;
}

auto NodeData::deserializeExecution(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error> {
    if (this->tasks.empty())
        return Error{Error::Code::NoObjectFound, "No task available"};

    // Use the aligned future to handle readiness non-blockingly
    auto const& task = this->tasks.front();

    if (!task->hasStarted()) {
        ExecutionCategory const taskExecutionCategory = task->category();
        bool const              isLazyExecution       = taskExecutionCategory == ExecutionCategory::Lazy;

        if (isLazyExecution) {
            // Prefer the Task's preferred executor if available; fall back to singleton
            if (task->executor) {
                if (auto ret = task->executor->submit(task); ret)
                    return ret;
            } else {
                if (auto ret = TaskPool::Instance().submit(task); ret)
                    return ret;
            }
        }
    }

    // If we have a future and the task is ready, copy the result. Otherwise report not completed.
    if (!this->futures.empty() && this->futures.front().ready()) {
        if (auto locked = this->futures.front().weak_task().lock()) {
            locked->resultCopy(obj);
            if (doPop) {
                this->tasks.pop_front();
                this->futures.pop_front();
                popType();
            }
            return std::nullopt;
        }
        // Task expired unexpectedly
        return Error{Error::Code::UnknownError, "Task expired before result could be copied"};
    }

    if (task->isCompleted()) {
        task->resultCopy(obj);
        if (doPop) {
            this->tasks.pop_front();
            // Keep futures deque aligned if present
            if (!this->futures.empty())
                this->futures.pop_front();
            popType();
        }
        return std::nullopt;
    }

    return Error{Error::Code::UnknownError, "Task is not completed"};
}

auto NodeData::deserializeData(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error> {
    sp_log("NodeData::deserializeData", "Function Called");
    sp_log("Deserializing data of type: " + std::string(inputMetadata.typeInfo->name()), "NodeData");
    sp_log("Current buffer size: " + std::to_string(data.size()), "NodeData");

    if (doPop) {
        if (!inputMetadata.deserializePop)
            return Error{Error::Code::UnserializableType, "No pop deserialization function provided"};
        inputMetadata.deserializePop(obj, data);
        sp_log("After pop, buffer size: " + std::to_string(data.size()), "NodeData");
        popType();
    } else {
        if (!inputMetadata.deserialize)
            return Error{Error::Code::UnserializableType, "No deserialization function provided"};
        inputMetadata.deserialize(obj, data);
    }
    return std::nullopt;
}

auto NodeData::empty() const -> bool {
    sp_log("NodeData::empty", "Function Called");
    return this->types.empty();
}

auto NodeData::pushType(InputMetadata const& meta) -> void {
    sp_log("NodeData::pushType", "Function Called");
    if (!types.empty()) {
        if (types.back().typeInfo == meta.typeInfo)
            types.back().elements++;
        else
            types.emplace_back(meta.typeInfo, 1, meta.dataCategory);
    } else {
        types.emplace_back(meta.typeInfo, 1, meta.dataCategory);
    }
}

auto NodeData::popType() -> void {
    sp_log("NodeData::popType", "Function Called");
    if (!this->types.empty())
        if (--this->types.front().elements == 0)
            this->types.erase(this->types.begin());
}

} // namespace SP
#include "NodeData.hpp"
#include "PathSPace.hpp"
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"

namespace SP {

NodeData::NodeData(InputData const& inputData, InOptions const& options, InsertReturn& ret) {
    this->serialize(inputData, options, ret);
}

auto NodeData::serialize(const InputData& inputData, const InOptions& options, InsertReturn& ret) -> std::optional<Error> {
    if (inputData.task) {
        std::optional<ExecutionOptions> const execution = options.execution;
        this->tasks.push_back(std::move(inputData.task));
        if (bool const isImmediateExecution = execution.value_or(inputData.task->executionOptions.value_or(ExecutionOptions{})).category
                                              == ExecutionOptions::Category::Immediate) {
            if (auto const ret = TaskPool::Instance().addTask(this->tasks.back()); ret)
                return ret;
        }
        ret.nbrTasksCreated++;
    } else {
        if (!inputData.metadata.serialize)
            return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};
        inputData.metadata.serialize(inputData.obj, data);
    }

    pushType(inputData.metadata);
    return std::nullopt;
}

auto NodeData::deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options) const -> Expected<int> {
    return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, options, false);
}

auto NodeData::deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int> {
    return deserializeImpl(obj, inputMetadata, std::nullopt, true);
}

auto NodeData::deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options, bool isExtract)
        -> Expected<int> {
    sp_log("NodeData::deserializeImpl", "Function Called");

    if (auto validationResult = validateInputs(inputMetadata); !validationResult) {
        return std::unexpected(validationResult.error());
    }

    if (this->types.front().category == DataCategory::Execution) {
        assert(!this->tasks.empty());
        return deserializeExecution(obj, inputMetadata, options.value_or(OutOptions{}), isExtract);
    } else {
        return deserializeData(obj, inputMetadata, isExtract);
    }
}

auto NodeData::validateInputs(const InputMetadata& inputMetadata) -> Expected<void> {
    sp_log("NodeData::validateInputs", "Function Called");

    if (this->types.empty()) {
        return std::unexpected(Error{Error::Code::NoObjectFound, "No data available for deserialization"});
    }

    if (!this->types.empty() && this->types.front().typeInfo != inputMetadata.typeInfo) {
        return std::unexpected(Error{Error::Code::InvalidType, "Type mismatch during deserialization"});
    }

    return {};
}

auto NodeData::deserializeExecution(void* obj, const InputMetadata& inputMetadata, const OutOptions& options, bool isExtract)
        -> Expected<int> {
    sp_log("NodeData::deserializeExecution", "Function Called");

    auto& task = this->tasks.front();

    std::optional<ExecutionOptions> const execution = options.execution;
    bool const isImmediateExecution
            = execution.value_or(task->executionOptions.value_or(ExecutionOptions{})).category == ExecutionOptions::Category::Immediate;

    Expected<int> result = isImmediateExecution ? 0 : handleLazyExecution(task, options, isExtract, obj);
    if (task->state.isCompleted()) {
        task->resultCopy(task->result, obj);
        if (isExtract) {
            this->tasks.pop_front();
            popType();
        }
        return 1;
    }
    return result;
}

auto NodeData::handleLazyExecution(std::shared_ptr<Task>& task, const OutOptions& options, bool isExtract, void* obj) -> Expected<int> {
    sp_log("NodeData::handleLazyExecution", "Function Called");

    if (!task->state.hasStarted())
        if (auto ret = TaskPool::Instance().addTask(task); ret)
            return std::unexpected(ret.value());
    return 0;
}

auto NodeData::handleTaskTimeout(std::shared_ptr<Task>& task, std::chrono::milliseconds timeout) -> Expected<void> {
    sp_log("NodeData::handleTaskTimeout", "Function Called");

    auto status = task->executionFuture->wait_for(timeout);
    if (status != std::future_status::ready)
        return std::unexpected(Error{Error::Code::Timeout, "Task execution timed out after " + std::to_string(timeout.count()) + "ms"});

    try {
        task->executionFuture->get();
    } catch (const std::exception& e) {
        return std::unexpected(Error{Error::Code::UnknownError, std::string("Task execution failed: ") + e.what()});
    }

    return {};
}

auto NodeData::copyTaskResult(std::shared_ptr<Task>& task, void* obj) -> Expected<int> {
    sp_log("NodeData::copyTaskResult", "Function Called");

    TaskState finalState = task->state.get();
    if (finalState == TaskState::Failed) {
        return std::unexpected(Error{Error::Code::UnknownError, "Task execution failed"});
    }

    if (finalState != TaskState::Completed) {
        return std::unexpected(
                Error{Error::Code::NoObjectFound, std::string("Task in unexpected state: ") + std::string(taskStateToString(finalState))});
    }
    // task->resultCopy(task->asyncTask->resultPtr, obj);
    return 1;
}

auto NodeData::deserializeData(void* obj, const InputMetadata& inputMetadata, bool isExtract) -> Expected<int> {
    sp_log("NodeData::deserializeData", "Function Called");

    if (isExtract) {
        if (!inputMetadata.deserializePop) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided"});
        }
        inputMetadata.deserializePop(obj, data);
        popType();
    } else {
        if (!inputMetadata.deserialize) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided"});
        }
        inputMetadata.deserialize(obj, data);
    }
    return 1;
}

auto NodeData::executeTask(std::shared_ptr<Task> const& task) -> Expected<void> {
    sp_log("NodeData::executeTask", "Function Called");

    try {
        task->function(*task.get(), false);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error{Error::Code::UnknownError, std::string("Task execution failed: ") + e.what()});
    }
}

auto NodeData::empty() const -> bool {
    return this->types.empty();
}

auto NodeData::pushType(InputMetadata const& meta) -> void {
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
    if (!this->types.empty()) {
        if (--this->types.front().elements == 0) {
            this->types.erase(this->types.begin());
        }
    }
}

} // namespace SP
#include "NodeData.hpp"
#include "core/InsertReturn.hpp"
#include "core/OutOptions.hpp"
#include "task/TaskPool.hpp"

namespace SP {

NodeData::NodeData(InputData const& inputData, InOptions const& options) {
    sp_log("NodeData::NodeData", "Function Called");
    this->serialize(inputData, options);
}

auto NodeData::serialize(const InputData& inputData, const InOptions& options) -> std::optional<Error> {
    sp_log("NodeData::serialize", "Function Called");
    if (inputData.taskCreator) {
        this->tasks.push_back(inputData.taskCreator());
        std::optional<ExecutionOptions::Category> const optionsExecutionCategory = options.execution.has_value() ? std::optional<ExecutionOptions::Category>(options.execution.value().category) : std::nullopt;
        bool const                                      isImmediateExecution     = optionsExecutionCategory.value_or(ExecutionOptions{}.category) == ExecutionOptions::Category::Immediate;
        if (isImmediateExecution) {
            if (auto const ret = TaskPool::Instance().addTask(this->tasks.back()); ret)
                return ret;
        }
    } else {
        if (!inputData.metadata.serialize)
            return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};
        inputData.metadata.serialize(inputData.obj, data);
    }

    pushType(inputData.metadata);
    return std::nullopt;
}

auto NodeData::deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options) const -> Expected<int> {
    sp_log("NodeData::deserialize", "Function Called");
    return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, options, false);
}

auto NodeData::deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int> {
    sp_log("NodeData::deserializePop", "Function Called");
    return this->deserializeImpl(obj, inputMetadata, std::nullopt, true);
}

auto NodeData::deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options, bool isExtract) -> Expected<int> {
    sp_log("NodeData::deserializeImpl", "Function Called");

    if (auto validationResult = validateInputs(inputMetadata); !validationResult)
        return std::unexpected(validationResult.error());

    if (this->types.front().category == DataCategory::Execution) {
        assert(!this->tasks.empty());
        return this->deserializeExecution(obj, inputMetadata, options.value_or(OutOptions{}), isExtract);
    } else {
        return this->deserializeData(obj, inputMetadata, isExtract);
    }
}

auto NodeData::validateInputs(const InputMetadata& inputMetadata) -> Expected<void> {
    sp_log("NodeData::validateInputs", "Function Called");

    if (this->types.empty())
        return std::unexpected(Error{Error::Code::NoObjectFound, "No data available for deserialization"});

    if (!this->types.empty() && this->types.front().typeInfo != inputMetadata.typeInfo)
        return std::unexpected(Error{Error::Code::InvalidType, "Type mismatch during deserialization"});

    return {};
}

auto NodeData::deserializeExecution(void* obj, const InputMetadata& inputMetadata, const OutOptions& options, bool isExtract) -> Expected<int> {
    sp_log("NodeData::deserializeExecution", "Function Called");

    auto& task = this->tasks.front();

    // If task hasn't started and is lazy, start it
    if (!task->hasStarted()) {
        std::optional<ExecutionOptions::Category> const optionsExecutionCategory = options.execution.has_value() ? std::optional<ExecutionOptions::Category>(options.execution.value().category) : std::nullopt;
        std::optional<ExecutionOptions::Category> const taskExecutionCategory    = task->category();
        bool const                                      isLazyExecution          = optionsExecutionCategory.value_or(taskExecutionCategory.value_or(ExecutionOptions{}.category)) == ExecutionOptions::Category::Lazy;

        if (isLazyExecution)
            if (auto ret = TaskPool::Instance().addTask(task); ret)
                return std::unexpected(ret.value());
    }

    // If completed, return result
    if (task->isCompleted()) {
        task->resultCopy(obj);
        if (isExtract) {
            this->tasks.pop_front();
            popType();
        }
        return 1;
    }

    // Task running but not completed
    return 0;
}

auto NodeData::deserializeData(void* obj, const InputMetadata& inputMetadata, bool isExtract) -> Expected<int> {
    sp_log("NodeData::deserializeData", "Function Called");

    if (isExtract) {
        if (!inputMetadata.deserializePop)
            return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided"});
        inputMetadata.deserializePop(obj, data);
        popType();
    } else {
        if (!inputMetadata.deserialize)
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided"});
        inputMetadata.deserialize(obj, data);
    }
    return 1;
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
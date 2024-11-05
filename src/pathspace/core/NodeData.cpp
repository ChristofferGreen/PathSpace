#include "NodeData.hpp"
#include "taskpool/TaskPool.hpp"

namespace SP {

NodeData::NodeData(InputData const& inputData, InOptions const& options, InsertReturn& ret) {
    sp_log("NodeData::NodeData", "Function Called");
    this->serialize(inputData, options, ret);
}

auto NodeData::serialize(const InputData& inputData, const InOptions& options, InsertReturn& ret) -> std::optional<Error> {
    sp_log("NodeData::serialize", "Function Called");
    if (inputData.task) {
        this->tasks.push_back(std::move(inputData.task));
        if (bool const isImmediateExecution
            = options.execution.value_or(inputData.task->executionOptions.value_or(ExecutionOptions{})).category
              == ExecutionOptions::Category::Immediate) {
            if (auto const ret = TaskPool::Instance().addTask(this->tasks.back()); ret)
                return ret;
        }
        ret.nbrTasksCreated++;
    } else {
        if (!inputData.metadata.serialize)
            return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};
        inputData.metadata.serialize2(inputData.obj, data2);
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

auto NodeData::deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options, bool isExtract)
        -> Expected<int> {
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

auto NodeData::deserializeExecution(void* obj, const InputMetadata& inputMetadata, const OutOptions& options, bool isExtract)
        -> Expected<int> {
    sp_log("NodeData::deserializeExecution", "Function Called");

    auto& task = this->tasks.front();

    // If task hasn't started and is lazy, start it
    if (!task->state.hasStarted()) {
        std::optional<ExecutionOptions> const execution = options.execution;
        bool const isLazyExecution
                = execution.value_or(task->executionOptions.value_or(ExecutionOptions{})).category == ExecutionOptions::Category::Lazy;

        if (isLazyExecution)
            if (auto ret = TaskPool::Instance().addTask(task); ret)
                return std::unexpected(ret.value());
    }

    // If completed, return result
    if (task->state.isCompleted()) {
        task->resultCopy(task->result, obj);
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
        if (!inputMetadata.deserializePop2)
            return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided"});
        inputMetadata.deserializePop2(obj, data2);
        popType();
    } else {
        if (!inputMetadata.deserialize2)
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided"});
        inputMetadata.deserialize2(obj, data2);
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
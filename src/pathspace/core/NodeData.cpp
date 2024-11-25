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
    if (inputData.taskCreator) {
        this->tasks.push_back(inputData.taskCreator());
        bool const isImmediateExecution = (*this->tasks.rbegin())->category() == ExecutionCategory::Immediate;
        if (isImmediateExecution) {
            if (auto const ret = TaskPool::Instance().addTask(this->tasks.back()); ret)
                return ret;
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

auto NodeData::deserialize(void* obj, const InputMetadata& inputMetadata) const -> Expected<int> {
    sp_log("NodeData::deserialize", "Function Called");
    return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, false);
}

auto NodeData::deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int> {
    sp_log("NodeData::deserializePop", "Function Called");
    return this->deserializeImpl(obj, inputMetadata, true);
}

auto NodeData::deserializeImpl(void* obj, const InputMetadata& inputMetadata, bool doPop) -> Expected<int> {
    sp_log("NodeData::deserializeImpl", "Function Called");

    if (auto validationResult = validateInputs(inputMetadata))
        return std::unexpected(*validationResult);

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

auto NodeData::deserializeExecution(void* obj, const InputMetadata& inputMetadata, bool doPop) -> Expected<int> {
    if (this->tasks.empty())
        return std::unexpected(Error{Error::Code::NoObjectFound, "No task available"});

    // Make a copy instead of taking a reference
    auto task = this->tasks.front();

    if (!task->hasStarted()) {
        ExecutionCategory const taskExecutionCategory = task->category();
        bool const              isLazyExecution       = taskExecutionCategory == ExecutionCategory::Lazy;

        if (isLazyExecution)
            if (auto ret = TaskPool::Instance().addTask(task); ret)
                return std::unexpected(ret.value());
    }

    if (task->isCompleted()) {
        task->resultCopy(obj);
        if (doPop) {
            this->tasks.pop_front();
            popType();
        }
        return 1;
    }

    return 0;
}

auto NodeData::deserializeData(void* obj, const InputMetadata& inputMetadata, bool doPop) -> Expected<int> {
    sp_log("NodeData::deserializeData", "Function Called");
    sp_log("Deserializing data of type: " + std::string(inputMetadata.typeInfo->name()), "NodeData");
    sp_log("Current buffer size: " + std::to_string(data.size()), "NodeData");

    if (doPop) {
        if (!inputMetadata.deserializePop)
            return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided"});
        inputMetadata.deserializePop(obj, data);
        sp_log("After pop, buffer size: " + std::to_string(data.size()), "NodeData");
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
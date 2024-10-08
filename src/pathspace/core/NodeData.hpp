#pragma once
#include "ElementType.hpp"
#include "Error.hpp"
#include "InOptions.hpp"
#include "core/ExecutionOptions.hpp"
#include "core/InsertReturn.hpp"
#include "path/ConstructiblePath.hpp"
#include "pathspace/type/DataCategory.hpp"
#include "pathspace/type/InputData.hpp"
#include "taskpool/TaskPool.hpp"
#include "type/InputMetadata.hpp"

#include <expected>
#include <optional>
#include <vector>

namespace SP {

class NodeData {
public:
    auto serialize(ConstructiblePath const& path, const InputData& inputData, const InOptions& options, InsertReturn& ret)
            -> std::optional<Error> {
        if (!inputData.metadata.serialize)
            return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};

        if (inputData.metadata.category == DataCategory::ExecutionFunctionPointer) {
            if (!options.execution.has_value()
                || (options.execution.has_value() && options.execution->category == ExecutionOptions::Category::Immediate)) {
                /*pool->addTask({.callable = [](void* const functionPointer) -> void {},
                               .functionPointer = inputData.obj,
                               .path = path,
                               .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{}});*/

                // Send the function over to the ThreadPool for execution. The return type should be reinserted to the space at
                // the right path. But will be stored in a std::vector<std::any>.
                // inputData.metadata.serializeFunctionPointer(inputData.obj, data, options.execution);

                /*pool->addTask({.callable = [](void* const functionPointer) -> void {

                               },
                               .functionPointer = inputData.obj,
                               .path = path,
                               .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{}});*/

                // ToDo: Figure out optimization for this usecase:
                // space.insert("/fun", [](){ return 32; });
                // space.extract<int>("/fun");
                // The above will create an unnecessary serialize/deserialize for the returned value.
                // Not sure if avoiding the serialize/deserialize is possible.
            }
        } else {
            inputData.metadata.serialize(inputData.obj, data);
        }

        updateTypes(inputData.metadata);
        return std::nullopt;
    }

    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<ExecutionOptions> const& execution) const
            -> Expected<int> {
        if (!inputMetadata.deserialize) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided."});
        }

        if (types.empty()) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No type information found."});
        }

        if (types.front().typeInfo != inputMetadata.typeInfo && types.front().category != DataCategory::ExecutionFunctionPointer) {
            return std::unexpected(Error{Error::Code::UnserializableType, "Type mismatch during deserialization."});
        }

        if (types.front().category == DataCategory::ExecutionFunctionPointer) {
            // ToDo: If execution is marked to happen in a different thread then do that instead
            if (!execution.has_value()
                || (execution.has_value() && execution.value().category == ExecutionOptions::Category::OnReadOrExtract)
                || (execution.has_value() && execution.value().category == ExecutionOptions::Category::Immediate)) {
                void* funPtr = nullptr;
                assert(inputMetadata.deserializeFunctionPointer);
                inputMetadata.deserializeFunctionPointer(&funPtr, data);
                assert(inputMetadata.executeFunctionPointer != nullptr);
                inputMetadata.executeFunctionPointer(funPtr, obj, nullptr);
            }
        } else {
            inputMetadata.deserialize(obj, data);
        }
        return 1;
    }

    Expected<int> deserializePop(void* obj, const InputMetadata& inputMetadata) {
        if (!inputMetadata.deserializePop) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided."});
        }

        inputMetadata.deserializePop(obj, data);
        updateTypesAfterPop();
        return 1;
    }

private:
    std::vector<SERIALIZATION_TYPE> data;
    std::vector<ElementType> types;

    auto updateTypes(InputMetadata const& meta) -> void {
        if (!types.empty()) {
            if (types.back().typeInfo == meta.typeInfo) {
                types.back().elements++;
            }
        } else {
            types.emplace_back(meta.typeInfo, 1, meta.category);
        }
    }

    auto updateTypesAfterPop() -> void {
        if (!types.empty()) {
            if (--types.back().elements == 0) {
                types.pop_back();
            }
        }
    }
};

} // namespace SP
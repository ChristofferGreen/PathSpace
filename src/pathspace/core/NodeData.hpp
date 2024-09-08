#pragma once
#include "ElementType.hpp"
#include "Error.hpp"
#include "InsertOptions.hpp"
#include "core/ExecutionOptions.hpp"
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
    auto serialize(const InputData& inputData, const InsertOptions& options, TaskPool* pool, InsertReturn& ret) -> std::optional<Error> {
        if (!inputData.metadata.serialize)
            return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};

        if (inputData.metadata.category == DataCategory::FunctionPointer && options.execution->category == ExecutionOptions::Category::Immediate) {
            // Allocate enough space to store a return type or a function pointer.
            // If the function returns a value, the return type will be stored in the allocated space after execution.
            // Execute the function and store the result in the allocated space when the function returns, use pool to execute the function.
            // If the function returns a void, the function pointer will be stored in the allocated space.
            inputData.metadata.serializeFunctionPointer(inputData.obj, data, options.execution);

            // ToDo: Figure out optimization for this usecase:
            // space.insert("/fun", [](){ return 32; });
            // space.grab<int>("/fun");
            // The above will create an unnecessary serialize/deserialize for the returned value.
            // Not sure if avoiding the serialize/deserialize is possible.
        } else {
            inputData.metadata.serialize(inputData.obj, data);
        }

        updateTypes(inputData.metadata);
        return std::nullopt;
    }

    auto deserialize(void* obj, const InputMetadata& inputMetadata, TaskPool* pool, std::optional<ExecutionOptions> const& execution) const -> std::expected<int, Error> {
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
            if (!execution.has_value() || (execution.has_value() && execution.value().category == ExecutionOptions::Category::OnReadOrGrab) || (execution.has_value() && execution.value().category == ExecutionOptions::Category::Immediate)) {
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

    std::expected<int, Error> deserializePop(void* obj, const InputMetadata& inputMetadata) {
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
#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "ExecutionOptions.hpp"
#include "InOptions.hpp"
#include "InsertReturn.hpp"
#include "taskpool/Task.hpp"
#include "type/DataCategory.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"

#include <cassert>
#include <deque>
#include <expected>
#include <optional>

namespace SP {

struct NodeData {
    NodeData() = default;
    NodeData(InputData const& inputData, InOptions const& options, InsertReturn& ret) {
        this->serialize(inputData, options, ret);
    }

    auto serialize(const InputData& inputData, const InOptions& options, InsertReturn& ret) -> std::optional<Error> {
        if (inputData.task) {
            this->tasks.push_back(std::move(inputData.task));
            ret.nbrTasksCreated++;
        } else {
            if (!inputData.metadata.serialize)
                return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};
            inputData.metadata.serialize(inputData.obj, data);
        }
        pushType(inputData.metadata);
        return std::nullopt;
    }

    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<ExecutionOptions> const& execution) const
            -> Expected<int> {
        return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, execution, false);
    }

    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int> {
        return deserializeImpl(obj, inputMetadata, std::nullopt, true);
    }

    std::vector<SERIALIZATION_TYPE> data;
    std::deque<std::shared_ptr<Task>> tasks;
    std::deque<ElementType> types;

    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<ExecutionOptions> const& execution, bool isExtract)
            -> Expected<int> {
        // Check if there's any data to process
        if (this->types.empty())
            return 0;

        // Validate type matches
        if (this->types.front().typeInfo != inputMetadata.typeInfo)
            return 0;

        // Handle execution types (function pointers and std::function)
        if (this->types.front().category == DataCategory::ExecutionFunctionPointer
            || this->types.front().category == DataCategory::ExecutionStdFunction) {
            assert(!this->tasks.empty());

            auto& task = this->tasks.front();
            bool const isImmediateExecution
                    = (!execution.has_value()
                       || (execution.has_value() && execution.value().category == ExecutionOptions::Category::Immediate));

            if (isImmediateExecution) {
                // Execute immediately in the current thread
                task->function(*task.get(), obj, inputMetadata.category == DataCategory::ExecutionStdFunction);
            } else {
                // Handle async execution
                if (!task->executionFuture) {
                    // Ensure we have storage for the result
                    task->resultStorage.resize(64); // Size needs to be appropriate for your data types

                    // Launch async task using task's storage
                    task->executionFuture = std::make_shared<std::future<void>>(
                            std::async(std::launch::async, [task, isOut = inputMetadata.category == DataCategory::ExecutionStdFunction]() {
                                task->function(*task.get(), task->resultStorage.data(), isOut);
                            }));
                    return std::unexpected(Error{Error::Code::NoObjectFound, "Task started but not complete"});
                }

                // Check if existing execution is complete
                auto status = task->executionFuture->wait_for(std::chrono::milliseconds(0));
                if (status != std::future_status::ready) {
                    return std::unexpected(Error{Error::Code::NoObjectFound, "Task still executing"});
                }

                // Get result and handle any exceptions
                try {
                    task->executionFuture->get();
                    // Copy result from task's storage to caller's memory
                    std::memcpy(obj, task->resultStorage.data(), task->resultStorage.size());
                } catch (const std::exception& e) {
                    return std::unexpected(Error{Error::Code::UnknownError, e.what()});
                }
            }

            // Handle extraction if requested
            if (isExtract) {
                this->tasks.pop_front();
                popType();
            }
            return 1;
        }

        // Handle regular data types
        if (isExtract) {
            // Handle extraction of data
            if (!inputMetadata.deserializePop) {
                return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided."});
            }
            inputMetadata.deserializePop(obj, data);
            popType();
        } else {
            // Handle reading of data
            if (!inputMetadata.deserialize) {
                return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided."});
            }
            inputMetadata.deserialize(obj, data);
        }
        return 1;
    }

    auto pushType(InputMetadata const& meta) -> void {
        if (!types.empty()) {
            if (types.back().typeInfo == meta.typeInfo)
                types.back().elements++;
            else
                types.emplace_back(meta.typeInfo, 1, meta.category);
        } else {
            types.emplace_back(meta.typeInfo, 1, meta.category);
        }
    }

    auto popType() -> void {
        if (!this->types.empty()) {
            if (--this->types.front().elements == 0) {
                this->types.erase(this->types.begin());
            }
        }
    }
};

} // namespace SP
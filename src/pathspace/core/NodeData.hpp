#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "InOptions.hpp"
#include "InsertReturn.hpp"
#include "core/BlockOptions.hpp"
#include "core/ExecutionOptions.hpp"
#include "core/OutOptions.hpp"
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

    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options) const -> Expected<int> {
        return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, options, false);
    }

    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int> {
        return deserializeImpl(obj, inputMetadata, std::nullopt, true);
    }

    std::vector<SERIALIZATION_TYPE> data;
    std::deque<std::shared_ptr<Task>> tasks;
    std::deque<ElementType> types;

    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options, bool isExtract)
            -> Expected<int> {
        // Early return if there's no data to process
        if (this->types.empty())
            return 0;

        // Validate type matches
        if (this->types.front().typeInfo != inputMetadata.typeInfo)
            return 0;

        // Handle execution type (functions, lambdas, etc.)
        if (this->types.front().category == DataCategory::Execution) {
            assert(!this->tasks.empty());
            auto& task = this->tasks.front();

            // Extract execution options
            std::optional<ExecutionOptions> const execution = options.value_or(OutOptions{}).execution;
            bool const isImmediateExecution = execution.value_or(task->executionOptions).category == ExecutionOptions::Category::Immediate;
            bool const isLazyExecution = execution.value_or(task->executionOptions).category == ExecutionOptions::Category::Lazy;
            bool const hasTimeout
                    = options.has_value() && options.value().block.has_value() && options.value().block.value().timeout.has_value();

            // Handle lazy execution
            if (isLazyExecution) {
                // Try to start the task if it hasn't been started
                if (task->state.tryStart()) {
                    task->executionFuture = std::make_shared<std::future<void>>(
                            std::async(std::launch::async,
                                       [task, isExtract = inputMetadata.executionCategory == ExecutionCategory::StdFunction]() {
                                           // Attempt to transition to running state
                                           if (task->state.transitionToRunning()) {
                                               try {
                                                   // Execute the task function
                                                   task->function(*task.get(), task->resultPtr, false);
                                                   task->state.markCompleted();
                                               } catch (...) {
                                                   // Handle any exceptions during execution
                                                   task->state.markFailed();
                                                   throw; // Re-throw to be caught by future
                                               }
                                           }
                                       }));
                }

                // Handle timeout vs non-timeout cases
                try {
                    if (hasTimeout) {
                        // With timeout - wait for specified duration
                        auto const timeout = options.value().block.value().timeout.value();
                        auto status = task->executionFuture->wait_for(timeout);

                        if (status != std::future_status::ready) {
                            return std::unexpected(Error{Error::Code::Timeout,
                                                         "Task execution timed out after " + std::to_string(timeout.count()) + "ms"});
                        }
                    } else {
                        // No timeout - wait indefinitely
                        task->executionFuture->wait();
                    }

                    // Get future result to propagate any exceptions
                    task->executionFuture->get();
                } catch (const std::exception& e) {
                    return std::unexpected(Error{Error::Code::UnknownError, std::string("Task execution failed with error: ") + e.what()});
                }

                // Check final state after waiting
                TaskState finalState = task->state.get();
                if (finalState == TaskState::Failed) {
                    return std::unexpected(Error{Error::Code::UnknownError, "Task execution failed"});
                } else if (finalState != TaskState::Completed) {
                    return std::unexpected(Error{Error::Code::NoObjectFound,
                                                 std::string("Task in unexpected state: ") + std::string(taskStateToString(finalState))});
                }

                // Task completed successfully, copy the result
                task->resultCopy(task->resultPtr, obj);

                // Handle extraction if requested
                if (isExtract) {
                    this->tasks.pop_front();
                    popType();
                }
                return 1;
            } else {
                // Handle immediate execution
                if (task->state.tryStart() && task->state.transitionToRunning()) {
                    try {
                        bool const objIsData = inputMetadata.executionCategory == ExecutionCategory::StdFunction;
                        task->function(*task.get(), obj, objIsData);
                        task->state.markCompleted();
                    } catch (const std::exception& e) {
                        task->state.markFailed();
                        return std::unexpected(Error{Error::Code::UnknownError, std::string("Immediate execution failed: ") + e.what()});
                    } catch (...) {
                        task->state.markFailed();
                        return std::unexpected(Error{Error::Code::UnknownError, "Immediate execution failed with unknown error"});
                    }
                } else {
                    return std::unexpected(Error{Error::Code::NoObjectFound, "Failed to start immediate execution"});
                }

                if (isExtract) {
                    this->tasks.pop_front();
                    popType();
                }
                return 1;
            }
        }

        // Handle non-execution types
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

    auto pushType(InputMetadata const& meta) -> void {
        if (!types.empty()) {
            if (types.back().typeInfo == meta.typeInfo)
                types.back().elements++;
            else
                types.emplace_back(meta.typeInfo, 1, meta.dataCategory);
        } else {
            types.emplace_back(meta.typeInfo, 1, meta.dataCategory);
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
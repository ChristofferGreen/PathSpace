#pragma once

#include "PathSpaceBase.hpp"
#include "PathSpaceLeaf.hpp"

namespace SP {
class PathSpace : public PathSpaceBase {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr) : pool(pool) {};

protected:
    template <typename DataType>
    auto
    inFunctionPointer(bool const isConcretePath, ConstructiblePath const& constructedPath, DataType const& data, InOptions const& options)
            -> bool {
        bool const isFunctionPointer = (InputData{data}.metadata.category == DataCategory::ExecutionFunctionPointer);
        bool const isImmediateExecution
                = (!options.execution.has_value()
                   || (options.execution.has_value() && options.execution.value().category == ExecutionOptions::Category::Immediate));
        if (isConcretePath && isFunctionPointer && isImmediateExecution) {
            if constexpr (std::is_function_v<std::remove_pointer_t<DataType>>) {
                pool->addTask({.userSuppliedFunctionPointer = reinterpret_cast<void*>(data),
                               .space = this,
                               .pathToInsertReturnValueTo = constructedPath,
                               .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{},
                               .taskExecutor = [](Task const& task) {
                                   assert(task.space);
                                   if (task.userSuppliedFunctionPointer != nullptr) {
                                       auto const fun
                                               = reinterpret_cast<std::invoke_result_t<DataType> (*)()>(task.userSuppliedFunctionPointer);
                                       task.space->insert(task.pathToInsertReturnValueTo.getPath(), fun());
                                   }
                               }});
            }
            return true;
        }
        return false;
    }

    virtual InsertReturn inImpl(GlobPathStringView const& path, InputData const& data, InOptions const& options) override {
        InsertReturn ret;
        if (!path.isValid()) {
            ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
            return ret;
        }
        bool const isConcretePath = path.isConcrete();
        auto constructedPath = isConcretePath ? ConstructiblePath{path} : ConstructiblePath{};
        if (!this->inFunctionPointer(isConcretePath, constructedPath, data, options))
            this->inInternal(constructedPath, path.begin(), path.end(), InputData{data}, options, ret);
        return ret;
    };

    virtual Expected<int> outImpl(ConcretePathStringView const& path,
                                  InputMetadata const& inputMetadata,
                                  OutOptions const& options,
                                  Capabilities const& capabilities,
                                  void* obj) const override {
        // ToDo: Make sure options.doPop is set to false
        return const_cast<PathSpace*>(this)->outInternal(path.begin(), path.end(), inputMetadata, obj, options, capabilities);
    }

    TaskPool* pool;
};

} // namespace SP
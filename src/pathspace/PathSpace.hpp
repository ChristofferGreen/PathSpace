#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "core/WaitMap.hpp"
#include "path/PathIterator.hpp"
#include "path/validation.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

template <typename T>
concept SimpleStringConvertible = requires(T t) {
    { std::string(t) }; // Only checks if it can make a string
};

struct TaskPool;
class PathSpace {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr);
    ~PathSpace();

    /**
     * @brief Inserts data into the PathSpace at the specified path.
     *
     * @tparam DataType The type of data being inserted.
     * @param path The glob-style path where the data should be inserted.
     * @param data The data to be inserted.
     * @param options Options controlling the insertion behavior, such as overwrite policies.
     * @return InsertReturn object containing information about the insertion operation, including any errors.
     */
    template <typename DataType, SimpleStringConvertible S>
    auto insert(S const& pathIn, DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        PathIterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return InsertReturn{.errors = {*error}};

        InputData inputData{std::forward<DataType>(data)};

        if constexpr (InputMetadataT<DataType>::dataCategory == DataCategory::Execution) {
            inputData.taskCreator = [this, fun = data, executionCategory = options.executionCategory, pathStr = path.toString()]() -> std::shared_ptr<Task> {
                return Task::Create(this, pathStr, fun, executionCategory);
            };
        } // ToDo: Look into if we really need to do all these copies of data

        return this->in(path, inputData);
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn) == true)
    auto insert(DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        return this->insert(pathIn, std::forward<DataType>(data), options & InNoValidation{});
    }

    /**
     * @brief Reads data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be read.
     * @param path The concrete path from which to read the data.
     * @param options Options controlling the read behavior, such as blocking policies.
     * @return Expected<DataType> containing the read data if successful, or an error if not.
     */
    template <typename DataType, SimpleStringConvertible S>
    auto read(S const& pathIn, Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        PathIterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);
        DataType obj;
        if (auto error = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj))
            return std::unexpected{*error};
        return obj;
    }
    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto read(Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        return this->read<DataType>(pathIn, options & OutNoValidation{});
    }

    /**
     * @brief Reads and removes data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be extractbed.
     * @param path The concrete path from which to extract the data.
     * @return Expected<DataType> containing the extractbed data if successful, or an error if not.
     */
    template <typename DataType, SimpleStringConvertible S>
    auto take(S const& pathIn, Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        PathIterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);
        DataType obj;
        if (auto error = this->out(path, InputMetadataT<DataType>{}, options & Pop{}, &obj))
            return std::unexpected(*error);
        return obj;
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto take(Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        return this->take<DataType>(pathIn, options & Pop{} & OutNoValidation{});
    }

    auto clear() -> void;

protected:
    friend class TaskPool;

    virtual auto in(PathIterator const& path, InputData const& data) -> InsertReturn;
    auto         out(PathIterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error>;
    auto         shutdown() -> void;

    TaskPool*     pool = nullptr;
    WaitMap       waitMap;
    PathSpaceLeaf root;
};

} // namespace SP
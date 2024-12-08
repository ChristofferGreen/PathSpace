#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "core/WaitMap.hpp"
#include "path/GlobPath.hpp"
#include "path/PathIterator.hpp"
#include "path/validation.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {
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
    template <typename DataType>
    auto insert(GlobPathStringView const& path, DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        if (auto error = path.validate(options.validationLevel))
            return InsertReturn{.errors = {*error}};

        InputData inputData{std::forward<DataType>(data)};

        if (inputData.metadata.dataCategory == DataCategory::Execution) {
            inputData.taskCreator = [&, pathStr = std::string(path.getPath())]() -> std::shared_ptr<Task> {
                return Task::Create(this, pathStr, std::forward<DataType>(data), inputData, options.executionCategory);
            };
        }

        return this->in(path, inputData);
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn) == true)
    auto insert(DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        return this->insert(GlobPathStringView{pathIn}, std::forward<DataType>(data), options & InNoValidation{});
    }

    /**
     * @brief Reads data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be read.
     * @param path The concrete path from which to read the data.
     * @param options Options controlling the read behavior, such as blocking policies.
     * @return Expected<DataType> containing the read data if successful, or an error if not.
     */
    template <typename DataType>
    auto read(GlobPathStringView const& path, Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        if (auto error = path.validate())
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
        return this->read<DataType>(GlobPathStringView{pathIn}, options & OutNoValidation{});
    }

    /**
     * @brief Reads and removes data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be extractbed.
     * @param path The concrete path from which to extract the data.
     * @return Expected<DataType> containing the extractbed data if successful, or an error if not.
     */
    template <typename DataType>
    auto take(GlobPathStringView const& path, Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
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
        return this->take<DataType>(GlobPathStringView{pathIn}, options & Pop{} & OutNoValidation{});
    }

    auto clear() -> void;

protected:
    friend class TaskPool;

    virtual auto in(GlobPathStringView const& path, InputData const& data) -> InsertReturn;
    auto         out(GlobPathStringView const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error>;
    auto         shutdown() -> void;

    TaskPool*     pool = nullptr;
    WaitMap       waitMap;
    PathSpaceLeaf root;
};

} // namespace SP
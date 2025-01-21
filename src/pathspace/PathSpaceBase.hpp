#pragma once
#include "core/Error.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "log/TaggedLogger.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "path/validation.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"

#include <optional>

namespace SP {
struct InputMetadata;
struct Out;
class PathSpaceBase {
public:
    /**
     * @brief Inserts data into the PathSpace at the specified path.
     *
     * @tparam DataType The type of data being inserted.
     * @param path The glob-style path where the data should be inserted.
     * @param data The data to be inserted.
     * @param options Options controlling the insertion behavior, such as overwrite policies.
     * @return InsertReturn object containing information about the insertion operation, including any errors.
     */
    template <typename DataType, StringConvertible S>
    auto insert(S const& pathIn, DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return InsertReturn{.errors = {*error}};

        InputData inputData{std::forward<DataType>(data)};

        if constexpr (InputMetadataT<DataType>::dataCategory == DataCategory::Execution)
            inputData.task = Task::Create(this, path.toString(), data, options.executionCategory);

        return this->in(path, inputData);
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn) == true)
    auto insert(DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        return this->insert(pathIn, std::forward<DataType>(data), options & InNoValidation{});
    }

protected:
    friend class TaskPool;
    friend class PathView;

    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn                                                      = 0;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> = 0;
    virtual auto shutdown() -> void                                                                                                   = 0;
    virtual auto notify(std::string const& notificationPath) -> void                                                                  = 0;
};

} // namespace SP
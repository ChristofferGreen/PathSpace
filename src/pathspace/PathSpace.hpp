#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "core/WaitMap.hpp"
#include "path/GlobPath.hpp"
#include "path/validation.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
#include "utils/TaggedLogger.hpp"
#include <chrono>

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
                return Task::Create(this, pathStr, std::forward<DataType>(data), inputData, options);
            };
        }

        return this->in(path, inputData, options);
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn) == true)
    auto insert(DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        const_cast<In&>(options).validationLevel = ValidationLevel::None;
        return this->insert(GlobPathStringView{pathIn}, std::forward<DataType>(data), options);
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
    auto read(ConcretePathStringView const& path, Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);
        DataType   obj;
        bool const doExtract = false;
        if (auto ret = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj, doExtract); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto read(Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        const_cast<Out&>(options).validationLevel = ValidationLevel::None;
        return this->read<DataType>(ConcretePathStringView{pathIn}, options);
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path, Out const& options = Block()) const -> Expected<DataType> {
        sp_log("PathSpace::readBlock", "Function Called");
        if (auto error = path.validate())
            return std::unexpected(*error);
        DataType   obj;
        bool const doExtract = false;
        return const_cast<PathSpace*>(this)->outBlock<DataType>(path, InputMetadataT<DataType>{}, options, &obj, doExtract);
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto readBlock(Out const& options = Block()) const -> Expected<DataType> {
        sp_log("PathSpace::readBlock", "Function Called");
        const_cast<Out&>(options).validationLevel = ValidationLevel::None;
        return this->readBlock<DataType>(ConcretePathStringView{pathIn}, options);
    }

    /**
     * @brief Reads and removes data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be extractbed.
     * @param path The concrete path from which to extract the data.
     * @return Expected<DataType> containing the extractbed data if successful, or an error if not.
     */
    template <typename DataType>
    auto extract(ConcretePathStringView const& path, Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        if (auto error = path.validate())
            return std::unexpected(*error);
        DataType   obj;
        bool const doExtract = true;
        auto const ret       = this->out(path, InputMetadataT<DataType>{}, options, &obj, doExtract);
        if (!ret)
            return std::unexpected(ret.error());
        if (ret.has_value() && (ret.value() == 0))
            return std::unexpected(Error{Error::Code::NoObjectFound, std::string("Object not found at: ").append(path.getPath())});
        return obj;
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto extract(Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        const_cast<Out&>(options).validationLevel = ValidationLevel::None;
        return this->extract<DataType>(ConcretePathStringView{pathIn}, options);
    }

    template <typename DataType>
    auto extractBlock(ConcretePathStringView const& path, Out const& options = Block()) -> Expected<DataType> {
        sp_log("PathSpace::extractBlock", "Function Called");
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);
        DataType   obj;
        bool const doExtract = true;
        return this->outBlock<DataType>(path, InputMetadataT<DataType>{}, options, &obj, doExtract);
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto extractBlock(Out const& options = Block()) -> Expected<DataType> {
        sp_log("PathSpace::extractBlock", "Function Called");
        const_cast<Out&>(options).validationLevel = ValidationLevel::None;
        return this->extractBlock<DataType>(ConcretePathStringView{pathIn}, options);
    }

    auto clear() -> void;

protected:
    friend class TaskPool;

    template <typename DataType>
    auto outBlock(ConcretePathStringView const& path, InputMetadataT<DataType> const inputMetadata, Out const& options, void* objP, bool const doExtract) -> Expected<DataType> {
        sp_log("PathSpace::outBlock", "Function Called");

        DataType      obj;
        Expected<int> result; // Moved outside to be accessible in all scopes

        // First try entirely outside the loop to minimize lock time
        {
            result = this->out(path, inputMetadata, options, &obj, doExtract);
            if ((result.has_value() && result.value() > 0) || options.block_ == false) {
                return obj;
            }
        }

        auto const deadline = std::chrono::system_clock::now() + options.timeout;
        while (true) {
            // Check deadline first
            auto now = std::chrono::system_clock::now();
            if (now >= deadline) {
                return std::unexpected(Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + std::string(path.getPath())});
            }

            // Wait with minimal scope
            auto guard = waitMap.wait(path);
            {
                bool success = guard.wait_until(deadline, [&]() {
                    result          = this->out(path, inputMetadata, options, &obj, doExtract);
                    bool haveResult = (result.has_value() && result.value() > 0);
                    return haveResult;
                });

                if (success && result.has_value() && result.value() > 0) {
                    return obj;
                }
            }

            if (std::chrono::system_clock::now() >= deadline) {
                return std::unexpected(Error{Error::Code::Timeout, "Operation timed out after waking from guard, waiting for data at path: " + std::string(path.getPath())});
            }
        }
    }

    virtual auto in(GlobPathStringView const& path, InputData const& data, In const& options) -> InsertReturn;
    virtual auto out(ConcretePathStringView const& path, InputMetadata const& inputMetadata, Out const& options, void* obj, bool const doExtract) -> Expected<int>;
    auto         shutdown() -> void;

    TaskPool*     pool = nullptr;
    PathSpaceLeaf root;
    WaitMap       waitMap;
};

} // namespace SP
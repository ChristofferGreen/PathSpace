#pragma once
#include "core/Error.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "log/TaggedLogger.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "path/validation.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
#include "core/NotificationSink.hpp"
#include "task/Executor.hpp"
#include "task/Future.hpp"
#include "task/TaskT.hpp"
#include "task/IFutureAny.hpp"
#include <type_traits>
#include "core/PathSpaceContext.hpp"

#include <optional>
#include <memory>
#include <string>

namespace SP {
struct InputMetadata;
struct Out;
class PathSpaceBase {
public:
    virtual ~PathSpaceBase() = default;
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

        // Special handling for std::unique_ptr<PathSpaceBase> or for children of PathSpaceBase.
        /*if constexpr (is_unique_ptr<std::remove_cvref_t<DataType>>::value) {
            // Check specifically for
            if constexpr (std::is_base_of_v<PathSpaceBase, typename std::remove_cvref_t<DataType>::element_type>) {
                return {};
            }
        } else {*/
        InputData inputData{std::forward<DataType>(data)};
        sp_log(std::string("PathSpaceBase::insert dataCategory=") + std::to_string(static_cast<int>(InputMetadataT<DataType>::dataCategory))
               + " type=" + (InputMetadataT<DataType>::typeInfo ? InputMetadataT<DataType>::typeInfo->name() : "null"), "PathSpaceBase");
        // Thread the injected executor through InputData so downstream (NodeData) can schedule via Executor
        inputData.executor = this->getExecutor();

        if constexpr (InputMetadataT<DataType>::dataCategory == DataCategory::Execution) {
            auto notifier = this->getNotificationSink();
            auto exec     = this->getExecutor();
            using ResultT = std::invoke_result_t<DataType>;
            auto taskT    = TaskT<ResultT>::Create(std::move(notifier), path.toString(), data, options.executionCategory, exec);
            inputData.task      = taskT->legacy_task();
            inputData.anyFuture = taskT->any_future();
        }

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
    template <typename DataType, StringConvertible S>
    auto read(S const& pathIn, Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);
        DataType obj;
        if (auto error = const_cast<PathSpaceBase*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj))
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
    template <typename DataType, StringConvertible S>
    auto take(S const& pathIn, Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);
        DataType obj;
        if (auto error = this->out(path, InputMetadataT<DataType>{}, options & Pop{}, &obj))
            return std::unexpected(*error);
        return obj;
    }

    /**
     * @brief Optional Future-returning read API for execution results.
     *
     * Behavior:
     * - Returns a type-erased FutureAny if an execution is present at the path (non-blocking peek).
     *   If not present or not an execution node, returns an error.
     *
     * Note: This helper does not replace the primary read/take APIs.
     */
    template <StringConvertible S>
    auto readFuture(S const& pathIn) const -> Expected<FutureAny> {
        sp_log("PathSpace::readFuture", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(ValidationLevel::Basic))
            return std::unexpected(*error);
        // Delegate to a virtual hook so concrete implementations can surface a FutureAny.
        if (auto fut = this->typedPeekFuture(path.toStringView())) {
            return *fut;
        }
        return std::unexpected(Error{Error::Code::NoObjectFound, "No execution future available at path"});
    }

    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto take(Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        return this->take<DataType>(pathIn, options & Pop{} & OutNoValidation{});
    }

protected:
    // Provide a weak NotificationSink for lifetime-safe task notifications.
    std::weak_ptr<NotificationSink> getNotificationSink() const {
        if (context_) {
            auto w = context_->getSink();
            if (!w.lock()) {
                struct DefaultNotificationSinkImpl : NotificationSink {
                    explicit DefaultNotificationSinkImpl(PathSpaceBase* owner) : owner(owner) {}
                    void notify(const std::string& notificationPath) override { owner->notify(notificationPath); }
                    PathSpaceBase* owner;
                };
                auto sink = std::shared_ptr<NotificationSink>(new DefaultNotificationSinkImpl(const_cast<PathSpaceBase*>(this)));
                context_->setSink(sink);
                return std::weak_ptr<NotificationSink>(sink);
            }
            return w;
        }
        if (!notificationSink_) {
            struct DefaultNotificationSinkImpl : NotificationSink {
                explicit DefaultNotificationSinkImpl(PathSpaceBase* owner) : owner(owner) {}
                void notify(const std::string& notificationPath) override { owner->notify(notificationPath); }
                PathSpaceBase* owner;
            };
            notificationSink_ = std::shared_ptr<NotificationSink>(new DefaultNotificationSinkImpl(const_cast<PathSpaceBase*>(this)));
        }
        return std::weak_ptr<NotificationSink>(notificationSink_);
    }

    // Executor injection point for task scheduling (set by concrete space)
    void setExecutor(Executor* exec) {
        if (context_) context_->setExecutor(exec);
        executor_ = exec;
    }
    Executor* getExecutor() const {
        if (context_ && context_->executor()) return context_->executor();
        return executor_;
    }

        // Expose shared context for friends (e.g., Leaf) to adopt for nested spaces
        std::shared_ptr<PathSpaceContext> getContext() const { return context_; }

        // Allow nested providers to adopt shared context and an optional mount prefix.
        // Default implementation adopts the context and aligns the executor if present.
        // The prefix is intentionally ignored here; concrete PathSpace implementations may store it.
        virtual void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string /*prefix*/) {
            context_ = std::move(context);
            if (context_ && context_->executor()) {
                executor_ = context_->executor();
            }
        }

        // Public forwarding helpers (wrap protected virtuals) to enable aliasing layers
    public:
        InsertReturn forwardIn(Iterator const& path, InputData const& data) { return this->in(path, data); }
        std::optional<Error> forwardOut(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) { return this->out(path, inputMetadata, options, obj); }
        void forwardNotify(std::string const& notificationPath) { this->notify(notificationPath); }

    protected:
        mutable std::shared_ptr<NotificationSink> notificationSink_;
        std::shared_ptr<PathSpaceContext>         context_;
        Executor*                                 executor_ = nullptr;
        friend class TaskPool;
        friend class PathView;
        friend class PathFileSystem;
        friend class PathSpace;
        friend class Leaf;

        // Hook for concrete spaces to expose a type-erased future aligned with an execution node.
        // Default implementation returns no future; PathSpace overrides to provide a real handle.
        virtual std::optional<FutureAny> typedPeekFuture(std::string_view) const { return std::nullopt; }

        virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn                                                      = 0;
        virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> = 0;
        virtual auto shutdown() -> void                                                                                                   = 0;
        virtual auto notify(std::string const& notificationPath) -> void                                                                  = 0;
    };

} // namespace SP
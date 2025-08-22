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

/**
 * PathSpaceBase — core path-addressable data space interface
 *
 * Public API overview:
 * - insert(path, value/execution): Insert typed values or executions at a path. Globs are allowed at insert to fan-out to existing nodes.
 * - read<T>(path[, Out]): Copy-read typed values; blocking/timeout via Out options. Paths must be concrete (non-glob).
 * - read<FutureAny>(path): Non-blocking peek for an execution's type-erased future (if present at path).
 * - take<T>(path[, Out]): Pop-and-read typed values (FIFO for queues); supports blocking/timeout via Out & Pop.
 *
 * Protected responsibilities:
 * - Notification sink, executor/context accessors.
 * - Forwarding helpers to enable aliasing layers and nested spaces.
 * - typedPeekFuture: hook for concrete spaces to surface a type-erased future for read<FutureAny>.
 *
 * Private:
 * - Core virtual hooks (in/out/shutdown/notify) and state.
 */
class PathSpaceBase {
public:
    // ---------- Public API ----------
    virtual ~PathSpaceBase() = default;

    // Insert a typed value or execution at a path (globs allowed for insert to fan-out to existing nodes).
    template <typename DataType, StringConvertible S>
    auto insert(S const& pathIn, DataType&& data, In const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return InsertReturn{.errors = {*error}};
        InputData inputData{std::forward<DataType>(data)};
        sp_log(std::string("PathSpaceBase::insert dataCategory=") + std::to_string(static_cast<int>(InputMetadataT<DataType>::dataCategory))
               + " type=" + (InputMetadataT<DataType>::typeInfo ? InputMetadataT<DataType>::typeInfo->name() : "null"), "PathSpaceBase");
        // Ensure executor is threaded through for downstream scheduling.
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

    // Read typed values (copy). Paths must be concrete (non-glob).
    // Use Out options for blocking (Block{timeout}) or validation level.
    template <typename DataType, StringConvertible S>
        requires(!std::is_same_v<std::remove_cvref_t<DataType>, FutureAny>)
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
        requires(validate_path(pathIn) && !std::is_same_v<std::remove_cvref_t<DataType>, FutureAny>)
    auto read(Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        return this->read<DataType>(pathIn, options & OutNoValidation{});
    }

    // Read a type-erased execution future (non-blocking peek). Returns NoObjectFound if absent.
    template <StringConvertible S>
    auto read(S const& pathIn, Out const& options = {}) const -> Expected<FutureAny> {
        sp_log("PathSpace::read<FutureAny>", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);
        if (auto fut = this->typedPeekFuture(path.toStringView())) {
            return *fut;
        }
        return std::unexpected(Error{Error::Code::NoObjectFound, "No execution future available at path"});
    }
    template <FixedString pathIn, typename DataType = FutureAny>
        requires(validate_path(pathIn) && std::is_same_v<std::remove_cvref_t<DataType>, FutureAny>)
    auto read(Out const& options = {}) const -> Expected<FutureAny> {
        sp_log("PathSpace::read<FutureAny>", "Function Called");
        return this->read<FutureAny>(pathIn, options & OutNoValidation{});
    }

    // Take typed values (pop). Use Out options for blocking behavior via Pop{} and Block{timeout}.
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
    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto take(Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        return this->take<DataType>(pathIn, options & Pop{} & OutNoValidation{});
    }

protected:
    // ---------- Protected API ----------
    // Provide a weak NotificationSink; downstream users should use notify(notificationPath) instead.
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
    // Executor injection point for task scheduling.
    void setExecutor(Executor* exec) {
        if (context_) context_->setExecutor(exec);
        executor_ = exec;
    }
    Executor* getExecutor() const {
        if (context_ && context_->executor()) return context_->executor();
        return executor_;
    }

    // Shared context access & adoption (used when mounting nested spaces).
    std::shared_ptr<PathSpaceContext> getContext() const { return context_; }
    virtual void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string /*prefix*/) {
        context_ = std::move(context);
        if (context_ && context_->executor()) {
            executor_ = context_->executor();
        }
    }



    // Hook for concrete spaces to expose a type-erased future aligned with an execution node.
    // Default implementation returns no future; PathSpace overrides to provide a real handle.
    virtual std::optional<FutureAny> typedPeekFuture(std::string_view) const { return std::nullopt; }

private:
    // ---------- Private state and virtual hooks ----------
    mutable std::shared_ptr<NotificationSink> notificationSink_;
    std::shared_ptr<PathSpaceContext>         context_;
    Executor*                                 executor_ = nullptr;

    friend class TaskPool;
    friend class PathView;
    friend class PathFileSystem;
    friend class PathSpace;
    friend class Leaf;
    friend class PathAlias;

    // Core virtual hooks implemented by concrete spaces.
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn                                                      = 0;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> = 0;
    virtual auto shutdown() -> void                                                                                                   = 0;
    virtual auto notify(std::string const& notificationPath) -> void                                                                  = 0;
};

} // namespace SP

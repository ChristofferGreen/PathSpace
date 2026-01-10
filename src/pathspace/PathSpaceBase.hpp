#pragma once
#include "core/ElementType.hpp"
#include "core/Error.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/NotificationSink.hpp"
#include "core/Out.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"
#include "path/ConcretePath.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "path/validation.hpp"
#include "task/Executor.hpp"
#include "task/IFutureAny.hpp"
#include "task/Task.hpp"
#include "task/TaskT.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace SP {
struct InputMetadata;
struct Out;
struct Node;
namespace History {
class UndoableSpace;
}
namespace VisitDetail {
struct Access;
}

struct VisitOptions {
    static constexpr std::size_t kUnlimitedDepth     = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t kUnlimitedChildren = 0;
    static constexpr std::size_t kDefaultMaxChildren = 256;

    std::string root = "/";
    std::size_t maxDepth = kUnlimitedDepth;
    std::size_t maxChildren = kDefaultMaxChildren;
    bool includeNestedSpaces = true;
    bool includeValues = true;

    [[nodiscard]] constexpr auto childLimitEnabled() const noexcept -> bool {
        return maxChildren != kUnlimitedChildren;
    }

    [[nodiscard]] static constexpr auto isUnlimitedChildren(std::size_t value) noexcept -> bool {
        return value == kUnlimitedChildren;
    }
};

struct PathSpaceJsonOptions {
    enum class Mode { Minimal, Debug };

    // Defaults mirror the old ctor: nested spaces opt-in; traversal unbounded.
    VisitOptions visit{
        .root                = "/",
        .maxDepth            = VisitOptions::kUnlimitedDepth,
        .maxChildren         = VisitOptions::kUnlimitedChildren,
        .includeNestedSpaces = false,
        .includeValues       = true,
    };
    std::size_t  maxQueueEntries           = std::numeric_limits<std::size_t>::max();
    bool         includeMetadata           = false;
    bool         includeOpaquePlaceholders = false;
    bool         includeDiagnostics        = false;
    bool         includeStructureFields    = false;
    bool         flatPaths                 = false; // when true, emit flat path->values JSON
    bool         flatSimpleValues          = false; // when true, simplify flat values to raw scalars/arrays when possible
    Mode         mode                      = Mode::Minimal;
    int          dumpIndent                = 2;
};

enum class VisitControl { Continue, SkipChildren, Stop };

struct PathEntry {
    std::string path;
    bool hasChildren      = false;
    bool hasValue         = false;
    bool hasNestedSpace   = false;
    std::size_t approxChildCount = 0;
    DataCategory frontCategory = DataCategory::None;
};

struct ValueSnapshot {
    std::vector<ElementType> types;
    std::size_t queueDepth       = 0;
    bool hasExecutionPayload     = false;
    bool hasSerializedPayload    = false;
    std::size_t rawBufferBytes   = 0;
};

struct Children {
    std::vector<std::string> names;
};

class ValueHandle {
public:
    ValueHandle() = default;
    ValueHandle(ValueHandle const&) = default;
    ValueHandle(ValueHandle&&) noexcept = default;
    ValueHandle& operator=(ValueHandle const&) = default;
    ValueHandle& operator=(ValueHandle&&) noexcept = default;
    ~ValueHandle() = default;

    [[nodiscard]] auto valid() const noexcept -> bool { return static_cast<bool>(impl_); }
    [[nodiscard]] auto hasValues() const noexcept -> bool { return valid() && includesValues_; }
    [[nodiscard]] auto queueDepth() const -> std::size_t;

    template <typename DataType>
    auto read() const -> Expected<DataType> {
        if (!this->hasValues()) {
            return std::unexpected(Error{Error::Code::NotSupported, "Value sampling disabled for this visit"});
        }
        DataType output{};
        InputMetadata metadata{InputMetadataT<DataType>{}};
        if (auto error = this->readInto(&output, metadata)) {
            return std::unexpected(*error);
        }
        return output;
    }

    auto snapshot() const -> Expected<ValueSnapshot>;

private:
    struct Impl;

    explicit ValueHandle(std::shared_ptr<Impl> impl, bool includesValues)
        : impl_(std::move(impl))
        , includesValues_(includesValues) {}

    auto readInto(void* destination, InputMetadata const& metadata) const -> std::optional<Error>;

    std::shared_ptr<Impl> impl_;
    bool                  includesValues_ = false;

    friend class PathSpaceBase;
    friend struct VisitDetail::Access;
};

namespace VisitDetail {
struct Access {
    static auto MakeHandle(PathSpaceBase const& owner, Node const& node, std::string const& path, bool includeValues) -> ValueHandle;
    static auto SerializeNodeData(ValueHandle const& handle) -> std::optional<std::vector<std::byte>>;
};
} // namespace VisitDetail

namespace Detail {
template <typename T>
struct SpanTraits {
    static constexpr bool isSpan    = false;
    static constexpr bool isConst   = false;
    using Value                     = void;
};

template <typename T>
struct SpanTraits<std::span<T>> {
    static constexpr bool isSpan  = true;
    static constexpr bool isConst = false;
    using Value                   = std::remove_cv_t<T>;
};

template <typename T>
struct SpanTraits<std::span<const T>> {
    static constexpr bool isSpan  = true;
    static constexpr bool isConst = true;
    using Value                   = std::remove_cv_t<T>;
};

template <typename Sig>
struct SpanCallbackHelper;

template <typename C, typename R, typename Arg>
struct SpanCallbackHelper<R (C::*)(Arg) const> {
    using ArgType = Arg;
};

template <typename C, typename R, typename Arg>
struct SpanCallbackHelper<R (C::*)(Arg)> {
    using ArgType = Arg;
};

template <typename R, typename Arg>
struct SpanCallbackHelper<R (*)(Arg)> {
    using ArgType = Arg;
};

template <typename Fn, typename = void>
struct SpanCallbackTraits {
    static constexpr bool isSpan      = false;
    static constexpr bool isConstSpan = false;
    using Value                       = void;
};

template <typename Fn>
struct SpanCallbackTraits<Fn, std::enable_if_t<std::is_pointer_v<Fn>>> {
    using ArgHelper = SpanCallbackHelper<Fn>;
    using Arg       = typename ArgHelper::ArgType;
    using Traits    = SpanTraits<std::remove_cvref_t<Arg>>;
    static constexpr bool isSpan      = Traits::isSpan;
    static constexpr bool isConstSpan = Traits::isSpan && Traits::isConst;
    using Value = typename Traits::Value;
};

template <typename Fn>
struct SpanCallbackTraits<Fn, std::void_t<decltype(&Fn::operator())>> {
    using ArgHelper = SpanCallbackHelper<decltype(&Fn::operator())>;
    using Arg       = typename ArgHelper::ArgType;
    using Traits    = SpanTraits<std::remove_cvref_t<Arg>>;
    static constexpr bool isSpan      = Traits::isSpan;
    static constexpr bool isConstSpan = Traits::isSpan && Traits::isConst;
    using Value = typename Traits::Value;
};

template <typename Fn>
concept IsSpanCallback = SpanCallbackTraits<Fn>::isSpan;

template <typename Fn>
concept IsMutableSpanCallback = SpanCallbackTraits<Fn>::isSpan && !SpanCallbackTraits<Fn>::isConstSpan;
} // namespace Detail

using PathVisitor = std::function<VisitControl(PathEntry const&, ValueHandle&)>;

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
        using RawData = std::remove_reference_t<DataType>;
        ValidationLevel effectiveValidation = options.validationLevel;
        if constexpr (InputMetadataT<DataType>::dataCategory == DataCategory::UniquePtr) {
            using Element = typename RawData::element_type;
            if constexpr (!std::is_base_of_v<PathSpaceBase, Element>) {
                return InsertReturn{.errors = {Error{Error::Code::InvalidType,
                                                     "UniquePtr payload must derive from PathSpaceBase"}}};
            }
            if (effectiveValidation == ValidationLevel::Basic) {
                effectiveValidation = ValidationLevel::Full;
            }
        }
        if (auto error = path.validate(effectiveValidation))
            return InsertReturn{.errors = {*error}};

        InputData inputData{std::forward<DataType>(data)};

        sp_log(std::string("PathSpaceBase::insert dataCategory=") + std::to_string(static_cast<int>(inputData.metadata.dataCategory))
               + " type=" + (inputData.metadata.typeInfo ? inputData.metadata.typeInfo->name() : "null"), "PathSpaceBase");

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
    // Compile-time path overload:
    // - Validates the path literal at compile time via validate_path(pathIn)
    // - Disables runtime validation using InNoValidation (already guaranteed by the constraint)
    // - Prefer this when the path is a FixedString literal for zero-cost checks
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
        if constexpr (std::is_same_v<std::remove_cvref_t<DataType>, Children>) {
            ConcretePathString canonicalRaw{path.toString()};
            auto canonical = canonicalRaw.canonicalized();
            if (!canonical) {
                return std::unexpected(canonical.error());
            }
            auto names = this->listChildrenCanonical(canonical->getPath());
            return Children{std::move(names)};
        } else {
            DataType obj;
            if (auto error = const_cast<PathSpaceBase*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj))
                return std::unexpected{*error};
            return obj;
        }
    }

    template <typename DataType>
        requires(std::is_same_v<std::remove_cvref_t<DataType>, Children>)
    auto read(ConcretePathStringView const& pathIn, Out const& options = {}) const -> Expected<DataType> {
        auto canonical = pathIn.canonicalized();
        if (!canonical) {
            return std::unexpected(canonical.error());
        }
        auto names = this->listChildrenCanonical(canonical->getPath());
        return Children{std::move(names)};
    }
    // Compile-time path overload for read<T>:
    // - Requires a FixedString literal path validated at compile time (validate_path(pathIn))
    // - Runtime validation is disabled using OutNoValidation
    // - For typed reads (non-FutureAny), paths must be concrete (non-glob)
    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn)
                 && !std::is_same_v<std::remove_cvref_t<DataType>, FutureAny>
                 && !std::is_same_v<std::remove_cvref_t<DataType>, Children>)
    auto read(Out const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        return this->read<DataType>(pathIn, options & OutNoValidation{});
    }

    // Span callback overload (fast path only; returns Error::NotSupported otherwise).
    template <StringConvertible S, typename Fn>
        requires Detail::IsSpanCallback<Fn>
    auto read(S const& pathIn, Fn&& fn, Out const& options = {}) const -> Expected<void> {
        sp_log("PathSpace::read<span>", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);

        using Traits = Detail::SpanCallbackTraits<Fn>;
        static_assert(Traits::isConstSpan, "Span callback must take std::span<const T>");
        using Value = typename Traits::Value;

        InputMetadata metadata{InputMetadataT<Value>{}};
        auto bridge = [&](void const* data, std::size_t count) {
            fn(std::span<const Value>(static_cast<Value const*>(data), count));
        };
        metadata.spanReader = bridge;
        if (auto error = const_cast<PathSpaceBase*>(this)->out(path, metadata, options, nullptr))
            return std::unexpected{*error};
        return {};
    }

    // Compile-time path overload for span callback (read-only).
    template <FixedString pathIn, typename Fn>
        requires Detail::IsSpanCallback<Fn>
    auto read(Fn&& fn, Out const& options = {}) const -> Expected<void> {
        sp_log("PathSpace::read<span>", "Function Called");
        return this->read(pathIn, std::forward<Fn>(fn), options & OutNoValidation{});
    }

    [[nodiscard]] auto toJSON(PathSpaceJsonOptions const& options = PathSpaceJsonOptions{}) -> Expected<std::string>;

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
    // Compile-time path overload for read<FutureAny>:
    // - Validates FixedString literal at compile time (validate_path(pathIn))
    // - Skips runtime validation with OutNoValidation
    // - Returns a type-erased future handle if present, or NoObjectFound otherwise
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
    // Compile-time path overload for take<T>:
    // - Validates FixedString literal at compile time (validate_path(pathIn))
    // - Applies Pop{} semantics (consume) and disables runtime validation with OutNoValidation
    // - Use for concrete (non-glob) paths where you want to pop from a queue/stream
    template <FixedString pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto take(Out const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        return this->take<DataType>(pathIn, options & Pop{} & OutNoValidation{});
    }

    // Mutable span callback (fast path only; does NOT pop). Intended for in-place updates of POD queues.
    template <StringConvertible S, typename Fn>
        requires Detail::IsMutableSpanCallback<Fn>
    auto take(S const& pathIn, Fn&& fn, Out const& options = {}) -> Expected<void> {
        sp_log("PathSpace::take<span>", "Function Called");
        Iterator const path{pathIn};
        if (auto error = path.validate(options.validationLevel))
            return std::unexpected(*error);

        using Traits = Detail::SpanCallbackTraits<Fn>;
        static_assert(!Traits::isConstSpan, "Mutable span callback must take std::span<T>");
        using Value = typename Traits::Value;

        InputMetadata metadata{InputMetadataT<Value>{}};
        auto bridge = [&](void* data, std::size_t count) {
            fn(std::span<Value>(static_cast<Value*>(data), count));
        };
        metadata.spanMutator = bridge;
        if (auto error = this->out(path, metadata, options, nullptr))
            return std::unexpected{*error};
        return {};
    }

    // Compile-time path overload for mutable span callback (non-pop).
    template <FixedString pathIn, typename Fn>
        requires Detail::IsMutableSpanCallback<Fn>
    auto take(Fn&& fn, Out const& options = {}) -> Expected<void> {
        sp_log("PathSpace::take<span>", "Function Called");
        return this->take(pathIn, std::forward<Fn>(fn), options & OutNoValidation{});
    }

    virtual auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void>;

    [[nodiscard]] std::shared_ptr<PathSpaceContext> sharedContext() const {
        return context_;
    }

protected:
    virtual auto listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> {
        (void)canonicalPath;
        return {};
    }
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

    // - Called by the parent space when this object is mounted under a path.
    // - Supplies a shared PathSpaceContext for wait/notify and an absolute mount prefix.
    // Default behavior:
    // - Adopt the provided context and propagate its executor into this space.
    // Override guidance:
    // - Always call PathSpaceBase::adoptContextAndPrefix(...) first to inherit context/executor.
    // - If you perform targeted wake-ups, capture the 'prefix' atomically (e.g., into a member) so that
    //   later operations can call ctx->notify(prefix) or ctx->notify(prefix + "/...") rather than notifyAll().
    // - Avoid blocking work or spawning threads here; this hook should be lightweight.
    // - Treat the prefix as read-only and stable for the lifetime of the mount unless explicitly retargeted.
    virtual void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string /*prefix*/) {
        context_ = std::move(context);
        if (context_ && context_->executor()) {
            executor_ = context_->executor();
        }
    }



    // Hook for concrete spaces to expose a type-erased future aligned with an execution node.
    // Default implementation returns no future; PathSpace overrides to provide a real handle.
    virtual std::optional<FutureAny> typedPeekFuture(std::string_view) const { return std::nullopt; }

    // Internal helper for layers that need raw trie access; spaces that cannot expose their root should return nullptr.
    virtual auto getRootNode() -> Node* { return nullptr; }
    virtual auto getRootNode() const -> Node* { return const_cast<PathSpaceBase*>(this)->getRootNode(); }

private:
    // ---------- Private state and virtual hooks ----------
    mutable std::shared_ptr<NotificationSink> notificationSink_;
    std::shared_ptr<PathSpaceContext>         context_;
    Executor*                                 executor_ = nullptr;

    auto makeValueHandle(Node const& node, std::string path, bool includeValues) const -> ValueHandle;

    friend struct VisitDetail::Access;
    friend class TaskPool;
    friend class PathView;
    friend class PathFileSystem;
    friend class PathSpace;
    friend class Leaf;
    friend class PathAlias;
    friend class PathSpaceTrellis;
    friend class History::UndoableSpace;
    friend class BoundedPathSpace;
    template <typename T> friend class BoundedPathSpaceT;

    // Core virtual hooks implemented by concrete spaces.
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn                                                      = 0;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> = 0;
    virtual auto shutdown() -> void                                                                                                   = 0;
    virtual auto notify(std::string const& notificationPath) -> void                                                                  = 0;
};

} // namespace SP

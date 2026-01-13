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
#include <array>
#include <tuple>
#include <utility>
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
    static constexpr std::size_t UnlimitedDepth     = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t UnlimitedChildren = 0;
    static constexpr std::size_t DefaultMaxChildren = 256;

    std::string root = "/";
    std::size_t maxDepth = UnlimitedDepth;
    std::size_t maxChildren = DefaultMaxChildren;
    bool includeNestedSpaces = true;
    bool includeValues = true;

    [[nodiscard]] constexpr auto childLimitEnabled() const noexcept -> bool {
        return maxChildren != UnlimitedChildren;
    }

    [[nodiscard]] static constexpr auto isUnlimitedChildren(std::size_t value) noexcept -> bool {
        return value == UnlimitedChildren;
    }
};

struct PathSpaceJsonOptions {
    enum class Mode { Minimal, Debug };

    // Defaults mirror the old ctor: nested spaces opt-in; traversal unbounded.
    VisitOptions visit{
        .root                = "/",
        .maxDepth            = VisitOptions::UnlimitedDepth,
        .maxChildren         = VisitOptions::UnlimitedChildren,
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

template <typename PtrT>
struct RawSpan {
    PtrT        data  = nullptr;
    std::size_t count = 0;
};

using RawConstSpan = RawSpan<void const*>;
using RawMutSpan   = RawSpan<void*>;

struct SpanPackResult {
    std::optional<Error> error;
    bool                 shouldPop = false;
};

using SpanPackConstCallback = std::function<std::optional<Error>(std::span<RawConstSpan const>)>;
using SpanPackMutCallback   = std::function<SpanPackResult(std::span<RawMutSpan const>)>;

class ValueHandle {
public:
    ValueHandle() = default;
    ValueHandle(ValueHandle const&) = default;
    ValueHandle(ValueHandle&&) noexcept = default;
    ValueHandle& operator=(ValueHandle const&) = default;
    ValueHandle& operator=(ValueHandle&&) noexcept = default;
    ~ValueHandle() = default;

    [[nodiscard]] auto valid() const noexcept -> bool { return static_cast<bool>(this->implPtr); }
    [[nodiscard]] auto hasValues() const noexcept -> bool { return this->valid() && this->includesValues; }
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
        : implPtr(std::move(impl))
        , includesValues(includesValues) {}

    auto readInto(void* destination, InputMetadata const& metadata) const -> std::optional<Error>;

    std::shared_ptr<Impl> implPtr;
    bool                  includesValues = false;

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

template <typename Arg>
struct SpanArgTraits {
    static constexpr bool isSpan      = false;
    static constexpr bool isConstSpan = false;
    using Value                       = void;
};

template <typename T>
struct SpanArgTraits<std::span<T>> {
    static constexpr bool isSpan      = true;
    static constexpr bool isConstSpan = false;
    using Value                       = std::remove_cv_t<T>;
};

template <typename T>
struct SpanArgTraits<std::span<const T>> {
    static constexpr bool isSpan      = true;
    static constexpr bool isConstSpan = true;
    using Value                       = std::remove_cv_t<T>;
};

template <typename Sig>
struct CallableArgs;

template <typename R, typename... Args>
struct CallableArgs<R (*)(Args...)> {
    using ArgsTuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct CallableArgs<R (C::*)(Args...) const> {
    using ArgsTuple = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct CallableArgs<R (C::*)(Args...)> {
    using ArgsTuple = std::tuple<Args...>;
};

template <typename Fn>
using SpanPackSignature = std::conditional_t<std::is_pointer_v<Fn> || std::is_member_function_pointer_v<Fn>,
                                             Fn,
                                             decltype(&Fn::operator())>;

template <typename Fn, typename = void>
struct SpanPackTraits {
    static constexpr bool isSpanPack    = false;
    static constexpr bool isConstPack   = false;
    static constexpr bool isMutablePack = false;
    static constexpr std::size_t arity  = 0;
    using Value                         = void;
};

template <typename Fn>
struct SpanPackTraits<Fn, std::void_t<typename CallableArgs<SpanPackSignature<Fn>>::ArgsTuple>> {
private:
    using ArgsTuple = typename CallableArgs<SpanPackSignature<Fn>>::ArgsTuple;
    static constexpr std::size_t Arity = std::tuple_size_v<ArgsTuple>;

    template <std::size_t... Is>
    static constexpr auto checkAll(std::index_sequence<Is...>) {
        using First = std::tuple_element_t<0, ArgsTuple>;
        using FirstTraits = SpanArgTraits<std::remove_cvref_t<First>>;
        constexpr bool allSpan    = (SpanArgTraits<std::remove_cvref_t<std::tuple_element_t<Is, ArgsTuple>>>::isSpan && ...);
        constexpr bool allConst   = (SpanArgTraits<std::remove_cvref_t<std::tuple_element_t<Is, ArgsTuple>>>::isConstSpan && ...);
        constexpr bool allMutable = ((!SpanArgTraits<std::remove_cvref_t<std::tuple_element_t<Is, ArgsTuple>>>::isConstSpan) && ...);
        constexpr bool allSame    = ((std::is_same_v<typename SpanArgTraits<std::remove_cvref_t<std::tuple_element_t<Is, ArgsTuple>>>::Value, typename SpanArgTraits<std::remove_cvref_t<First>>::Value>) && ...);
        return std::tuple<FirstTraits, std::bool_constant<allSpan>, std::bool_constant<allConst>, std::bool_constant<allMutable>, std::bool_constant<allSame>>{};
    }

    using Checks = decltype(checkAll(std::make_index_sequence<Arity>{}));
    using FirstTraits = std::tuple_element_t<0, Checks>;
    static constexpr bool AllSpan    = std::tuple_element_t<1, Checks>::value;
    static constexpr bool AllConst   = std::tuple_element_t<2, Checks>::value;
    static constexpr bool AllMutable = std::tuple_element_t<3, Checks>::value;
    static constexpr bool AllSame    = std::tuple_element_t<4, Checks>::value;

public:
    static constexpr bool isSpanPack    = AllSpan && AllSame;
    static constexpr bool isConstPack   = isSpanPack && AllConst;
    static constexpr bool isMutablePack = isSpanPack && AllMutable;
    static constexpr std::size_t arity  = Arity;
    using Value                         = std::conditional_t<isSpanPack, typename FirstTraits::Value, void>;
};
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

    // Atomic multi-path insert; values are published together using POD fast-path reservations.
    template <FixedString... names, typename... DataTypes>
        requires(sizeof...(names) > 1 && sizeof...(names) == sizeof...(DataTypes))
    auto insert(DataTypes&&... data) -> InsertReturn {
        sp_log("PathSpace::insert<pack>", "Function Called");
        static_assert(sizeof...(names) == sizeof...(DataTypes));
        using First = std::remove_cvref_t<std::tuple_element_t<0, std::tuple<DataTypes...>>>;
        static_assert((std::is_same_v<First, std::remove_cvref_t<DataTypes>> && ...),
                      "Pack insert requires all value types to match");
        static_assert(std::is_trivially_copyable_v<First>,
                      "Pack insert requires POD-compatible values");

        constexpr std::size_t Arity = sizeof...(names);
        std::array<std::string, Arity> paths{std::string(names)...};

        using StoredTuple = std::tuple<std::remove_reference_t<DataTypes>...>;
        StoredTuple stored(std::forward<DataTypes>(data)...);
        std::array<void const*, Arity> valuePtrs{};
        [&, idx = std::size_t{0}]<std::size_t... Is>(std::index_sequence<Is...>) mutable {
            ((valuePtrs[idx++] = static_cast<void const*>(&std::get<Is>(stored))), ...);
        }(std::make_index_sequence<Arity>{});

        InputMetadata metadata{InputMetadataT<First>{}};
        if (metadata.createPodPayload == nullptr) {
            metadata.createPodPayload = &PodPayload<First>::CreateShared;
        }
        return this->packInsert(std::span<const std::string>(paths.data(), paths.size()),
                                metadata,
                                std::span<void const* const>(valuePtrs.data(), valuePtrs.size()));
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

    // Multi-array POD span read (compile-time field names, runtime base path).
    template <FixedString... names, StringConvertible S, typename Fn>
        requires(sizeof...(names) > 0)
    auto read(S const& basePath, Fn&& fn, Out const& options = {}) const -> Expected<void> {
        using Traits = Detail::SpanPackTraits<Fn>;
        static_assert(Traits::isSpanPack, "Callback must accept spans");
        static_assert(Traits::isConstPack, "Callback spans must be const for read");
        constexpr std::size_t Arity = sizeof...(names);
        static_assert(Arity == Traits::arity, "Template field count must match callback arity");
        return this->readSpanPackImpl<Arity, typename Traits::Value>(basePath,
                                                                       std::array<std::string_view, Arity>{std::string_view(names)...},
                                                                       std::forward<Fn>(fn),
                                                                       options);
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

    // Multi-array POD span mutable take (compile-time field names, runtime base path).
    template <FixedString... names, StringConvertible S, typename Fn>
        requires(sizeof...(names) > 0)
    auto take(S const& basePath, Fn&& fn, Out const& options = {}) -> Expected<void> {
        using Traits = Detail::SpanPackTraits<Fn>;
        static_assert(Traits::isSpanPack, "Callback must accept spans");
        static_assert(Traits::isMutablePack, "Callback spans must be mutable for take");
        constexpr std::size_t Arity = sizeof...(names);
        static_assert(Arity == Traits::arity, "Template field count must match callback arity");
        return this->takeSpanPackImpl<Arity, typename Traits::Value>(basePath,
                                                                       std::array<std::string_view, Arity>{std::string_view(names)...},
                                                                       std::forward<Fn>(fn),
                                                                       options);
    }

    virtual auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void>;

    [[nodiscard]] std::shared_ptr<PathSpaceContext> sharedContext() const {
        return this->context;
    }

protected:
    virtual auto listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> {
        (void)canonicalPath;
        return {};
    }
    // ---------- Protected API ----------
    // Provide a weak NotificationSink; downstream users should use notify(notificationPath) instead.
    std::weak_ptr<NotificationSink> getNotificationSink() const {
        if (this->context) {
            auto w = this->context->getSink();
            if (!w.lock()) {
                struct DefaultNotificationSinkImpl : NotificationSink {
                    explicit DefaultNotificationSinkImpl(PathSpaceBase* owner) : owner(owner) {}
                    void notify(const std::string& notificationPath) override { owner->notify(notificationPath); }
                    PathSpaceBase* owner;
                };
                auto sink = std::shared_ptr<NotificationSink>(new DefaultNotificationSinkImpl(const_cast<PathSpaceBase*>(this)));
                this->context->setSink(sink);
                return std::weak_ptr<NotificationSink>(sink);
            }
            return w;
        }
        if (!this->notificationSinkPtr) {
            struct DefaultNotificationSinkImpl : NotificationSink {
                explicit DefaultNotificationSinkImpl(PathSpaceBase* owner) : owner(owner) {}
                void notify(const std::string& notificationPath) override { owner->notify(notificationPath); }
                PathSpaceBase* owner;
            };
            this->notificationSinkPtr = std::shared_ptr<NotificationSink>(new DefaultNotificationSinkImpl(const_cast<PathSpaceBase*>(this)));
        }
        return std::weak_ptr<NotificationSink>(this->notificationSinkPtr);
    }
    // Executor injection point for task scheduling.
    void setExecutor(Executor* exec) {
        if (this->context) this->context->setExecutor(exec);
        this->executorPtr = exec;
    }
    Executor* getExecutor() const {
        if (this->context && this->context->executor()) return this->context->executor();
        return this->executorPtr;
    }

    // Shared context access & adoption (used when mounting nested spaces).
    std::shared_ptr<PathSpaceContext> getContext() const { return this->context; }

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
        this->context = std::move(context);
        if (this->context && this->context->executor()) {
            this->executorPtr = this->context->executor();
        }
    }



    // Hook for concrete spaces to expose a type-erased future aligned with an execution node.
    // Default implementation returns no future; PathSpace overrides to provide a real handle.
    virtual std::optional<FutureAny> typedPeekFuture(std::string_view) const { return std::nullopt; }

    // Internal helper for layers that need raw trie access; spaces that cannot expose their root should return nullptr.
    virtual auto getRootNode() -> Node* { return nullptr; }
    virtual auto getRootNode() const -> Node* { return const_cast<PathSpaceBase*>(this)->getRootNode(); }

private:

    template <typename Value, std::size_t... Is, typename Fn>
    static auto invokeConstPack(std::span<RawConstSpan const> spans, Fn& fn, std::index_sequence<Is...>) -> std::optional<Error> {
        fn(std::span<const Value>(static_cast<Value const*>(spans[Is].data), spans[Is].count)...);
        return std::nullopt;
    }

    template <typename Value, std::size_t... Is, typename Fn>
    static auto invokeMutPack(std::span<RawMutSpan const> spans, Fn& fn, std::index_sequence<Is...>) -> SpanPackResult {
        using Return = std::invoke_result_t<Fn&, decltype((void)Is, std::declval<std::span<Value>>())...>;
        if constexpr (std::is_void_v<Return>) {
            fn(std::span<Value>(static_cast<Value*>(spans[Is].data), spans[Is].count)...);
            return SpanPackResult{.error = std::nullopt, .shouldPop = false};
        } else {
            auto result = fn(std::span<Value>(static_cast<Value*>(spans[Is].data), spans[Is].count)...);
            static_assert(std::is_convertible_v<decltype(result), bool>,
                          "Span pack mutable callback must return void or bool");
            return SpanPackResult{.error = std::nullopt, .shouldPop = static_cast<bool>(result)};
        }
    }

    static auto joinPathComponent(std::string_view base, std::string_view component) -> std::string {
        if (base.empty() || base == "/") {
            std::string result{"/"};
            result.append(component);
            return result;
        }
        std::string result{base};
        if (!result.empty() && result.back() != '/') {
            result.push_back('/');
        }
        result.append(component);
        return result;
    }

    template <std::size_t N, typename Value, StringConvertible S, typename Fn>
    auto readSpanPackImpl(S const& basePath,
                          std::array<std::string_view, N> const& names,
                          Fn&& fn,
                          Out const& options) const -> Expected<void> {
        sp_log("PathSpace::read<span_pack>", "Function Called");
        if (options.doBlock || options.doPop) {
            return std::unexpected(Error{Error::Code::NotSupported, "Span pack read does not support blocking or pop"});
        }
        Iterator baseIter{basePath};
        if (auto error = baseIter.validate(options.validationLevel)) {
            return std::unexpected(*error);
        }
        std::vector<std::string> paths;
        paths.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            auto fullPath = joinPathComponent(baseIter.toStringView(), names[i]);
            Iterator check{fullPath};
            if (auto err = check.validate(options.validationLevel)) {
                return std::unexpected(*err);
            }
            paths.emplace_back(std::move(fullPath));
        }

        InputMetadata metadata{InputMetadataT<Value>{}};
        metadata.podPreferred = true;

        auto adapter = [fn = std::forward<Fn>(fn)](std::span<RawConstSpan const> spans) mutable -> std::optional<Error> {
            return invokeConstPack<Value>(std::span<RawConstSpan const>(spans.data(), spans.size()),
                                          fn,
                                          std::make_index_sequence<N>{});
        };

        return this->spanPackConst(std::span<const std::string>(paths.data(), paths.size()), metadata, options, adapter);
    }

    template <std::size_t N, typename Value, StringConvertible S, typename Fn>
    auto takeSpanPackImpl(S const& basePath,
                          std::array<std::string_view, N> const& names,
                          Fn&& fn,
                          Out const& options) -> Expected<void> {
        sp_log("PathSpace::take<span_pack>", "Function Called");
        Iterator baseIter{basePath};
        if (auto error = baseIter.validate(options.validationLevel)) {
            return std::unexpected(*error);
        }
        std::vector<std::string> paths;
        paths.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            auto fullPath = joinPathComponent(baseIter.toStringView(), names[i]);
            Iterator check{fullPath};
            if (auto err = check.validate(options.validationLevel)) {
                return std::unexpected(*err);
            }
            paths.emplace_back(std::move(fullPath));
        }

        InputMetadata metadata{InputMetadataT<Value>{}};
        metadata.podPreferred = true;

        auto adapter = [fn = std::forward<Fn>(fn)](std::span<RawMutSpan const> spans) mutable -> SpanPackResult {
            return invokeMutPack<Value>(std::span<RawMutSpan const>(spans.data(), spans.size()),
                                        fn,
                                        std::make_index_sequence<N>{});
        };

        return this->spanPackMut(std::span<const std::string>(paths.data(), paths.size()), metadata, options, adapter);
    }

    // ---------- Private state and virtual hooks ----------
    mutable std::shared_ptr<NotificationSink> notificationSinkPtr;
    std::shared_ptr<PathSpaceContext>         context;
    Executor*                                 executorPtr = nullptr;

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
    virtual auto spanPackConst(std::span<const std::string> paths,
                               InputMetadata const& metadata,
                               Out const& options,
                               SpanPackConstCallback const& fn) const -> Expected<void> {
        (void)paths;
        (void)metadata;
        (void)options;
        (void)fn;
        return std::unexpected(Error{Error::Code::NotSupported, "Span pack not supported"});
    }

    virtual auto spanPackMut(std::span<const std::string> paths,
                             InputMetadata const& metadata,
                             Out const& options,
                             SpanPackMutCallback const& fn) const -> Expected<void> {
        (void)paths;
        (void)metadata;
        (void)options;
        (void)fn;
        return std::unexpected(Error{Error::Code::NotSupported, "Span pack not supported"});
    }

    virtual auto packInsert(std::span<const std::string> paths,
                            InputMetadata const& metadata,
                            std::span<void const* const> values) -> InsertReturn {
        (void)paths;
        (void)metadata;
        (void)values;
        return InsertReturn{.errors = {Error{Error::Code::NotSupported, "Pack insert not supported"}}};
    }
};

} // namespace SP

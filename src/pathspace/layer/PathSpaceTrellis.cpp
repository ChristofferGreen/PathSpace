#include "PathSpaceTrellis.hpp"

#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "core/PathSpaceContext.hpp"
#include "path/Iterator.hpp"
#include "path/UnvalidatedPath.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace SP {
namespace {
constexpr auto kPollSlice = std::chrono::milliseconds{1};

auto isNoObjectOrTimeout(Error const& error) -> bool {
    return error.code == Error::Code::NoObjectFound || error.code == Error::Code::Timeout;
}

auto makeError(Error::Code code, std::string message) -> Error {
    return Error{code, std::move(message)};
}

} // namespace

PathSpaceTrellis::PathSpaceTrellis(std::shared_ptr<PathSpaceBase> backing)
    : backing_(std::move(backing)) {}

auto PathSpaceTrellis::in(Iterator const& path, InputData const& data) -> InsertReturn {
    if (!backing_) {
        InsertReturn ret;
        ret.errors.emplace_back(makeError(Error::Code::InvalidPermissions, "PathSpaceTrellis backing not set"));
        return ret;
    }

    if (path.isAtEnd()) {
        return handleRootInsert(data);
    }

    auto component = path.currentComponent();
    if (component == "_system") {
        auto commandIter = path.next();
        if (commandIter.isAtEnd()) {
            InsertReturn ret;
            ret.errors.emplace_back(makeError(Error::Code::InvalidPath, "Missing trellis command segment"));
            return ret;
        }

        auto commandSegment = commandIter.currentComponent();
        auto tail           = commandIter.next();
        if (!tail.isAtEnd()) {
            InsertReturn ret;
            ret.errors.emplace_back(makeError(Error::Code::InvalidPath, "Trellis command path is too deep"));
            return ret;
        }
        return handleSystemCommand(commandSegment, data);
    }

    // Pass-through for regular subpaths.
    Iterator const target{joinWithMount(path.toStringView())};
    return backing_->in(target, data);
}

auto PathSpaceTrellis::out(Iterator const& path,
                           InputMetadata const& inputMetadata,
                           Out const& options,
                           void* obj) -> std::optional<Error> {
    if (!backing_) {
        return makeError(Error::Code::InvalidPermissions, "PathSpaceTrellis backing not set");
    }

    if (!path.isAtEnd()) {
        auto component = path.currentComponent();
        if (component == "_system") {
            return makeError(Error::Code::InvalidPermissions, "PathSpaceTrellis system paths are not readable");
        }
        Iterator const target{joinWithMount(path.toStringView())};
        return backing_->out(target, inputMetadata, options, obj);
    }

    return tryFanOutRead(inputMetadata, options, obj, options.doPop);
}

auto PathSpaceTrellis::notify(std::string const& notificationPath) -> void {
    if (!backing_)
        return;

    if (notificationPath.empty() || notificationPath == "/") {
        for (auto const& source : snapshotSources()) {
            backing_->notify(source);
        }
        return;
    }

    std::string_view trimmed = notificationPath;
    if (!trimmed.empty() && trimmed.front() == '/')
        trimmed.remove_prefix(1);

    if (trimmed.rfind("_system", 0) == 0)
        return;

    backing_->notify(joinWithMount(notificationPath));
}

auto PathSpaceTrellis::visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> {
    if (!backing_) {
        return std::unexpected(Error{Error::Code::InvalidPermissions, "PathSpaceTrellis backing not set"});
    }

    VisitOptions mapped = options;
    mapped.root        = joinWithMount(options.root);

    auto trellisVisitor = [&](PathEntry const& upstreamEntry, ValueHandle& handle) -> VisitControl {
        auto localPath = stripMount(upstreamEntry.path);
        if (localPath.rfind("/_system", 0) == 0) {
            return VisitControl::SkipChildren;
        }
        PathEntry remapped = upstreamEntry;
        remapped.path      = localPath;
        return visitor(remapped, handle);
    };

    return backing_->visit(trellisVisitor, mapped);
}

auto PathSpaceTrellis::shutdown() -> void {
    isShutdown_.store(true, std::memory_order_relaxed);
    for (auto const& source : snapshotSources()) {
        backing_->notify(source);
    }
}

void PathSpaceTrellis::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
    mountPrefix_ = std::move(prefix);
}

auto PathSpaceTrellis::debugSources() const -> std::vector<std::string> {
    return snapshotSources();
}

auto PathSpaceTrellis::getRootNode() -> Node* {
    if (!backing_)
        return nullptr;
    return backing_->getRootNode();
}

auto PathSpaceTrellis::typedPeekFuture(std::string_view pathIn) const -> std::optional<FutureAny> {
    if (!backing_)
        return std::nullopt;

    if (pathIn.empty() || pathIn == "/" || pathIn == ".") {
        return tryFanOutFuture();
    }

    Iterator const iter{pathIn};
    if (!iter.isAtEnd()) {
        auto component = iter.currentComponent();
        if (component == "_system") {
            return std::nullopt;
        }
        auto joined = joinWithMount(pathIn);
        return backing_->typedPeekFuture(joined);
    }

    return tryFanOutFuture();
}

auto PathSpaceTrellis::joinWithMount(std::string_view tail) const -> std::string {
    if (mountPrefix_.empty())
        return std::string{tail};
    if (tail.empty())
        return mountPrefix_;

    std::string result = mountPrefix_;
    bool        prefixEndsWithSlash = !result.empty() && result.back() == '/';
    bool        tailStartsWithSlash = !tail.empty() && tail.front() == '/';

    if (prefixEndsWithSlash && tailStartsWithSlash) {
        tail.remove_prefix(1);
    } else if (!prefixEndsWithSlash && !tailStartsWithSlash) {
        result.push_back('/');
    }

    result.append(tail.begin(), tail.end());
    return result;
}

auto PathSpaceTrellis::stripMount(std::string const& absolute) const -> std::string {
    if (mountPrefix_.empty() || mountPrefix_ == "/")
        return absolute;
    if (absolute == mountPrefix_)
        return "/";
    if (absolute.size() > mountPrefix_.size() && absolute.rfind(mountPrefix_, 0) == 0) {
        auto remainder = absolute.substr(mountPrefix_.size());
        if (remainder.empty())
            return "/";
        if (remainder.front() != '/')
            return std::string{"/"} + remainder;
        return remainder;
    }
    return absolute;
}

auto PathSpaceTrellis::snapshotSources() const -> std::vector<std::string> {
    std::shared_lock<std::shared_mutex> guard{registryMutex_};
    return sourceOrder_;
}

auto PathSpaceTrellis::canonicalize(std::string const& path) const -> std::optional<std::string> {
    UnvalidatedPathView const view{std::string_view{path}};
    auto                      canonical = view.canonicalize_absolute();
    if (!canonical)
        return std::nullopt;
    return canonical.value();
}

auto PathSpaceTrellis::isMoveOnly(InputData const& data) const -> bool {
    return data.metadata.dataCategory == DataCategory::UniquePtr;
}

auto PathSpaceTrellis::extractStringPayload(InputData const& data) const -> std::optional<std::string> {
    if (data.obj == nullptr)
        return std::nullopt;

    auto const* typeInfo = data.metadata.typeInfo;
    if (typeInfo == &typeid(std::string))
        return *static_cast<std::string const*>(data.obj);
    if (typeInfo == &typeid(std::string_view))
        return std::string{*static_cast<std::string_view const*>(data.obj)};
    if (typeInfo == &typeid(char const*))
        return std::string{static_cast<char const*>(data.obj)};
    if (typeInfo == &typeid(char*))
        return std::string{static_cast<char const*>(data.obj)};

    // Fallback: assume null-terminated string literal.
    return std::string{static_cast<char const*>(data.obj)};
}

auto PathSpaceTrellis::handleRootInsert(InputData const& data) -> InsertReturn {
    InsertReturn aggregate;

    auto sources = snapshotSources();
    if (sources.empty()) {
        aggregate.errors.emplace_back(makeError(Error::Code::NoObjectFound, "No trellis sources registered"));
        return aggregate;
    }

    bool   moveOnly   = isMoveOnly(data);
    size_t baseCursor = roundRobinCursor_.fetch_add(1, std::memory_order_relaxed);
    size_t startIndex = sources.empty() ? 0 : baseCursor % sources.size();

    auto routeInsert = [&](std::string const& target) {
        Iterator const iter{target};
        InsertReturn  partial = backing_->in(iter, data);
        mergeInsertReturn(aggregate, partial);
    };

    if (moveOnly) {
        routeInsert(sources[startIndex]);
        return aggregate;
    }

    for (size_t offset = 0; offset < sources.size(); ++offset) {
        size_t index = (startIndex + offset) % sources.size();
        routeInsert(sources[index]);
    }
    return aggregate;
}

auto PathSpaceTrellis::handleSystemCommand(std::string_view command, InputData const& data) -> InsertReturn {
    auto payloadOpt = extractStringPayload(data);
    if (!payloadOpt) {
        InsertReturn ret;
        ret.errors.emplace_back(makeError(Error::Code::InvalidType, "Trellis command requires string payload"));
        return ret;
    }

    if (command == "enable")
        return handleEnable(*payloadOpt);
    if (command == "disable")
        return handleDisable(*payloadOpt);

    InsertReturn ret;
    ret.errors.emplace_back(makeError(Error::Code::InvalidPath, "Unknown trellis command"));
    return ret;
}

auto PathSpaceTrellis::handleEnable(std::string const& payload) -> InsertReturn {
    InsertReturn ret;

    auto canonicalOpt = canonicalize(payload);
    if (!canonicalOpt) {
        ret.errors.emplace_back(makeError(Error::Code::InvalidPath, "Invalid trellis source path"));
        return ret;
    }

    std::unique_lock<std::shared_mutex> guard{registryMutex_};
    auto const& canonical = *canonicalOpt;
    if (sourceSet_.contains(canonical))
        return ret;

    sourceSet_.insert(canonical);
    sourceOrder_.push_back(canonical);
    return ret;
}

auto PathSpaceTrellis::handleDisable(std::string const& payload) -> InsertReturn {
    InsertReturn ret;

    auto canonicalOpt = canonicalize(payload);
    if (!canonicalOpt)
        return ret;

    std::unique_lock<std::shared_mutex> guard{registryMutex_};
    auto const& canonical = *canonicalOpt;
    if (!sourceSet_.erase(canonical))
        return ret;

    auto newEnd = std::remove(sourceOrder_.begin(), sourceOrder_.end(), canonical);
    sourceOrder_.erase(newEnd, sourceOrder_.end());
    return ret;
}

auto PathSpaceTrellis::tryFanOutRead(InputMetadata const& metadata,
                                     Out const& options,
                                     void* obj,
                                     bool /*doPop*/) -> std::optional<Error> {
    auto sources = snapshotSources();
    if (sources.empty()) {
        return makeError(Error::Code::NoObjectFound, "No trellis sources registered");
    }

    size_t baseCursor = roundRobinCursor_.fetch_add(1, std::memory_order_relaxed);
    size_t startIndex = baseCursor % sources.size();

    auto attemptSources = [&](Out const& attemptOptions) -> std::optional<Error> {
        bool sawEmpty = false;
        for (size_t offset = 0; offset < sources.size(); ++offset) {
            size_t index = (startIndex + offset) % sources.size();
            Iterator const iter{sources[index]};
            auto           error = backing_->out(iter, metadata, attemptOptions, obj);
            if (!error.has_value()) {
                return std::nullopt;
            }
            if (isNoObjectOrTimeout(*error)) {
                sawEmpty = true;
                continue;
            }
            return error;
        }
        if (sawEmpty) {
            return makeError(Error::Code::NoObjectFound, "No trellis sources ready");
        }
        return makeError(Error::Code::NoSuchPath, "No trellis sources available");
    };

    Out immediateOptions = options;
    immediateOptions.doBlock = false;
    immediateOptions.timeout = std::chrono::milliseconds{0};
    if (auto immediate = attemptSources(immediateOptions); !immediate.has_value()) {
        return std::nullopt;
    } else if (!options.doBlock || immediate->code != Error::Code::NoObjectFound) {
        return immediate;
    }

    auto const startTime = std::chrono::steady_clock::now();
    auto const infinite  = options.timeout == DEFAULT_TIMEOUT;

    while (true) {
        if (isShutdown_.load(std::memory_order_relaxed)) {
            return makeError(Error::Code::Timeout, "PathSpaceTrellis shutting down");
        }

        std::chrono::milliseconds remaining = options.timeout;
        if (!infinite) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);
            if (elapsed >= options.timeout) {
                return makeError(Error::Code::Timeout, "PathSpaceTrellis read timed out");
            }
            remaining = options.timeout - elapsed;
        }

        Out blockOptions = options;
        blockOptions.doBlock = true;
        blockOptions.timeout = infinite ? kPollSlice : std::min(kPollSlice, remaining);

        auto result = attemptSources(blockOptions);
        if (!result.has_value()) {
            return std::nullopt;
        }
        if (!isNoObjectOrTimeout(*result)) {
            return result;
        }

        if (!infinite) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);
            if (elapsed >= options.timeout) {
                return makeError(Error::Code::Timeout, "PathSpaceTrellis read timed out");
            }
        }
    }
}

auto PathSpaceTrellis::tryFanOutFuture() const -> std::optional<FutureAny> {
    auto sources = snapshotSources();
    if (sources.empty())
        return std::nullopt;

    size_t baseCursor = roundRobinCursor_.fetch_add(1, std::memory_order_relaxed);
    size_t startIndex = baseCursor % sources.size();

    for (size_t offset = 0; offset < sources.size(); ++offset) {
        size_t index = (startIndex + offset) % sources.size();
        if (auto fut = backing_->typedPeekFuture(sources[index])) {
            return fut;
        }
    }
    return std::nullopt;
}

auto PathSpaceTrellis::mergeInsertReturn(InsertReturn& target, InsertReturn const& source) -> void {
    target.nbrValuesInserted += source.nbrValuesInserted;
    target.nbrSpacesInserted += source.nbrSpacesInserted;
    target.nbrTasksInserted  += source.nbrTasksInserted;
    target.nbrValuesSuppressed += source.nbrValuesSuppressed;
    if (!source.retargets.empty()) {
        target.retargets.insert(target.retargets.end(), source.retargets.begin(), source.retargets.end());
    }
    if (!source.errors.empty()) {
        target.errors.insert(target.errors.end(), source.errors.begin(), source.errors.end());
    }
}

auto PathSpaceTrellis::isSystemPath(Iterator const& path) -> bool {
    return !path.isAtEnd() && path.currentComponent() == "_system";
}

} // namespace SP

#include "PathSpaceTrellis.hpp"

#include "core/Error.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>

namespace SP {

namespace {

auto toLowerCopy(std::string_view value) -> std::string {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

} // namespace

PathSpaceTrellis::PathSpaceTrellis(std::shared_ptr<PathSpaceBase> backing)
    : backing_(std::move(backing)) {
    if (backing_) {
        if (auto ctx = backing_->getContext()) {
            this->adoptContextAndPrefix(ctx, {});
        }
    }
}

void PathSpaceTrellis::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
    std::lock_guard<std::mutex> lg(statesMutex_);
    mountPrefix_ = std::move(prefix);
}

auto PathSpaceTrellis::canonicalizeAbsolute(std::string const& raw) -> Expected<std::string> {
    ConcretePathString path{raw};
    auto               canonical = path.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    return canonical->getPath();
}

auto PathSpaceTrellis::canonicalizeSourceList(std::vector<std::string> const& rawSources)
    -> Expected<std::vector<std::string>> {
    if (rawSources.empty()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Source list must not be empty"});
    }
    std::vector<std::string> canonical;
    canonical.reserve(rawSources.size());
    std::unordered_set<std::string> seen;

    for (auto const& raw : rawSources) {
        auto entry = canonicalizeAbsolute(raw);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        auto canonicalValue = entry.value();
        auto [_, inserted]  = seen.insert(canonicalValue);
        if (!inserted) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                         "Source list must not contain duplicate entries"});
        }
        canonical.push_back(std::move(canonicalValue));
    }
    return canonical;
}

auto PathSpaceTrellis::parseEnableCommand(InputData const& data) const -> Expected<EnableParseResult> {
    if (data.metadata.typeInfo != &typeid(EnableTrellisCommand)) {
        return std::unexpected(Error{Error::Code::InvalidType,
                                     "Enable trellis command requires EnableTrellisCommand payload"});
    }
    auto const* command = static_cast<EnableTrellisCommand const*>(data.obj);
    if (!command) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Enable command payload missing"});
    }

    auto outputPath = canonicalizeAbsolute(command->name);
    if (!outputPath) {
        return std::unexpected(outputPath.error());
    }
    if (outputPath->starts_with("/_system/trellis/state")) {
        return std::unexpected(Error{Error::Code::InvalidPath,
                                     "Output path is reserved for trellis state"});
    }

    auto canonicalSources = canonicalizeSourceList(command->sources);
    if (!canonicalSources) {
        return std::unexpected(canonicalSources.error());
    }
    auto const& sources = canonicalSources.value();
    if (std::find(sources.begin(), sources.end(), outputPath.value()) != sources.end()) {
        return std::unexpected(Error{Error::Code::InvalidPath,
                                     "Output path cannot also be used as a source"});
    }

    auto modeString = toLowerCopy(command->mode);
    auto policyString = toLowerCopy(command->policy);

    TrellisMode mode = TrellisMode::Queue;
    if (modeString == "queue") {
        mode = TrellisMode::Queue;
    } else if (modeString == "latest") {
        return std::unexpected(Error{Error::Code::NotSupported,
                                     "Latest mode is not yet supported"});
    } else {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                     "Unsupported trellis mode: " + command->mode});
    }

    TrellisPolicy policy = TrellisPolicy::RoundRobin;
    if (policyString == "round_robin") {
        policy = TrellisPolicy::RoundRobin;
    } else if (policyString == "priority") {
        policy = TrellisPolicy::Priority;
    } else {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                     "Unsupported trellis policy: " + command->policy});
    }

    EnableParseResult result{
        .outputPath  = std::move(outputPath.value()),
        .mode        = mode,
        .policy      = policy,
        .sources     = sources,
    };
    return result;
}

auto PathSpaceTrellis::parseDisableCommand(InputData const& data) const -> Expected<std::string> {
    if (data.metadata.typeInfo != &typeid(DisableTrellisCommand)) {
        return std::unexpected(Error{Error::Code::InvalidType,
                                     "Disable trellis command requires DisableTrellisCommand payload"});
    }
    auto const* command = static_cast<DisableTrellisCommand const*>(data.obj);
    if (!command) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Disable command payload missing"});
    }
    auto canonical = canonicalizeAbsolute(command->name);
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    return canonical.value();
}

auto PathSpaceTrellis::handleEnable(InputData const& data) -> InsertReturn {
    InsertReturn ret;
    auto parsed = parseEnableCommand(data);
    if (!parsed) {
        ret.errors.emplace_back(parsed.error());
        return ret;
    }

    std::lock_guard<std::mutex> lg(statesMutex_);
    if (trellis_.contains(parsed->outputPath)) {
        ret.errors.emplace_back(Error{Error::Code::InvalidPath, "Trellis already enabled for path"});
        return ret;
    }

    auto state = std::make_shared<TrellisState>();
    state->mode            = parsed->mode;
    state->policy          = parsed->policy;
    state->sources         = parsed->sources;
    state->roundRobinCursor = 0;
    trellis_.emplace(parsed->outputPath, std::move(state));

    if (auto ctx = this->getContext()) {
        ctx->notify(parsed->outputPath);
    }
    return ret;
}

auto PathSpaceTrellis::handleDisable(InputData const& data) -> InsertReturn {
    InsertReturn ret;
    auto parsed = parseDisableCommand(data);
    if (!parsed) {
        ret.errors.emplace_back(parsed.error());
        return ret;
    }

    std::shared_ptr<TrellisState> removed;
    {
        std::lock_guard<std::mutex> lg(statesMutex_);
        auto it = trellis_.find(parsed.value());
        if (it == trellis_.end()) {
            ret.errors.emplace_back(Error{Error::Code::NotFound, "Trellis not found for path"});
            return ret;
        }
        removed = std::move(it->second);
        trellis_.erase(it);
    }

    if (removed) {
        std::lock_guard<std::mutex> stateLock(removed->mutex);
        removed->shuttingDown = true;
    }

    if (auto ctx = this->getContext()) {
        ctx->notify(parsed.value());
    }
    return ret;
}

auto PathSpaceTrellis::in(Iterator const& path, InputData const& data) -> InsertReturn {
    auto pathStr = path.toString();
    if (pathStr == "/_system/trellis/enable") {
        return handleEnable(data);
    }
    if (pathStr == "/_system/trellis/disable") {
        return handleDisable(data);
    }
    if (!backing_) {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"}}};
    }
    return backing_->in(path, data);
}

auto PathSpaceTrellis::tryServeQueue(TrellisState& state,
                                     InputMetadata const& inputMetadata,
                                     Out const& options,
                                     void* obj) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
    }

    std::vector<std::string> sourcesCopy;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        sourcesCopy = state.sources;
    }

    if (sourcesCopy.empty()) {
        return Error{Error::Code::NotFound, "No sources configured"};
    }

    std::size_t startIndex = 0;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        startIndex = (state.policy == TrellisPolicy::RoundRobin) ? state.roundRobinCursor : 0;
    }

    auto attemptOptions = options;
    attemptOptions.doBlock = false;

    std::optional<Error> lastError;

    auto advanceCursor = [&](std::size_t next) {
        if (state.policy == TrellisPolicy::RoundRobin) {
            std::lock_guard<std::mutex> lg(state.mutex);
            state.roundRobinCursor = next % sourcesCopy.size();
        }
    };

    for (std::size_t offset = 0; offset < sourcesCopy.size(); ++offset) {
        std::size_t index      = (startIndex + offset) % sourcesCopy.size();
        Iterator    sourceIter = Iterator{sourcesCopy[index]};
        auto        err        = backing_->out(sourceIter, inputMetadata, attemptOptions, obj);
        if (!err) {
            advanceCursor(index + 1);
            return std::nullopt;
        }
        if (err->code != Error::Code::NoObjectFound
            && err->code != Error::Code::NotFound
            && err->code != Error::Code::NoSuchPath) {
            return err;
        }
        lastError = err;
    }
    return lastError.value_or(Error{Error::Code::NoObjectFound, "No data available in sources"});
}

auto PathSpaceTrellis::waitAndServeQueue(TrellisState& state,
                                         InputMetadata const& inputMetadata,
                                         Out const& options,
                                         void* obj,
                                         std::chrono::system_clock::time_point deadline) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    std::vector<std::string> sourcesCopy;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
        sourcesCopy = state.sources;
    }
    if (sourcesCopy.empty()) {
        return Error{Error::Code::NotFound, "No sources configured"};
    }

    auto now = std::chrono::system_clock::now();
    if (now >= deadline) {
        return Error{Error::Code::Timeout, "Trellis wait timed out"};
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds::zero()) {
        return Error{Error::Code::Timeout, "Trellis wait timed out"};
    }

    std::size_t waitIndex = 0;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.policy == TrellisPolicy::RoundRobin) {
            waitIndex = state.roundRobinCursor % sourcesCopy.size();
        }
    }

    auto blockingOptions   = options;
    blockingOptions.doBlock = true;
    blockingOptions.timeout = remaining;

    Iterator sourceIter{sourcesCopy[waitIndex]};
    auto     err = backing_->out(sourceIter, inputMetadata, blockingOptions, obj);
    if (!err) {
        if (state.policy == TrellisPolicy::RoundRobin) {
            std::lock_guard<std::mutex> lg(state.mutex);
            state.roundRobinCursor = (waitIndex + 1) % sourcesCopy.size();
        }
        return std::nullopt;
    }
    return err;
}

auto PathSpaceTrellis::serveLatest(TrellisState& state,
                                   InputMetadata const& inputMetadata,
                                   Out const& options,
                                   void* obj) -> std::optional<Error> {
    // Latest mode currently behaves as priority read across sources.
    // It always prefers the first source that has data and performs a non-destructive read.
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
    }
    auto attemptOptions = options;
    attemptOptions.doBlock = false;
    attemptOptions.doPop   = false;

    std::vector<std::string> sourcesCopy;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        sourcesCopy = state.sources;
    }

    if (sourcesCopy.empty()) {
        return Error{Error::Code::NotFound, "No sources configured"};
    }

    for (auto const& source : sourcesCopy) {
        Iterator sourceIter{source};
        auto     err = backing_->out(sourceIter, inputMetadata, attemptOptions, obj);
        if (!err) {
            return std::nullopt;
        }
        if (err->code != Error::Code::NoObjectFound
            && err->code != Error::Code::NotFound
            && err->code != Error::Code::NoSuchPath) {
            return err;
        }
    }
    return Error{Error::Code::NoObjectFound, "No data available in sources"};
}

auto PathSpaceTrellis::out(Iterator const& path,
                           InputMetadata const& inputMetadata,
                           Out const& options,
                           void* obj) -> std::optional<Error> {
    auto absolutePathRaw = path.toString();
    std::string absolutePath;
    {
        std::lock_guard<std::mutex> lg(statesMutex_);
        if (!mountPrefix_.empty() && mountPrefix_ != "/") {
            if (absolutePathRaw.empty() || absolutePathRaw == "/") {
                absolutePath = mountPrefix_;
            } else if (absolutePathRaw.front() == '/') {
                absolutePath = mountPrefix_ + absolutePathRaw;
            } else {
                absolutePath = mountPrefix_ + "/" + absolutePathRaw;
            }
        } else {
            absolutePath = absolutePathRaw;
        }
    }
    auto canonicalAbsolute = canonicalizeAbsolute(absolutePath);
    if (!canonicalAbsolute) {
        return canonicalAbsolute.error();
    }
    absolutePath = canonicalAbsolute.value();

    std::shared_ptr<TrellisState> state;
    {
        std::lock_guard<std::mutex> lg(statesMutex_);
        auto it = trellis_.find(absolutePath);
        if (it != trellis_.end()) {
            state = it->second;
        }
    }

    if (!state) {
        if (backing_) {
            return backing_->out(path, inputMetadata, options, obj);
        }
        return Error{Error::Code::NotFound, "Path not managed by trellis"};
    }

    auto deadline = std::chrono::system_clock::now() + options.timeout;

    if (state->mode == TrellisMode::Queue) {
        auto result = tryServeQueue(*state, inputMetadata, options, obj);
        if (!result) {
            return result;
        }
        if (!options.doBlock) {
            return result;
        }
        return waitAndServeQueue(*state, inputMetadata, options, obj, deadline);
    }

    auto result = serveLatest(*state, inputMetadata, options, obj);
    if (!result) {
        return result;
    }
    if (!options.doBlock) {
        return result;
    }
    return waitAndServeQueue(*state, inputMetadata, options, obj, deadline);
}

auto PathSpaceTrellis::notify(std::string const& notificationPath) -> void {
    if (auto ctx = this->getContext()) {
        ctx->notify(notificationPath);
    }
}

auto PathSpaceTrellis::shutdown() -> void {
    std::unordered_map<std::string, std::shared_ptr<TrellisState>> snapshot;
    {
        std::lock_guard<std::mutex> lg(statesMutex_);
        snapshot = trellis_;
        trellis_.clear();
    }
    for (auto& [_, state] : snapshot) {
        if (!state) continue;
        std::lock_guard<std::mutex> lg(state->mutex);
        state->shuttingDown = true;
    }
    if (auto ctx = this->getContext()) {
        ctx->shutdown();
    }
}

} // namespace SP

#include "PathSpaceTrellis.hpp"

#include "core/Error.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
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

auto encodeStateKey(std::string_view path) -> std::string {
    constexpr char hexDigits[] = "0123456789abcdef";
    std::string    encoded;
    encoded.reserve(path.size() * 2);
    for (unsigned char ch : path) {
        encoded.push_back(hexDigits[(ch >> 4) & 0x0F]);
        encoded.push_back(hexDigits[ch & 0x0F]);
    }
    return encoded;
}

} // namespace

PathSpaceTrellis::PathSpaceTrellis(std::shared_ptr<PathSpaceBase> backing)
    : backing_(std::move(backing)) {
    if (backing_) {
        if (auto ctx = backing_->getContext()) {
            this->adoptContextAndPrefix(ctx, {});
        }
        std::lock_guard<std::mutex> lg(statesMutex_);
        if (!persistenceLoaded_) {
            restorePersistedStatesLocked();
        }
    }
}

void PathSpaceTrellis::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
    std::lock_guard<std::mutex> lg(statesMutex_);
    mountPrefix_ = std::move(prefix);
    restorePersistedStatesLocked();
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

auto PathSpaceTrellis::stateConfigPathFor(std::string const& canonicalOutputPath) -> std::string {
    auto key = encodeStateKey(canonicalOutputPath);
    std::string path = "/_system/trellis/state/";
    path.append(key);
    path.append("/config");
    return path;
}

auto PathSpaceTrellis::stateStatsPathFor(std::string const& canonicalOutputPath) -> std::string {
    auto key = encodeStateKey(canonicalOutputPath);
    std::string path = "/_system/trellis/state/";
    path.append(key);
    path.append("/stats");
    return path;
}

auto PathSpaceTrellis::modeToString(TrellisMode mode) -> std::string {
    switch (mode) {
    case TrellisMode::Queue:
        return "queue";
    case TrellisMode::Latest:
        return "latest";
    }
    return "queue";
}

auto PathSpaceTrellis::policyToString(TrellisPolicy policy) -> std::string {
    switch (policy) {
    case TrellisPolicy::RoundRobin:
        return "round_robin";
    case TrellisPolicy::Priority:
        return "priority";
    }
    return "round_robin";
}

auto PathSpaceTrellis::modeFromString(std::string_view value) -> Expected<TrellisMode> {
    auto lowered = toLowerCopy(value);
    if (lowered == "queue") {
        return TrellisMode::Queue;
    }
    if (lowered == "latest") {
        return TrellisMode::Latest;
    }
    return std::unexpected(Error{Error::Code::MalformedInput,
                                 "Unsupported trellis mode in persisted config: " + std::string(value)});
}

auto PathSpaceTrellis::policyFromString(std::string_view value) -> Expected<TrellisPolicy> {
    auto lowered = toLowerCopy(value);
    if (lowered == "round_robin") {
        return TrellisPolicy::RoundRobin;
    }
    if (lowered == "priority") {
        return TrellisPolicy::Priority;
    }
    return std::unexpected(Error{Error::Code::MalformedInput,
                                 "Unsupported trellis policy in persisted config: " + std::string(value)});
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
        mode = TrellisMode::Latest;
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

auto PathSpaceTrellis::persistStateLocked(std::string const& canonicalOutputPath, TrellisState const& state)
    -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }

    TrellisPersistedConfig config{
        .name    = canonicalOutputPath,
        .sources = state.sources,
        .mode    = modeToString(state.mode),
        .policy  = policyToString(state.policy),
    };

    auto const configPath = stateConfigPathFor(canonicalOutputPath);

    while (true) {
        auto existing = backing_->take<TrellisPersistedConfig>(configPath);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        return existing.error();
    }

    auto insertResult = backing_->insert(configPath, config);
    if (!insertResult.errors.empty()) {
        return insertResult.errors.front();
    }
    return std::nullopt;
}

auto PathSpaceTrellis::erasePersistedState(std::string const& canonicalOutputPath) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    auto const configPath = stateConfigPathFor(canonicalOutputPath);
    while (true) {
        auto existing = backing_->take<TrellisPersistedConfig>(configPath);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            // Drop mismatched legacy entries by attempting a raw string pop.
            auto legacy = backing_->take<std::string>(configPath);
            if (legacy) {
                continue;
            }
            auto legacyCode = legacy.error().code;
            if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                break;
            }
            return legacy.error();
        }
        return existing.error();
    }
    return std::nullopt;
}

auto PathSpaceTrellis::persistStats(std::string const& canonicalOutputPath, TrellisState const& state)
    -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    auto const statsPath = stateStatsPathFor(canonicalOutputPath);

    while (true) {
        auto existing = backing_->take<TrellisStats>(statsPath);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::string>(statsPath);
            if (legacy) {
                continue;
            }
            auto legacyCode = legacy.error().code;
            if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                break;
            }
            return legacy.error();
        }
        return existing.error();
    }

    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto stats = TrellisStats{
        .name          = canonicalOutputPath,
        .mode          = modeToString(state.mode),
        .policy        = policyToString(state.policy),
        .sources       = state.sources,
        .sourceCount   = static_cast<std::uint64_t>(state.sources.size()),
        .servedCount   = 0,
        .waitCount     = 0,
        .errorCount    = 0,
        .lastSource    = std::string{},
        .lastErrorCode = 0,
        .lastUpdateNs  = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),
    };

    auto insertResult = backing_->insert(statsPath, stats);
    if (!insertResult.errors.empty()) {
        return insertResult.errors.front();
    }
    return std::nullopt;
}

auto PathSpaceTrellis::eraseStats(std::string const& canonicalOutputPath) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    auto const statsPath = stateStatsPathFor(canonicalOutputPath);
    while (true) {
        auto existing = backing_->take<TrellisStats>(statsPath);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::string>(statsPath);
            if (legacy) {
                continue;
            }
            auto legacyCode = legacy.error().code;
            if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                break;
            }
            return legacy.error();
        }
        return existing.error();
    }
    return std::nullopt;
}

auto PathSpaceTrellis::updateStats(std::string const& canonicalOutputPath,
                                   std::function<void(TrellisStats&)> const& mutate) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    auto const statsPath = stateStatsPathFor(canonicalOutputPath);
    TrellisStats stats{};
    bool         haveStats = false;

    while (true) {
        auto existing = backing_->take<TrellisStats>(statsPath);
        if (existing) {
            stats     = std::move(existing.value());
            haveStats = true;
            break;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::string>(statsPath);
            if (legacy) {
                continue;
            }
            auto legacyCode = legacy.error().code;
            if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                break;
            }
            return legacy.error();
        }
        return existing.error();
    }

    if (!haveStats) {
        std::shared_ptr<TrellisState> stateSnapshot;
        {
            std::lock_guard<std::mutex> lg(statesMutex_);
            auto it = trellis_.find(canonicalOutputPath);
            if (it != trellis_.end()) {
                stateSnapshot = it->second;
            }
        }
        if (!stateSnapshot) {
            return std::nullopt;
        }
        stats.name        = canonicalOutputPath;
        stats.mode        = modeToString(stateSnapshot->mode);
        stats.policy      = policyToString(stateSnapshot->policy);
        stats.sources     = stateSnapshot->sources;
        stats.sourceCount = static_cast<std::uint64_t>(stateSnapshot->sources.size());
    }

    mutate(stats);

    auto insertResult = backing_->insert(statsPath, stats);
    if (!insertResult.errors.empty()) {
        return insertResult.errors.front();
    }
    return std::nullopt;
}

void PathSpaceTrellis::recordServeSuccess(std::string const& canonicalOutputPath,
                                          std::string const& sourcePath,
                                          bool waited) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    (void)updateStats(canonicalOutputPath, [&](TrellisStats& stats) {
        stats.servedCount += 1;
        if (waited) {
            stats.waitCount += 1;
        }
        stats.lastSource    = sourcePath;
        stats.lastErrorCode = 0;
        stats.lastUpdateNs  = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    });
}

void PathSpaceTrellis::recordServeError(std::string const& canonicalOutputPath, Error const& error) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    (void)updateStats(canonicalOutputPath, [&](TrellisStats& stats) {
        stats.errorCount += 1;
        stats.lastErrorCode = static_cast<std::int32_t>(error.code);
        stats.lastUpdateNs  = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    });
}

void PathSpaceTrellis::restorePersistedStatesLocked() {
    if (persistenceLoaded_ || !backing_) {
        persistenceLoaded_ = true;
        return;
    }

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto                   keys = backing_->listChildren(stateRoot);

    for (auto const& key : keys) {
        auto configPath = std::string{"/_system/trellis/state/"};
        configPath.append(key);
        configPath.append("/config");

        auto persisted = backing_->read<TrellisPersistedConfig>(configPath);
        if (!persisted) {
            auto code = persisted.error().code;
            if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
                continue;
            }
            continue;
        }

        auto canonicalOutput = canonicalizeAbsolute(persisted->name);
        if (!canonicalOutput) {
            continue;
        }
        if (trellis_.contains(canonicalOutput.value())) {
            continue;
        }

        auto canonicalSources = canonicalizeSourceList(persisted->sources);
        if (!canonicalSources) {
            continue;
        }
        auto mode = modeFromString(persisted->mode);
        if (!mode) {
            continue;
        }
        auto policy = policyFromString(persisted->policy);
        if (!policy) {
            continue;
        }

        auto state = std::make_shared<TrellisState>();
        state->mode    = mode.value();
        state->policy  = policy.value();
        state->sources = canonicalSources.value();
        state->roundRobinCursor = 0;
        state->shuttingDown     = false;

        trellis_.emplace(canonicalOutput.value(), std::move(state));
        // Ensure stats exist but preserve counters when already present.
        auto statsPath = stateStatsPathFor(canonicalOutput.value());
        auto statsExisting = backing_->read<TrellisStats>(statsPath);
        if (!statsExisting) {
            auto it = trellis_.find(canonicalOutput.value());
            if (it != trellis_.end()) {
                (void)persistStats(canonicalOutput.value(), *it->second);
            }
        }
    }

    persistenceLoaded_ = true;
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
    auto [it, inserted] = trellis_.emplace(parsed->outputPath, std::move(state));

    if (auto persistError = persistStateLocked(parsed->outputPath, *it->second)) {
        trellis_.erase(it);
        ret.errors.emplace_back(*persistError);
        return ret;
    }
    if (auto statsError = persistStats(parsed->outputPath, *it->second)) {
        trellis_.erase(it);
        ret.errors.emplace_back(*statsError);
        return ret;
    }

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
    if (auto persistError = erasePersistedState(parsed.value())) {
        ret.errors.emplace_back(*persistError);
    }
    if (auto statsError = eraseStats(parsed.value())) {
        ret.errors.emplace_back(*statsError);
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
                                     void* obj,
                                     std::optional<std::string>& servicedSource) -> std::optional<Error> {
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
            servicedSource = sourcesCopy[index];
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
                                         std::chrono::system_clock::time_point deadline,
                                         std::optional<std::string>& servicedSource) -> std::optional<Error> {
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
        servicedSource = sourcesCopy[waitIndex];
        if (state.policy == TrellisPolicy::RoundRobin) {
            std::lock_guard<std::mutex> lg(state.mutex);
            state.roundRobinCursor = (waitIndex + 1) % sourcesCopy.size();
        }
        return std::nullopt;
    }
    return err;
}

auto PathSpaceTrellis::waitAndServeLatest(TrellisState& state,
                                          InputMetadata const& inputMetadata,
                                          Out const& options,
                                          void* obj,
                                          std::chrono::system_clock::time_point deadline,
                                          std::optional<std::string>& servicedSource) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }

    std::vector<std::string> sourcesCopy;
    std::size_t              waitIndex = 0;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
        sourcesCopy = state.sources;
        if (!sourcesCopy.empty() && state.policy == TrellisPolicy::RoundRobin) {
            waitIndex = state.roundRobinCursor % sourcesCopy.size();
        }
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

    auto blockingOptions   = options;
    blockingOptions.doBlock = true;
    blockingOptions.doPop   = false; // Latest mode is non-destructive.
    blockingOptions.timeout = remaining;

    Iterator sourceIter{sourcesCopy[waitIndex]};
    auto     err = backing_->out(sourceIter, inputMetadata, blockingOptions, obj);
    if (!err) {
        servicedSource = sourcesCopy[waitIndex];
        if (state.policy == TrellisPolicy::RoundRobin) {
            std::lock_guard<std::mutex> lg(state.mutex);
            if (!state.sources.empty()) {
                state.roundRobinCursor = (waitIndex + 1) % state.sources.size();
            }
        }
        return std::nullopt;
    }
    return err;
}

auto PathSpaceTrellis::serveLatest(TrellisState& state,
                                   InputMetadata const& inputMetadata,
                                   Out const& options,
                                   void* obj,
                                   std::optional<std::string>& servicedSource) -> std::optional<Error> {
    // Latest mode performs a non-destructive read across sources, selecting a candidate
    // using the configured policy. Round-robin rotates through sources that reported data
    // so repeated reads can observe other producers without clearing the backing queues.
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
    }

    std::vector<std::string> sourcesCopy;
    std::size_t              startIndex = 0;
    TrellisPolicy            policy;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        sourcesCopy = state.sources;
        policy      = state.policy;
        if (!sourcesCopy.empty() && state.policy == TrellisPolicy::RoundRobin) {
            startIndex = state.roundRobinCursor % sourcesCopy.size();
        }
    }

    if (sourcesCopy.empty()) {
        return Error{Error::Code::NotFound, "No sources configured"};
    }

    auto attemptOptions   = options;
    attemptOptions.doBlock = false;
    attemptOptions.doPop   = false; // Latest mode is intentionally non-destructive.

    std::optional<Error> lastError;

    auto advanceCursor = [&](std::size_t index) {
        if (policy == TrellisPolicy::RoundRobin) {
            std::lock_guard<std::mutex> lg(state.mutex);
            if (!state.sources.empty()) {
                state.roundRobinCursor = (index + 1) % state.sources.size();
            }
        }
    };

    auto visitIndex = [&](std::size_t idx) -> std::optional<Error> {
        Iterator sourceIter{sourcesCopy[idx]};
        auto     err = backing_->out(sourceIter, inputMetadata, attemptOptions, obj);
        if (!err) {
            servicedSource = sourcesCopy[idx];
            advanceCursor(idx);
            return std::nullopt;
        }
        if (err->code != Error::Code::NoObjectFound
            && err->code != Error::Code::NotFound
            && err->code != Error::Code::NoSuchPath) {
            return err;
        }
        lastError = err;
        return err;
    };

    if (policy == TrellisPolicy::RoundRobin) {
        for (std::size_t offset = 0; offset < sourcesCopy.size(); ++offset) {
            auto index = (startIndex + offset) % sourcesCopy.size();
            if (auto res = visitIndex(index); !res.has_value()) {
                return std::nullopt;
            } else if (res->code != Error::Code::NoObjectFound
                       && res->code != Error::Code::NotFound
                       && res->code != Error::Code::NoSuchPath) {
                return res;
            }
        }
    } else {
        for (std::size_t index = 0; index < sourcesCopy.size(); ++index) {
            if (auto res = visitIndex(index); !res.has_value()) {
                return std::nullopt;
            } else if (res->code != Error::Code::NoObjectFound
                       && res->code != Error::Code::NotFound
                       && res->code != Error::Code::NoSuchPath) {
                return res;
            }
        }
    }

    return lastError.value_or(Error{Error::Code::NoObjectFound, "No data available in sources"});
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
    std::string const trellisPath = absolutePath;

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
        std::optional<std::string> servicedSource;
        auto result = tryServeQueue(*state, inputMetadata, options, obj, servicedSource);
        if (!result) {
            if (servicedSource) {
                recordServeSuccess(trellisPath, *servicedSource, false);
            }
            return result;
        }
        if (!options.doBlock) {
            if (result->code != Error::Code::NoObjectFound
                && result->code != Error::Code::NotFound
                && result->code != Error::Code::NoSuchPath) {
                recordServeError(trellisPath, *result);
            }
            return result;
        }
        std::optional<std::string> waitedSource;
        auto waitResult = waitAndServeQueue(*state, inputMetadata, options, obj, deadline, waitedSource);
        if (!waitResult) {
            if (waitedSource) {
                recordServeSuccess(trellisPath, *waitedSource, true);
            }
        } else {
            if (waitResult->code != Error::Code::NoObjectFound
                && waitResult->code != Error::Code::NotFound
                && waitResult->code != Error::Code::NoSuchPath) {
                recordServeError(trellisPath, *waitResult);
            }
        }
        return waitResult;
    }

    std::optional<std::string> servicedSource;
    auto result = serveLatest(*state, inputMetadata, options, obj, servicedSource);
    if (!result) {
        if (servicedSource) {
            recordServeSuccess(trellisPath, *servicedSource, false);
        }
        return result;
    }
    if (!options.doBlock) {
        if (result->code != Error::Code::NoObjectFound
            && result->code != Error::Code::NotFound
            && result->code != Error::Code::NoSuchPath) {
            recordServeError(trellisPath, *result);
        }
        return result;
    }
    std::optional<std::string> waitedSource;
    auto waitResult = waitAndServeLatest(*state, inputMetadata, options, obj, deadline, waitedSource);
    if (!waitResult) {
        if (waitedSource) {
            recordServeSuccess(trellisPath, *waitedSource, true);
        }
    } else {
        if (waitResult->code != Error::Code::NoObjectFound
            && waitResult->code != Error::Code::NotFound
            && waitResult->code != Error::Code::NoSuchPath) {
            recordServeError(trellisPath, *waitResult);
        }
    }
    return waitResult;
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

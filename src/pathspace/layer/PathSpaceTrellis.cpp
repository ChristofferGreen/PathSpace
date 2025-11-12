#include "PathSpaceTrellis.hpp"

#include "core/Error.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <sstream>
#include <string_view>
#include <vector>
#include <array>

namespace SP {

namespace {

auto toLowerCopy(std::string_view value) -> std::string {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

auto trimCopy(std::string_view value) -> std::string {
    auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

auto splitCommaSeparated(std::string_view value) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::size_t              start = 0;
    while (start <= value.size()) {
        auto pos = value.find(',', start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(trimCopy(value.substr(start)));
            break;
        }
        parts.emplace_back(trimCopy(value.substr(start, pos - start)));
        start = pos + 1;
    }
    return parts;
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

namespace {
constexpr auto kMaxLatestWaitSlice = std::chrono::milliseconds(20);
}

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
    (void)this->getNotificationSink();
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

auto PathSpaceTrellis::stateBackpressurePathFor(std::string const& canonicalOutputPath) -> std::string {
    auto key = encodeStateKey(canonicalOutputPath);
    std::string path = "/_system/trellis/state/";
    path.append(key);
    path.append("/backpressure/max_waiters");
    return path;
}

auto PathSpaceTrellis::legacyStateBackpressurePathFor(std::string const& canonicalOutputPath) -> std::string {
    auto key = encodeStateKey(canonicalOutputPath);
    std::string path = "/_system/trellis/state/";
    path.append(key);
    path.append("/config/max_waiters");
    return path;
}

auto PathSpaceTrellis::stateBufferedReadyPathFor(std::string const& canonicalOutputPath) -> std::string {
    auto key = encodeStateKey(canonicalOutputPath);
    std::string path = "/_system/trellis/state/";
    path.append(key);
    path.append("/stats/buffered_ready");
    return path;
}

auto PathSpaceTrellis::stateTracePathFor(std::string const& canonicalOutputPath) -> std::string {
    auto key = encodeStateKey(canonicalOutputPath);
    std::string path = "/_system/trellis/state/";
    path.append(key);
    path.append("/stats/latest_trace");
    return path;
}

auto PathSpaceTrellis::stateRootPathFor(std::string const& canonicalOutputPath) -> std::string {
    auto key = encodeStateKey(canonicalOutputPath);
    std::string path = "/_system/trellis/state/";
    path.append(key);
    return path;
}

auto PathSpaceTrellis::formatDurationMs(std::chrono::milliseconds value) -> std::string {
    std::ostringstream oss;
    oss << value.count();
    return oss.str();
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
    auto policyParts = splitCommaSeparated(command->policy);
    if (policyParts.empty() || policyParts.front().empty()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Trellis policy must be specified"});
    }
    auto policyString = toLowerCopy(policyParts.front());
    std::size_t maxWaiters = 0;

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

    for (std::size_t idx = 1; idx < policyParts.size(); ++idx) {
        auto const& token = policyParts[idx];
        if (token.empty()) continue;
        auto lowered = toLowerCopy(token);
        constexpr std::string_view kMaxWaitersPrefix = "max_waiters=";
        if (lowered.starts_with(kMaxWaitersPrefix)) {
            auto valueStr = token.substr(kMaxWaitersPrefix.size());
            try {
                maxWaiters = static_cast<std::size_t>(std::stoul(valueStr));
            } catch (...) {
                return std::unexpected(Error{Error::Code::MalformedInput,
                                             "Invalid max_waiters value in policy: " + token});
            }
        } else {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                         "Unsupported policy modifier: " + token});
        }
    }

    EnableParseResult result{
        .outputPath  = std::move(outputPath.value()),
        .mode        = mode,
        .policy      = policy,
        .sources     = sources,
        .maxWaitersPerSource = maxWaiters,
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
        if (code == Error::Code::InvalidType) {
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

    auto attemptInsert = [&]() -> std::optional<Error> {
        auto insertResult = backing_->insert(configPath, config);
        if (!insertResult.errors.empty()) {
            return insertResult.errors.front();
        }
        return std::nullopt;
    };

    if (auto insertError = attemptInsert()) {
        if (insertError->code == Error::Code::InvalidPathSubcomponent) {
            if (auto legacyClear = clearLegacyStateNode(canonicalOutputPath)) {
                return legacyClear;
            }
            if (auto retryError = attemptInsert()) {
                return retryError;
            }
        } else {
            return insertError;
        }
    }

    if (auto legacyClear = clearLegacyStateNode(canonicalOutputPath)) {
        return legacyClear;
    }

    auto const limitPath = stateBackpressurePathFor(canonicalOutputPath);
    while (true) {
        auto existingLimit = backing_->take<std::uint32_t>(limitPath);
        if (existingLimit) {
            continue;
        }
        auto const code = existingLimit.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::string>(limitPath);
            if (legacy) {
                continue;
            }
            auto legacyCode = legacy.error().code;
            if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                break;
            }
            return legacy.error();
        }
        return existingLimit.error();
    }
    auto limitInsert = backing_->insert(limitPath, static_cast<std::uint32_t>(state.maxWaitersPerSource));
    if (!limitInsert.errors.empty()) {
        return limitInsert.errors.front();
    }
    return clearLegacyStateNode(canonicalOutputPath);
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
    auto const limitPath = stateBackpressurePathFor(canonicalOutputPath);
    while (true) {
        auto existingLimit = backing_->take<std::uint32_t>(limitPath);
        if (existingLimit) {
            continue;
        }
        auto const code = existingLimit.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::string>(limitPath);
            if (legacy) {
                continue;
            }
            auto legacyCode = legacy.error().code;
            if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                break;
            }
            return legacy.error();
        }
        return existingLimit.error();
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
        .backpressureCount = 0,
        .lastSource    = std::string{},
        .lastErrorCode = 0,
        .lastUpdateNs  = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),
    };

    auto insertResult = backing_->insert(statsPath, stats);
    if (!insertResult.errors.empty()) {
        return insertResult.errors.front();
    }
    std::size_t readyCountSnapshot = 0;
    {
        std::lock_guard<std::mutex> readyLock(state.mutex);
        readyCountSnapshot = state.readySources.size();
    }
    updateBufferedReadyStats(canonicalOutputPath, readyCountSnapshot);
    return std::nullopt;
}

auto PathSpaceTrellis::persistTraceSnapshot(std::string const& canonicalOutputPath,
                                            std::vector<TrellisTraceEvent> const& snapshot)
    -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    auto const tracePath = stateTracePathFor(canonicalOutputPath);

    while (true) {
        auto existing = backing_->take<TrellisTraceSnapshot>(tracePath);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::vector<std::string>>(tracePath);
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

    TrellisTraceSnapshot payload{
        .events = snapshot,
    };
    auto insertResult = backing_->insert(tracePath, payload);
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
    auto bufferedPath = stateBufferedReadyPathFor(canonicalOutputPath);
    while (true) {
        auto existing = backing_->take<std::uint64_t>(bufferedPath);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::string>(bufferedPath);
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
    if (auto traceError = eraseTraceSnapshot(canonicalOutputPath)) {
        return traceError;
    }
    return std::nullopt;
}

auto PathSpaceTrellis::eraseTraceSnapshot(std::string const& canonicalOutputPath) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    auto const tracePath = stateTracePathFor(canonicalOutputPath);
    while (true) {
        auto existing = backing_->take<TrellisTraceSnapshot>(tracePath);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::vector<std::string>>(tracePath);
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
        stats.backpressureCount = 0;
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
    updateBufferedReadyStats(canonicalOutputPath);
}

void PathSpaceTrellis::recordServeError(std::string const& canonicalOutputPath, Error const& error) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    (void)updateStats(canonicalOutputPath, [&](TrellisStats& stats) {
        stats.errorCount += 1;
        if (error.code == Error::Code::CapacityExceeded) {
            stats.backpressureCount += 1;
        }
        stats.lastErrorCode = static_cast<std::int32_t>(error.code);
        stats.lastUpdateNs  = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    });
    updateBufferedReadyStats(canonicalOutputPath);
}

void PathSpaceTrellis::updateBufferedReadyStats(std::string const& canonicalOutputPath,
                                                std::optional<std::size_t> readyCountOverride) {
    if (!backing_) {
        return;
    }
    auto readyCount = readyCountOverride.value_or(bufferedReadyCount(canonicalOutputPath));
    auto path       = stateBufferedReadyPathFor(canonicalOutputPath);
    while (true) {
        auto existing = backing_->take<std::uint64_t>(path);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
            break;
        }
        if (code == Error::Code::InvalidType) {
            auto legacy = backing_->take<std::string>(path);
            if (legacy) {
                continue;
            }
            auto legacyCode = legacy.error().code;
            if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                break;
            }
            return;
        }
        return;
    }
    (void)backing_->insert(path, static_cast<std::uint64_t>(readyCount));
}

std::optional<std::string> PathSpaceTrellis::pickReadySource(TrellisState& state) {
    std::lock_guard<std::mutex> lg(state.mutex);
    while (!state.readySources.empty()) {
        auto source = std::move(state.readySources.front());
        state.readySources.pop_front();
        state.readySourceSet.erase(source);
        if (std::find(state.sources.begin(), state.sources.end(), source) != state.sources.end()) {
            return source;
        }
    }
    return std::nullopt;
}

bool PathSpaceTrellis::enqueueReadySource(TrellisState& state, std::string const& source) {
    if (state.readySourceSet.contains(source)) {
        return false;
    }
    state.readySources.push_back(source);
    state.readySourceSet.insert(source);
    return true;
}

void PathSpaceTrellis::appendTraceEvent(std::string const& canonicalOutputPath,
                                        TrellisState& state,
                                        std::string message) {
    if (!backing_) {
        return;
    }
    auto now       = std::chrono::system_clock::now();
    auto timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

    std::vector<TrellisTraceEvent> snapshot;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.trace.size() >= kTraceCapacity) {
            state.trace.pop_front();
        }
        state.trace.push_back(TrellisTraceEvent{
            .timestampNs = timestamp,
            .message     = std::move(message),
        });
        snapshot.assign(state.trace.begin(), state.trace.end());
    }
    (void)persistTraceSnapshot(canonicalOutputPath, snapshot);
}

std::size_t PathSpaceTrellis::bufferedReadyCount(std::string const& canonicalOutputPath) {
    std::shared_ptr<TrellisState> state;
    {
        std::lock_guard<std::mutex> lg(statesMutex_);
        auto it = trellis_.find(canonicalOutputPath);
        if (it != trellis_.end()) {
            state = it->second;
        }
    }
    if (!state) {
        return 0;
    }
    std::lock_guard<std::mutex> lg(state->mutex);
    return state->readySources.size();
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
        auto limitPath = stateBackpressurePathFor(canonicalOutput.value());
        auto limitRead = backing_->read<std::uint32_t>(limitPath);
        bool migratedLegacyLimit = false;
        if (limitRead) {
            state->maxWaitersPerSource = limitRead.value();
        } else if (limitRead.error().code == Error::Code::InvalidType) {
            (void)backing_->take<std::string>(limitPath);
            state->maxWaitersPerSource = 0;
        } else {
            auto legacyLimitPath = legacyStateBackpressurePathFor(canonicalOutput.value());
            auto legacyLimitRead = backing_->read<std::uint32_t>(legacyLimitPath);
            if (legacyLimitRead) {
                state->maxWaitersPerSource = legacyLimitRead.value();
                migratedLegacyLimit        = true;
            } else if (legacyLimitRead.error().code == Error::Code::InvalidType) {
                (void)backing_->take<std::string>(legacyLimitPath);
                state->maxWaitersPerSource = 0;
                migratedLegacyLimit        = true;
            } else {
                auto fallbackPath = stateRootPathFor(canonicalOutput.value());
                fallbackPath.append("/max_waiters");
                auto fallbackRead = backing_->read<std::uint32_t>(fallbackPath);
                if (fallbackRead) {
                    state->maxWaitersPerSource = fallbackRead.value();
                    migratedLegacyLimit        = true;
                } else if (fallbackRead.error().code == Error::Code::InvalidType) {
                    (void)backing_->take<std::string>(fallbackPath);
                    state->maxWaitersPerSource = 0;
                    migratedLegacyLimit        = true;
                } else {
                    state->maxWaitersPerSource = 0;
                }
            }
        }

        trellis_.emplace(canonicalOutput.value(), std::move(state));
        if (migratedLegacyLimit) {
            auto it = trellis_.find(canonicalOutput.value());
            if (it != trellis_.end()) {
                (void)clearLegacyStateNode(canonicalOutput.value());
                (void)persistStateLocked(canonicalOutput.value(), *it->second);
            }
        }
        updateBufferedReadyStats(canonicalOutput.value(), 0);
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

auto PathSpaceTrellis::clearLegacyStateNode(std::string const& canonicalOutputPath,
                                            bool removeActiveConfig) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    auto const rootPath          = stateRootPathFor(canonicalOutputPath);
    auto const legacyConfigPath  = rootPath + "/config";
    auto const legacyLimitPaths  = std::array{
        legacyStateBackpressurePathFor(canonicalOutputPath),
        rootPath + "/max_waiters",
    };

    auto clearRootValue = [&]() -> std::optional<Error> {
        while (true) {
            auto existing = backing_->take<std::string>(rootPath);
            if (existing) {
                continue;
            }
            auto const code = existing.error().code;
            if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
                break;
            }
            if (code == Error::Code::InvalidType) {
                auto legacyConfig = backing_->take<TrellisPersistedConfig>(rootPath);
                if (legacyConfig) {
                    continue;
                }
                auto const legacyCode = legacyConfig.error().code;
                if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                    break;
                }
                if (legacyCode == Error::Code::InvalidType) {
                    auto legacyLimit = backing_->take<std::uint32_t>(rootPath);
                    if (legacyLimit) {
                        continue;
                    }
                    auto const limitCode = legacyLimit.error().code;
                    if (limitCode == Error::Code::NoObjectFound || limitCode == Error::Code::NoSuchPath) {
                        break;
                    }
                    return legacyLimit.error();
                }
                return legacyConfig.error();
            }
            return existing.error();
        }
        return std::nullopt;
    };

    auto clearConfigValue = [&]() -> std::optional<Error> {
        if (removeActiveConfig) {
            while (true) {
                auto existing = backing_->take<TrellisPersistedConfig>(legacyConfigPath);
                if (existing) {
                    continue;
                }
                auto const code = existing.error().code;
                if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath
                    || code == Error::Code::InvalidPathSubcomponent) {
                    break;
                }
                if (code == Error::Code::InvalidType) {
                    auto legacyString = backing_->take<std::string>(legacyConfigPath);
                    if (legacyString) {
                        continue;
                    }
                    auto const legacyCode = legacyString.error().code;
                    if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath
                        || legacyCode == Error::Code::InvalidPathSubcomponent) {
                        break;
                    }
                    return legacyString.error();
                }
                return existing.error();
            }
            return std::nullopt;
        }
        while (true) {
            auto existing = backing_->read<TrellisPersistedConfig>(legacyConfigPath);
            if (existing) {
                return std::nullopt;
            }
            auto const code = existing.error().code;
            if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
                return std::nullopt;
            }
            if (code == Error::Code::InvalidType) {
                while (true) {
                    auto legacyString = backing_->take<std::string>(legacyConfigPath);
                    if (legacyString) {
                        continue;
                    }
                    auto const legacyCode = legacyString.error().code;
                    if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath) {
                        break;
                    }
                    if (legacyCode == Error::Code::InvalidType) {
                        auto legacyLimit = backing_->take<std::uint32_t>(legacyConfigPath);
                        if (legacyLimit) {
                            continue;
                        }
                        auto const limitCode = legacyLimit.error().code;
                        if (limitCode == Error::Code::NoObjectFound || limitCode == Error::Code::NoSuchPath) {
                            break;
                        }
                        return legacyLimit.error();
                    }
                    return legacyString.error();
                }
                continue;
            }
            if (code == Error::Code::InvalidPathSubcomponent) {
                if (auto rootClear = clearRootValue()) {
                    return rootClear;
                }
                continue;
            }
            return existing.error();
        }
    };

    auto clearLimitValue = [&](std::string const& path) -> std::optional<Error> {
        while (true) {
            auto existing = backing_->take<std::uint32_t>(path);
            if (existing) {
                continue;
            }
            auto const code = existing.error().code;
            if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath
                || code == Error::Code::InvalidPathSubcomponent) {
                break;
            }
            if (code == Error::Code::InvalidType) {
                auto legacyString = backing_->take<std::string>(path);
                if (legacyString) {
                    continue;
                }
                auto const legacyCode = legacyString.error().code;
                if (legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath
                    || legacyCode == Error::Code::InvalidPathSubcomponent) {
                    break;
                }
                return legacyString.error();
            }
            return existing.error();
        }
        return std::nullopt;
    };

    if (auto rootError = clearRootValue()) {
        return rootError;
    }
    if (auto configError = clearConfigValue()) {
        return configError;
    }
    for (auto const& path : legacyLimitPaths) {
        if (auto limitError = clearLimitValue(path)) {
            return limitError;
        }
    }
    return std::nullopt;
}

auto PathSpaceTrellis::handleEnable(InputData const& data) -> InsertReturn {
    InsertReturn ret;
    auto parsed = parseEnableCommand(data);
    if (!parsed) {
        ret.errors.emplace_back(parsed.error());
        return ret;
    }

    std::unique_lock<std::mutex> statesLock(statesMutex_);
    if (trellis_.contains(parsed->outputPath)) {
        ret.errors.emplace_back(Error{Error::Code::InvalidPath, "Trellis already enabled for path"});
        return ret;
    }

    auto statePtr = std::make_shared<TrellisState>();
    statePtr->mode             = parsed->mode;
    statePtr->policy           = parsed->policy;
    statePtr->sources          = parsed->sources;
    statePtr->roundRobinCursor = 0;

    auto [it, inserted] = trellis_.emplace(parsed->outputPath, statePtr);
    if (!inserted) {
        statesLock.unlock();
        ret.errors.emplace_back(Error{Error::Code::InvalidPath, "Trellis already enabled for path"});
        return ret;
    }

    statesLock.unlock();

    if (auto persistError = persistStateLocked(parsed->outputPath, *statePtr)) {
        std::lock_guard<std::mutex> eraseLock(statesMutex_);
        trellis_.erase(parsed->outputPath);
        ret.errors.emplace_back(*persistError);
        return ret;
    }
    if (auto statsError = persistStats(parsed->outputPath, *statePtr)) {
        std::lock_guard<std::mutex> eraseLock(statesMutex_);
        trellis_.erase(parsed->outputPath);
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
                                     std::string const& canonicalOutputPath,
                                     InputMetadata const& inputMetadata,
                                     Out const& options,
                                     void* obj,
                                     std::optional<std::string>& servicedSource) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }
    if (auto readySource = pickReadySource(state)) {
        updateBufferedReadyStats(canonicalOutputPath);
        auto attemptOptions   = options;
        attemptOptions.doBlock = false;
        Iterator sourceIter{*readySource};
        auto     err = backing_->out(sourceIter, inputMetadata, attemptOptions, obj);
        if (!err) {
            {
                std::ostringstream readyMsg;
                readyMsg << "serve_queue.ready"
                         << " src=" << *readySource
                         << " outcome=success";
                appendTraceEvent(canonicalOutputPath, state, readyMsg.str());
            }
            servicedSource = *readySource;
            if (state.policy == TrellisPolicy::RoundRobin) {
                std::lock_guard<std::mutex> lg(state.mutex);
                auto it = std::find(state.sources.begin(), state.sources.end(), *readySource);
                if (it != state.sources.end()) {
                    state.roundRobinCursor = static_cast<std::size_t>(std::distance(state.sources.begin(), it) + 1)
                                             % state.sources.size();
                }
            }
            return std::nullopt;
        }
        if (err->code != Error::Code::NoObjectFound
            && err->code != Error::Code::NotFound
            && err->code != Error::Code::NoSuchPath) {
            std::ostringstream readyErr;
            readyErr << "serve_queue.ready"
                     << " src=" << *readySource
                     << " outcome=error"
                     << " error=" << describeError(*err);
            appendTraceEvent(canonicalOutputPath, state, readyErr.str());
            return err;
        }
        std::ostringstream readyEmpty;
        readyEmpty << "serve_queue.ready"
                   << " src=" << *readySource
                   << " outcome=empty";
        appendTraceEvent(canonicalOutputPath, state, readyEmpty.str());
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
            {
                std::ostringstream successMsg;
                successMsg << "serve_queue.result"
                           << " src=" << sourcesCopy[index]
                           << " outcome=success";
                appendTraceEvent(canonicalOutputPath, state, successMsg.str());
            }
            advanceCursor(index + 1);
            return std::nullopt;
        }
        if (err->code != Error::Code::NoObjectFound
            && err->code != Error::Code::NotFound
            && err->code != Error::Code::NoSuchPath) {
            std::ostringstream errMsg;
            errMsg << "serve_queue.result"
                   << " src=" << sourcesCopy[index]
                   << " outcome=error"
                   << " error=" << describeError(*err);
            appendTraceEvent(canonicalOutputPath, state, errMsg.str());
            return err;
        }
        {
            std::ostringstream emptyMsg;
            emptyMsg << "serve_queue.result"
                     << " src=" << sourcesCopy[index]
                     << " outcome=empty";
            appendTraceEvent(canonicalOutputPath, state, emptyMsg.str());
        }
        lastError = err;
    }
    return lastError.value_or(Error{Error::Code::NoObjectFound, "No data available in sources"});
}

auto PathSpaceTrellis::waitAndServeQueue(TrellisState& state,
                                         std::string const& canonicalOutputPath,
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

    auto const& selectedSource = sourcesCopy[waitIndex];
    auto registerWaiterIfNeeded = [&](std::string const& src) -> std::optional<Error> {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
        if (state.maxWaitersPerSource == 0) {
            return std::nullopt;
        }
        auto current = state.activeWaiters[src];
        if (current >= state.maxWaitersPerSource) {
            return Error{Error::Code::CapacityExceeded,
                         "Back-pressure: wait limit reached for source"};
        }
        state.activeWaiters[src] = current + 1;
        return std::nullopt;
    };
    auto unregisterWaiter = [&](std::string const& src) {
        if (state.maxWaitersPerSource == 0) {
            return;
        }
        std::lock_guard<std::mutex> lg(state.mutex);
        auto it = state.activeWaiters.find(src);
        if (it != state.activeWaiters.end()) {
            if (it->second <= 1) {
                state.activeWaiters.erase(it);
            } else {
                it->second -= 1;
            }
        }
    };

    if (auto registrationError = registerWaiterIfNeeded(selectedSource)) {
        return registrationError;
    }
    {
        std::ostringstream waitMsg;
        waitMsg << "wait_queue.block"
                << " src=" << selectedSource
                << " timeout_ms=" << formatDurationMs(remaining);
        appendTraceEvent(canonicalOutputPath, state, waitMsg.str());
    }
    auto unregisterGuard = std::unique_ptr<void, std::function<void(void*)>>(
        nullptr, [&](void*) { unregisterWaiter(selectedSource); });

    Iterator sourceIter{selectedSource};
    auto     err = backing_->out(sourceIter, inputMetadata, blockingOptions, obj);
    unregisterGuard.reset();
    if (!err) {
        servicedSource = selectedSource;
        if (state.policy == TrellisPolicy::RoundRobin) {
            std::lock_guard<std::mutex> lg(state.mutex);
            state.roundRobinCursor = (waitIndex + 1) % sourcesCopy.size();
        }
        {
            std::ostringstream successMsg;
            successMsg << "wait_queue.result"
                       << " src=" << selectedSource
                       << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, successMsg.str());
        }
        {
            std::ostringstream serveMsg;
            serveMsg << "serve_queue.result"
                     << " src=" << selectedSource
                     << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, serveMsg.str());
        }
        return std::nullopt;
    }
    if (err->code != Error::Code::NoObjectFound
        && err->code != Error::Code::NotFound
        && err->code != Error::Code::NoSuchPath) {
        std::ostringstream errMsg;
        errMsg << "wait_queue.result"
               << " src=" << selectedSource
               << " outcome=error"
               << " error=" << describeError(*err);
        appendTraceEvent(canonicalOutputPath, state, errMsg.str());
        std::ostringstream serveErr;
        serveErr << "serve_queue.result"
                 << " src=" << selectedSource
                 << " outcome=error"
                 << " error=" << describeError(*err);
        appendTraceEvent(canonicalOutputPath, state, serveErr.str());
    } else {
        std::ostringstream emptyMsg;
        emptyMsg << "wait_queue.result"
                 << " src=" << selectedSource
                 << " outcome=empty";
        appendTraceEvent(canonicalOutputPath, state, emptyMsg.str());
        std::ostringstream serveEmpty;
        serveEmpty << "serve_queue.result"
                   << " src=" << selectedSource
                   << " outcome=empty";
        appendTraceEvent(canonicalOutputPath, state, serveEmpty.str());
    }
    return err;
}

auto PathSpaceTrellis::waitAndServeLatest(TrellisState& state,
                                          std::string const& canonicalOutputPath,
                                          InputMetadata const& inputMetadata,
                                          Out const& options,
                                          void* obj,
                                          std::chrono::system_clock::time_point deadline,
                                          std::optional<std::string>& servicedSource) -> std::optional<Error> {
    if (!backing_) {
        return Error{Error::Code::InvalidPermissions, "No backing PathSpace configured"};
    }

    std::vector<std::string> sourcesCopy;
    std::size_t              startIndex = 0;
    TrellisPolicy            policySnapshot = TrellisPolicy::RoundRobin;
    {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
        sourcesCopy = state.sources;
        policySnapshot = state.policy;
        if (!sourcesCopy.empty() && policySnapshot == TrellisPolicy::RoundRobin) {
            startIndex = state.roundRobinCursor % sourcesCopy.size();
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

    auto const policyLabel = policyToString(policySnapshot);

    auto registerWaiterIfNeeded = [&](std::string const& src) -> std::optional<Error> {
        std::lock_guard<std::mutex> lg(state.mutex);
        if (state.shuttingDown) {
            return Error{Error::Code::Timeout, "Trellis is shutting down"};
        }
        if (state.maxWaitersPerSource == 0) {
            return std::nullopt;
        }
        auto current = state.activeWaiters[src];
        if (current >= state.maxWaitersPerSource) {
            return Error{Error::Code::CapacityExceeded,
                         "Back-pressure: wait limit reached for source"};
        }
        state.activeWaiters[src] = current + 1;
        return std::nullopt;
    };
    auto unregisterWaiter = [&](std::string const& src) {
        if (state.maxWaitersPerSource == 0) {
            return;
        }
        std::lock_guard<std::mutex> lg(state.mutex);
        auto it = state.activeWaiters.find(src);
        if (it != state.activeWaiters.end()) {
            if (it->second <= 1) {
                state.activeWaiters.erase(it);
            } else {
                it->second -= 1;
            }
        }
    };

    for (std::size_t attempt = 0; attempt < sourcesCopy.size(); ++attempt) {
        if (auto ready = pickReadySource(state)) {
            updateBufferedReadyStats(canonicalOutputPath);
            auto readyOptions   = options;
            readyOptions.doBlock = false;
            readyOptions.doPop   = false;
            Iterator readyIter{*ready};
            auto     readyErr = backing_->out(readyIter, inputMetadata, readyOptions, obj);
            if (!readyErr) {
                servicedSource = *ready;
                if (policySnapshot == TrellisPolicy::RoundRobin) {
                    auto it = std::find(sourcesCopy.begin(), sourcesCopy.end(), *ready);
                    if (it != sourcesCopy.end()) {
                        std::lock_guard<std::mutex> lg(state.mutex);
                        if (!state.sources.empty()) {
                            state.roundRobinCursor
                                = (static_cast<std::size_t>(std::distance(sourcesCopy.begin(), it)) + 1)
                                  % state.sources.size();
                        }
                    }
                }
                std::ostringstream readyMsg;
                readyMsg << "wait_latest.ready"
                         << " policy=" << policyLabel
                         << " src=" << *ready
                         << " outcome=success";
                appendTraceEvent(canonicalOutputPath, state, readyMsg.str());
                std::ostringstream notifyMsg;
                notifyMsg << "notify.ready"
                          << " output=" << canonicalOutputPath
                          << " src=" << *ready;
                appendTraceEvent(canonicalOutputPath, state, notifyMsg.str());
                return std::nullopt;
            }
            if (readyErr->code != Error::Code::Timeout
                && readyErr->code != Error::Code::NoObjectFound
                && readyErr->code != Error::Code::NotFound
                && readyErr->code != Error::Code::NoSuchPath) {
                std::ostringstream readyError;
                readyError << "wait_latest.ready"
                           << " policy=" << policyLabel
                           << " src=" << *ready
                           << " outcome=error"
                           << " error=" << describeError(*readyErr);
                appendTraceEvent(canonicalOutputPath, state, readyError.str());
                return readyErr;
            }
        }

        auto index = (startIndex + attempt) % sourcesCopy.size();
        auto const& selectedSource = sourcesCopy[index];

        auto now = std::chrono::system_clock::now();
        if (now >= deadline) {
            break;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining <= std::chrono::milliseconds::zero()) {
            break;
        }

        auto blockingOptions   = options;
        blockingOptions.doBlock = true;
        blockingOptions.doPop   = false; // Latest mode is non-destructive.
        auto slotsRemaining      = static_cast<long>(sourcesCopy.size() - attempt);
        if (slotsRemaining < 1) {
            slotsRemaining = 1;
        }
        auto perSourceTimeout = remaining / slotsRemaining;
        if (slotsRemaining == 1) {
            perSourceTimeout = remaining;
        }
        if (perSourceTimeout <= std::chrono::milliseconds::zero()) {
            perSourceTimeout = std::chrono::milliseconds(1);
        }
        if (slotsRemaining > 1 && perSourceTimeout > kMaxLatestWaitSlice) {
            perSourceTimeout = kMaxLatestWaitSlice;
        }
        blockingOptions.timeout = perSourceTimeout;

        if (auto registrationError = registerWaiterIfNeeded(selectedSource)) {
            std::ostringstream failed;
            failed << "wait_latest.register_failed"
                   << " policy=" << policyLabel
                   << " src=" << selectedSource
                   << " error=" << describeError(*registrationError);
            appendTraceEvent(canonicalOutputPath, state, failed.str());
            if (registrationError->code != Error::Code::CapacityExceeded) {
                return registrationError;
            }
            continue;
        }
        {
            std::ostringstream waiting;
            waiting << "wait_latest.block"
                    << " policy=" << policyLabel
                    << " src=" << selectedSource
                    << " attempt=" << (attempt + 1) << "/" << sourcesCopy.size()
                    << " remaining_ms=" << formatDurationMs(remaining)
                    << " timeout_ms=" << formatDurationMs(perSourceTimeout);
            appendTraceEvent(canonicalOutputPath, state, waiting.str());
        }
        auto unregisterGuard = std::unique_ptr<void, std::function<void(void*)>>(
            nullptr, [&](void*) { unregisterWaiter(selectedSource); });

        Iterator sourceIter{selectedSource};
        auto     err = backing_->out(sourceIter, inputMetadata, blockingOptions, obj);
        unregisterGuard.reset();
        if (!err) {
            servicedSource = selectedSource;
            if (state.policy == TrellisPolicy::RoundRobin) {
                std::lock_guard<std::mutex> lg(state.mutex);
                if (!state.sources.empty()) {
                    state.roundRobinCursor = (index + 1) % state.sources.size();
                }
            }
            std::ostringstream readyMsg;
            readyMsg << "wait_latest.ready"
                     << " policy=" << policyLabel
                     << " src=" << selectedSource
                     << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, readyMsg.str());
            std::ostringstream notifyMsg;
            notifyMsg << "notify.ready"
                      << " output=" << canonicalOutputPath
                      << " src=" << selectedSource;
            appendTraceEvent(canonicalOutputPath, state, notifyMsg.str());
            std::ostringstream success;
            success << "wait_latest.result"
                    << " policy=" << policyLabel
                    << " src=" << selectedSource
                    << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, success.str());
            return std::nullopt;
        }
        if (err->code == Error::Code::Timeout) {
            std::ostringstream timeoutMsg;
            timeoutMsg << "wait_latest.result"
                       << " policy=" << policyLabel
                       << " src=" << selectedSource
                       << " outcome=timeout";
            appendTraceEvent(canonicalOutputPath, state, timeoutMsg.str());
            continue;
        }
        if (err->code == Error::Code::NoObjectFound
            || err->code == Error::Code::NotFound
            || err->code == Error::Code::NoSuchPath) {
            std::ostringstream emptyMsg;
            emptyMsg << "wait_latest.result"
                     << " policy=" << policyLabel
                     << " src=" << selectedSource
                     << " outcome=empty"
                     << " error=" << describeError(*err);
            appendTraceEvent(canonicalOutputPath, state, emptyMsg.str());
            continue;
        }
        std::ostringstream errorMsg;
        errorMsg << "wait_latest.result"
                 << " policy=" << policyLabel
                 << " src=" << selectedSource
                 << " outcome=error"
                 << " error=" << describeError(*err);
        appendTraceEvent(canonicalOutputPath, state, errorMsg.str());
        return err;
    }
    auto immediateOptions   = options;
    immediateOptions.doBlock = false;
    if (auto finalAttempt = serveLatest(state,
                                        canonicalOutputPath,
                                        inputMetadata,
                                        immediateOptions,
                                        obj,
                                        servicedSource)) {
        if (finalAttempt->code != Error::Code::NoObjectFound
            && finalAttempt->code != Error::Code::NotFound
            && finalAttempt->code != Error::Code::NoSuchPath) {
            return finalAttempt;
        }
    } else {
        if (servicedSource) {
            std::ostringstream readyMsg;
            readyMsg << "wait_latest.ready"
                     << " policy=" << policyLabel
                     << " src=" << *servicedSource
                     << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, readyMsg.str());
            std::ostringstream success;
            success << "wait_latest.result"
                    << " policy=" << policyLabel
                    << " src=" << *servicedSource
                    << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, success.str());
        }
        return std::nullopt;
    }
    appendTraceEvent(canonicalOutputPath,
                     state,
                     "wait_latest.result policy=" + policyLabel + " outcome=timeout");
    return Error{Error::Code::Timeout, "Trellis wait timed out"};
}

auto PathSpaceTrellis::serveLatest(TrellisState& state,
                                   std::string const& canonicalOutputPath,
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
        if (!sourcesCopy.empty() && policy == TrellisPolicy::RoundRobin) {
            startIndex = state.roundRobinCursor % sourcesCopy.size();
        }
    }

    if (sourcesCopy.empty()) {
        return Error{Error::Code::NotFound, "No sources configured"};
    }

    auto attemptOptions   = options;
    attemptOptions.doBlock = false;
    attemptOptions.doPop   = false; // Latest mode is intentionally non-destructive.

    auto const policyLabel = policyToString(policy);
    std::optional<Error> lastError;
    bool                 allowColdScan = !options.doBlock;

    auto advanceCursor = [&](std::size_t index) {
        if (policy == TrellisPolicy::RoundRobin) {
            std::lock_guard<std::mutex> lg(state.mutex);
            if (!state.sources.empty()) {
                state.roundRobinCursor = (index + 1) % state.sources.size();
            }
        }
    };

    if (auto readySource = pickReadySource(state)) {
        updateBufferedReadyStats(canonicalOutputPath);
        {
            std::ostringstream cached;
            cached << "serve_latest.cache"
                   << " policy=" << policyLabel
                   << " src=" << *readySource;
            appendTraceEvent(canonicalOutputPath, state, cached.str());
        }
        auto attemptOptions   = options;
        attemptOptions.doBlock = false;
        attemptOptions.doPop   = false;
        Iterator sourceIter{*readySource};
        auto     err = backing_->out(sourceIter, inputMetadata, attemptOptions, obj);
        if (!err) {
            servicedSource = *readySource;
            auto it = std::find(sourcesCopy.begin(), sourcesCopy.end(), *readySource);
            if (it != sourcesCopy.end()) {
                auto idx = static_cast<std::size_t>(std::distance(sourcesCopy.begin(), it));
                advanceCursor(idx);
            }
            std::ostringstream success;
            success << "serve_latest.result"
                    << " policy=" << policyLabel
                    << " src=" << *readySource
                    << " cached=true"
                    << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, success.str());
            std::ostringstream notifyMsg;
            notifyMsg << "notify.ready"
                      << " output=" << canonicalOutputPath
                      << " src=" << *readySource;
            appendTraceEvent(canonicalOutputPath, state, notifyMsg.str());
            return std::nullopt;
        }
        if (err->code != Error::Code::NoObjectFound
            && err->code != Error::Code::NotFound
            && err->code != Error::Code::NoSuchPath) {
            std::ostringstream errorMsg;
            errorMsg << "serve_latest.result"
                     << " policy=" << policyLabel
                     << " src=" << *readySource
                     << " cached=true"
                     << " outcome=error"
                     << " error=" << describeError(*err);
            appendTraceEvent(canonicalOutputPath, state, errorMsg.str());
            return err;
        }
        std::ostringstream emptyMsg;
        emptyMsg << "serve_latest.result"
                 << " policy=" << policyLabel
                 << " src=" << *readySource
                 << " cached=true"
                 << " outcome=empty"
                 << " error=" << describeError(*err);
        appendTraceEvent(canonicalOutputPath, state, emptyMsg.str());
    }

    auto visitIndex = [&](std::size_t idx) -> std::optional<Error> {
        Iterator sourceIter{sourcesCopy[idx]};
        auto     err = backing_->out(sourceIter, inputMetadata, attemptOptions, obj);
        if (!err) {
            servicedSource = sourcesCopy[idx];
            advanceCursor(idx);
            std::ostringstream success;
            success << "serve_latest.result"
                    << " policy=" << policyLabel
                    << " src=" << sourcesCopy[idx]
                    << " cached=false"
                    << " outcome=success";
            appendTraceEvent(canonicalOutputPath, state, success.str());
            std::ostringstream notifyMsg;
            notifyMsg << "notify.ready"
                      << " output=" << canonicalOutputPath
                      << " src=" << sourcesCopy[idx];
            appendTraceEvent(canonicalOutputPath, state, notifyMsg.str());
            return std::nullopt;
        }
        if (err->code != Error::Code::NoObjectFound
            && err->code != Error::Code::NotFound
            && err->code != Error::Code::NoSuchPath) {
            std::ostringstream errorMsg;
            errorMsg << "serve_latest.result"
                     << " policy=" << policyLabel
                     << " src=" << sourcesCopy[idx]
                     << " cached=false"
                     << " outcome=error"
                     << " error=" << describeError(*err);
            appendTraceEvent(canonicalOutputPath, state, errorMsg.str());
            return err;
        }
        lastError = err;
        std::ostringstream emptyMsg;
        emptyMsg << "serve_latest.result"
                 << " policy=" << policyLabel
                 << " src=" << sourcesCopy[idx]
                 << " cached=false"
                 << " outcome=empty"
                 << " error=" << describeError(*err);
        appendTraceEvent(canonicalOutputPath, state, emptyMsg.str());
        return err;
    };

    if (allowColdScan) {
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
    } else {
        lastError = Error{Error::Code::NoObjectFound, "No data available in sources"};
    }

    if (!allowColdScan) {
        appendTraceEvent(canonicalOutputPath,
                         state,
                         "serve_latest.result policy=" + policyLabel + " outcome=wait_required");
        return Error{Error::Code::NoObjectFound, "No data available in sources"};
    }

    if (!lastError.has_value()) {
        appendTraceEvent(canonicalOutputPath,
                         state,
                         "serve_latest.result policy=" + policyLabel + " outcome=no_data");
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
        auto result = tryServeQueue(*state, trellisPath, inputMetadata, options, obj, servicedSource);
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
        auto waitResult = waitAndServeQueue(*state, trellisPath, inputMetadata, options, obj, deadline, waitedSource);
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
    auto result = serveLatest(*state, trellisPath, inputMetadata, options, obj, servicedSource);
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
    auto waitResult = waitAndServeLatest(*state, trellisPath, inputMetadata, options, obj, deadline, waitedSource);
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
    struct TraceEntry {
        std::shared_ptr<TrellisState> state;
        std::string                   output;
        std::string                   source;
    };
    std::vector<std::string> outputsToUpdate;
    std::vector<TraceEntry>  tracesToRecord;

    auto canonical = canonicalizeAbsolute(notificationPath);
    if (canonical) {
        std::lock_guard<std::mutex> lg(statesMutex_);
        for (auto const& [outputPath, statePtr] : trellis_) {
            if (!statePtr) continue;
            bool enqueued = false;
            {
                std::lock_guard<std::mutex> stateLock(statePtr->mutex);
                if (std::find(statePtr->sources.begin(), statePtr->sources.end(), canonical.value())
                    != statePtr->sources.end()) {
                    enqueued = enqueueReadySource(*statePtr, canonical.value());
                }
            }
            if (enqueued) {
                outputsToUpdate.push_back(outputPath);
                tracesToRecord.push_back(TraceEntry{
                    .state  = statePtr,
                    .output = outputPath,
                    .source = canonical.value(),
                });
            }
        }
    }
    for (auto const& output : outputsToUpdate) {
        updateBufferedReadyStats(output);
    }
    for (auto& entry : tracesToRecord) {
        if (!entry.state) {
            continue;
        }
        std::ostringstream traceMsg;
        traceMsg << "notify.ready"
                 << " output=" << entry.output
                 << " src=" << entry.source;
        appendTraceEvent(entry.output, *entry.state, traceMsg.str());
    }
    if (auto ctx = this->getContext()) {
        for (auto const& output : outputsToUpdate) {
            ctx->notify(output);
        }
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

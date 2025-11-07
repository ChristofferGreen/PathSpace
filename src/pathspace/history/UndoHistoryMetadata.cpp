#include "history/UndoHistoryMetadata.hpp"

#include "history/UndoHistoryUtils.hpp"

#include <charconv>
#include <initializer_list>
#include <sstream>
#include <unordered_map>

namespace SP::History::UndoMetadata {

namespace {

using SP::Error;
using SP::Expected;

auto joinGenerations(std::vector<std::size_t> const& gens) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < gens.size(); ++i) {
        if (i > 0)
            oss << ',';
        oss << gens[i];
    }
    return oss.str();
}

auto parseGenerations(std::string_view value) -> Expected<std::vector<std::size_t>> {
    std::vector<std::size_t> result;
    if (value.empty())
        return result;
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find(',', start);
        if (end == std::string_view::npos)
            end = value.size();
        auto token = value.substr(start, end - start);
        std::size_t number = 0;
        auto fc = std::from_chars(token.data(), token.data() + token.size(), number);
        if (fc.ec != std::errc()) {
            return std::unexpected(Error{Error::Code::UnknownError, "Failed to parse generation list"});
        }
        result.push_back(number);
        start = end + 1;
    }
    return result;
}

auto encodeKeyValueLines(
    std::initializer_list<std::pair<std::string_view, std::string>> pairs) -> std::string {
    std::ostringstream oss;
    for (auto const& [key, value] : pairs) {
        oss << key << ':' << value << '\n';
    }
    return oss.str();
}

auto parseKeyValueLines(std::string const& text, std::string_view context)
    -> Expected<std::unordered_map<std::string, std::string>> {
    std::unordered_map<std::string, std::string> values;
    std::istringstream                            iss(text);
    std::string                                   line;
    while (std::getline(iss, line)) {
        if (line.empty())
            continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            return std::unexpected(
                Error{Error::Code::UnknownError, std::string(context) + " invalid line"});
        }
        std::string key   = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        values[std::move(key)] = std::move(value);
    }
    return values;
}

template <typename T>
auto parseIntegral(std::string const& value, T& out) -> bool {
    static_assert(std::is_integral_v<T>, "parseIntegral requires integral type");
    auto const* begin = value.data();
    auto const* end   = value.data() + value.size();
    auto        rc    = std::from_chars(begin, end, out);
    return rc.ec == std::errc() && rc.ptr == end;
}

template <typename T>
auto requireIntegralField(std::unordered_map<std::string, std::string> const& values,
                          std::string_view key,
                          std::string_view missingMessage,
                          std::string_view invalidMessage) -> Expected<T> {
    auto it = values.find(std::string(key));
    if (it == values.end()) {
        return std::unexpected(
            Error{Error::Code::UnknownError, std::string(missingMessage)});
    }
    T out{};
    if (!parseIntegral(it->second, out)) {
        return std::unexpected(
            Error{Error::Code::UnknownError, std::string(invalidMessage)});
    }
    return out;
}

auto requireGenerationList(std::unordered_map<std::string, std::string> const& values,
                           std::string_view key,
                           std::string_view missingMessage)
    -> Expected<std::vector<std::size_t>> {
    auto it = values.find(std::string(key));
    if (it == values.end()) {
        return std::unexpected(
            Error{Error::Code::UnknownError, std::string(missingMessage)});
    }
    return parseGenerations(it->second);
}

} // namespace

auto encodeEntryMeta(EntryMetadata const& meta) -> std::string {
    return encodeKeyValueLines({
        {"version", std::to_string(UndoUtils::EntryMetaVersion)},
        {"generation", std::to_string(meta.generation)},
        {"bytes", std::to_string(meta.bytes)},
        {"timestamp_ms", std::to_string(meta.timestampMs)},
    });
}

auto parseEntryMeta(std::string const& text) -> Expected<EntryMetadata> {
    auto valuesExpected = parseKeyValueLines(text, "Entry metadata");
    if (!valuesExpected)
        return std::unexpected(valuesExpected.error());
    auto values = std::move(valuesExpected.value());

    auto versionExpected = requireIntegralField<std::uint32_t>(values,
                                                               "version",
                                                               "Invalid entry meta version",
                                                               "Invalid entry meta version");
    if (!versionExpected)
        return std::unexpected(versionExpected.error());

    auto generationExpected = requireIntegralField<std::size_t>(values,
                                                                "generation",
                                                                "Invalid entry meta generation",
                                                                "Invalid entry meta generation");
    if (!generationExpected)
        return std::unexpected(generationExpected.error());

    auto bytesExpected = requireIntegralField<std::size_t>(values,
                                                           "bytes",
                                                           "Invalid entry meta bytes",
                                                           "Invalid entry meta bytes");
    if (!bytesExpected)
        return std::unexpected(bytesExpected.error());

    auto timestampExpected = requireIntegralField<std::uint64_t>(values,
                                                                 "timestamp_ms",
                                                                 "Invalid entry meta timestamp",
                                                                 "Invalid entry meta timestamp");
    if (!timestampExpected)
        return std::unexpected(timestampExpected.error());

    if (*versionExpected != UndoUtils::EntryMetaVersion) {
        return std::unexpected(Error{Error::Code::UnknownError, "Incomplete entry metadata"});
    }

    EntryMetadata meta;
    meta.generation  = *generationExpected;
    meta.bytes       = *bytesExpected;
    meta.timestampMs = *timestampExpected;

    return meta;
}

auto encodeStateMeta(StateMetadata const& meta) -> std::string {
    return encodeKeyValueLines({
        {"version", std::to_string(UndoUtils::StateMetaVersion)},
        {"live_generation", std::to_string(meta.liveGeneration)},
        {"undo", joinGenerations(meta.undoGenerations)},
        {"redo", joinGenerations(meta.redoGenerations)},
        {"manual_gc", meta.manualGc ? "1" : "0"},
        {"ram_cache_entries", std::to_string(meta.ramCacheEntries)},
    });
}

auto parseStateMeta(std::string const& text) -> Expected<StateMetadata> {
    auto valuesExpected = parseKeyValueLines(text, "State metadata");
    if (!valuesExpected)
        return std::unexpected(valuesExpected.error());
    auto values = std::move(valuesExpected.value());

    auto versionExpected = requireIntegralField<std::uint32_t>(values,
                                                               "version",
                                                               "Invalid state meta version",
                                                               "Invalid state meta version");
    if (!versionExpected)
        return std::unexpected(versionExpected.error());

    auto liveGenerationExpected = requireIntegralField<std::size_t>(values,
                                                                    "live_generation",
                                                                    "Invalid live generation",
                                                                    "Invalid live generation");
    if (!liveGenerationExpected)
        return std::unexpected(liveGenerationExpected.error());

    auto undoGenerationsExpected = requireGenerationList(values,
                                                         "undo",
                                                         "Incomplete state metadata");
    if (!undoGenerationsExpected)
        return std::unexpected(undoGenerationsExpected.error());

    auto redoGenerationsExpected = requireGenerationList(values,
                                                         "redo",
                                                         "Incomplete state metadata");
    if (!redoGenerationsExpected)
        return std::unexpected(redoGenerationsExpected.error());

    auto manualFlagExpected = requireIntegralField<int>(values,
                                                        "manual_gc",
                                                        "Incomplete state metadata",
                                                        "Invalid manual_gc flag");
    if (!manualFlagExpected)
        return std::unexpected(manualFlagExpected.error());

    auto ramEntriesExpected = requireIntegralField<std::size_t>(values,
                                                                "ram_cache_entries",
                                                                "Invalid ram_cache_entries",
                                                                "Invalid ram_cache_entries");
    if (!ramEntriesExpected)
        return std::unexpected(ramEntriesExpected.error());

    if (*versionExpected != UndoUtils::StateMetaVersion) {
        return std::unexpected(Error{Error::Code::UnknownError, "Incomplete state metadata"});
    }

    StateMetadata meta;
    meta.liveGeneration  = *liveGenerationExpected;
    meta.undoGenerations = std::move(undoGenerationsExpected.value());
    meta.redoGenerations = std::move(redoGenerationsExpected.value());
    meta.manualGc        = (*manualFlagExpected != 0);
    meta.ramCacheEntries = *ramEntriesExpected;

    return meta;
}

} // namespace SP::History::UndoMetadata

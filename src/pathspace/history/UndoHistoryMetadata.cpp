#include "history/UndoHistoryMetadata.hpp"

#include "history/UndoHistoryUtils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace SP::History::UndoMetadata {

namespace {

using SP::Error;
using SP::Expected;

template <typename T>
void appendScalar(std::vector<std::byte>& out, T value) {
    static_assert(std::is_integral_v<T>, "appendScalar requires integral type");
    auto const u = static_cast<std::make_unsigned_t<T>>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        auto byteValue = static_cast<unsigned int>((u >> (i * 8)) & 0xFFu);
        out.push_back(static_cast<std::byte>(byteValue));
    }
}

template <typename T>
auto readScalar(std::span<const std::byte>& data) -> Expected<T> {
    static_assert(std::is_integral_v<T>, "readScalar requires integral type");
    if (data.size() < sizeof(T)) {
        return std::unexpected(
            Error{Error::Code::UnknownError, "Metadata truncated"});
    }
    std::make_unsigned_t<T> value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<std::make_unsigned_t<T>>(
                     std::to_integer<unsigned int>(data[i]))
                 << (i * 8);
    }
    data = data.subspan(sizeof(T));
    return static_cast<T>(value);
}

auto encodeGenerationList(std::vector<std::size_t> const& generations) -> std::vector<std::byte> {
    std::vector<std::byte> buffer;
    buffer.reserve(sizeof(std::uint32_t) + generations.size() * sizeof(std::uint64_t));
    appendScalar(buffer, static_cast<std::uint32_t>(generations.size()));
    for (auto generation : generations) {
        appendScalar(buffer, static_cast<std::uint64_t>(generation));
    }
    return buffer;
}

auto decodeGenerationList(std::span<const std::byte>& data)
    -> Expected<std::vector<std::size_t>> {
    auto countExpected = readScalar<std::uint32_t>(data);
    if (!countExpected)
        return std::unexpected(countExpected.error());

    std::vector<std::size_t> generations;
    generations.reserve(*countExpected);
    for (std::uint32_t i = 0; i < *countExpected; ++i) {
        auto generationExpected = readScalar<std::uint64_t>(data);
        if (!generationExpected)
            return std::unexpected(generationExpected.error());
        generations.push_back(static_cast<std::size_t>(*generationExpected));
    }
    return generations;
}

} // namespace

auto encodeEntryMeta(EntryMetadata const& meta) -> std::vector<std::byte> {
    std::vector<std::byte> buffer;
    buffer.reserve(4 * sizeof(std::uint64_t));

    appendScalar(buffer, UndoUtils::EntryMetaVersion);
    appendScalar(buffer, static_cast<std::uint64_t>(meta.generation));
    appendScalar(buffer, static_cast<std::uint64_t>(meta.bytes));
    appendScalar(buffer, static_cast<std::uint64_t>(meta.timestampMs));

    return buffer;
}

auto parseEntryMeta(std::span<const std::byte> data) -> Expected<EntryMetadata> {
    auto versionExpected = readScalar<std::uint32_t>(data);
    if (!versionExpected)
        return std::unexpected(versionExpected.error());
    if (*versionExpected != UndoUtils::EntryMetaVersion) {
        return std::unexpected(
            Error{Error::Code::UnknownError, "Unsupported entry meta version"});
    }

    auto generationExpected = readScalar<std::uint64_t>(data);
    if (!generationExpected)
        return std::unexpected(generationExpected.error());

    auto bytesExpected = readScalar<std::uint64_t>(data);
    if (!bytesExpected)
        return std::unexpected(bytesExpected.error());

    auto timestampExpected = readScalar<std::uint64_t>(data);
    if (!timestampExpected)
        return std::unexpected(timestampExpected.error());

    EntryMetadata meta;
    meta.generation  = static_cast<std::size_t>(*generationExpected);
    meta.bytes       = static_cast<std::size_t>(*bytesExpected);
    meta.timestampMs = *timestampExpected;

    return meta;
}

auto encodeStateMeta(StateMetadata const& meta) -> std::vector<std::byte> {
    std::vector<std::byte> buffer;
    buffer.reserve(64);

    appendScalar(buffer, UndoUtils::StateMetaVersion);
    appendScalar(buffer, static_cast<std::uint64_t>(meta.liveGeneration));

    auto undoBytes = encodeGenerationList(meta.undoGenerations);
    buffer.insert(buffer.end(), undoBytes.begin(), undoBytes.end());

    auto redoBytes = encodeGenerationList(meta.redoGenerations);
    buffer.insert(buffer.end(), redoBytes.begin(), redoBytes.end());

    appendScalar(buffer, static_cast<std::uint32_t>(meta.manualGc ? 1 : 0));
    appendScalar(buffer, static_cast<std::uint64_t>(meta.ramCacheEntries));

    return buffer;
}

auto parseStateMeta(std::span<const std::byte> data) -> Expected<StateMetadata> {
    auto versionExpected = readScalar<std::uint32_t>(data);
    if (!versionExpected)
        return std::unexpected(versionExpected.error());
    if (*versionExpected != UndoUtils::StateMetaVersion) {
        return std::unexpected(
            Error{Error::Code::UnknownError, "Unsupported state meta version"});
    }

    auto liveGenerationExpected = readScalar<std::uint64_t>(data);
    if (!liveGenerationExpected)
        return std::unexpected(liveGenerationExpected.error());

    auto undoGenerationsExpected = decodeGenerationList(data);
    if (!undoGenerationsExpected)
        return std::unexpected(undoGenerationsExpected.error());

    auto redoGenerationsExpected = decodeGenerationList(data);
    if (!redoGenerationsExpected)
        return std::unexpected(redoGenerationsExpected.error());

    auto manualExpected = readScalar<std::uint32_t>(data);
    if (!manualExpected)
        return std::unexpected(manualExpected.error());

    auto ramExpected = readScalar<std::uint64_t>(data);
    if (!ramExpected)
        return std::unexpected(ramExpected.error());

    StateMetadata meta;
    meta.liveGeneration   = static_cast<std::size_t>(*liveGenerationExpected);
    meta.undoGenerations  = std::move(undoGenerationsExpected.value());
    meta.redoGenerations  = std::move(redoGenerationsExpected.value());
    meta.manualGc         = (*manualExpected != 0);
    meta.ramCacheEntries  = static_cast<std::size_t>(*ramExpected);

    return meta;
}

} // namespace SP::History::UndoMetadata

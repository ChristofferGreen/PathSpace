#include "history/UndoSavefileCodec.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace SP::History::UndoSavefile {

namespace {

using SP::Error;
using SP::Expected;

template <typename T>
void appendScalar(std::vector<std::byte>& buffer, T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                  "appendScalar requires integral or enum type");
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT uvalue = static_cast<UnsignedT>(value);
    for (std::size_t idx = 0; idx < sizeof(T); ++idx) {
        auto byteValue = static_cast<unsigned int>((uvalue >> (idx * 8)) & 0xFFu);
        buffer.push_back(static_cast<std::byte>(byteValue));
    }
}

void appendBytes(std::vector<std::byte>& buffer, std::span<const std::byte> data) {
    buffer.insert(buffer.end(), data.begin(), data.end());
}

void appendString(std::vector<std::byte>& buffer, std::string const& value) {
    appendScalar(buffer, static_cast<std::uint32_t>(value.size()));
    auto const byteView = std::as_bytes(std::span(value.data(), value.size()));
    appendBytes(buffer, byteView);
}

template <typename T>
auto readScalar(std::span<const std::byte>& buffer) -> Expected<T> {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                  "readScalar requires integral or enum type");
    if (buffer.size() < sizeof(T)) {
        return std::unexpected(Error{Error::Code::UnknownError, "Savefile truncated"});
    }
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT value = 0;
    for (std::size_t idx = 0; idx < sizeof(T); ++idx) {
        value |= static_cast<UnsignedT>(std::to_integer<unsigned int>(buffer[idx])) << (idx * 8);
    }
    buffer = buffer.subspan(sizeof(T));
    return static_cast<T>(value);
}

auto readBytes(std::span<const std::byte>& buffer, std::size_t size)
    -> Expected<std::vector<std::byte>> {
    if (buffer.size() < size) {
        return std::unexpected(Error{Error::Code::UnknownError, "Savefile truncated"});
    }
    std::vector<std::byte> out(size);
    std::memcpy(out.data(), buffer.data(), size);
    buffer = buffer.subspan(size);
    return out;
}

auto readString(std::span<const std::byte>& buffer) -> Expected<std::string> {
    auto sizeExpected = readScalar<std::uint32_t>(buffer);
    if (!sizeExpected)
        return std::unexpected(sizeExpected.error());
    auto size = static_cast<std::size_t>(*sizeExpected);
    if (buffer.size() < size) {
        return std::unexpected(Error{Error::Code::UnknownError, "Savefile truncated"});
    }
    std::string value(size, '\0');
    std::memcpy(value.data(), buffer.data(), size);
    buffer = buffer.subspan(size);
    return value;
}

void encodeEntry(std::vector<std::byte>& buffer, EntryBlock const& entry) {
    auto metaBytes = UndoMetadata::encodeEntryMeta(entry.metadata);
    appendScalar(buffer, static_cast<std::uint32_t>(metaBytes.size()));
    appendBytes(buffer, std::span(metaBytes.data(), metaBytes.size()));
    appendScalar(buffer, entry.timestampMs);
    appendScalar(buffer, static_cast<std::uint64_t>(entry.snapshot.size()));
    appendBytes(buffer, std::span(entry.snapshot.data(), entry.snapshot.size()));
}

auto decodeEntry(std::span<const std::byte>& buffer) -> Expected<EntryBlock> {
    EntryBlock entry;

    auto metaSizeExpected = readScalar<std::uint32_t>(buffer);
    if (!metaSizeExpected)
        return std::unexpected(metaSizeExpected.error());
    auto metaBytesExpected = readBytes(buffer, *metaSizeExpected);
    if (!metaBytesExpected)
        return std::unexpected(metaBytesExpected.error());
    auto metaParsed = UndoMetadata::parseEntryMeta(
        std::span<const std::byte>(metaBytesExpected->data(), metaBytesExpected->size()));
    if (!metaParsed)
        return std::unexpected(metaParsed.error());
    entry.metadata = std::move(metaParsed.value());

    auto timestampExpected = readScalar<std::uint64_t>(buffer);
    if (!timestampExpected)
        return std::unexpected(timestampExpected.error());
    entry.timestampMs = *timestampExpected;

    auto snapshotSizeExpected = readScalar<std::uint64_t>(buffer);
    if (!snapshotSizeExpected)
        return std::unexpected(snapshotSizeExpected.error());
    auto snapshotExpected = readBytes(buffer, static_cast<std::size_t>(*snapshotSizeExpected));
    if (!snapshotExpected)
        return std::unexpected(snapshotExpected.error());
    entry.snapshot = std::move(snapshotExpected.value());

    return entry;
}

template <typename Fn>
auto decodeEntries(std::span<const std::byte>& buffer, Fn&& fn)
    -> Expected<std::vector<EntryBlock>> {
    auto countExpected = readScalar<std::uint32_t>(buffer);
    if (!countExpected)
        return std::unexpected(countExpected.error());
    std::vector<EntryBlock> entries;
    entries.reserve(*countExpected);
    for (std::uint32_t idx = 0; idx < *countExpected; ++idx) {
        auto entryExpected = decodeEntry(buffer);
        if (!entryExpected)
            return std::unexpected(entryExpected.error());
        auto entry = std::move(entryExpected.value());
        fn(entry);
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

auto encode(Document const& document) -> std::vector<std::byte> {
    std::vector<std::byte> buffer;
    buffer.reserve(4096);

    appendScalar(buffer, SavefileMagic);
    appendScalar(buffer, SavefileVersion);

    appendString(buffer, document.rootPath);

    appendScalar(buffer, static_cast<std::uint64_t>(document.options.maxEntries));
    appendScalar(buffer, static_cast<std::uint64_t>(document.options.maxBytesRetained));
    appendScalar(buffer, static_cast<std::uint64_t>(document.options.ramCacheEntries));
    appendScalar(buffer, static_cast<std::uint64_t>(document.options.maxDiskBytes));
    appendScalar(buffer, document.options.keepLatestForMs);
    appendScalar(buffer, static_cast<std::uint8_t>(document.options.manualGarbageCollect ? 1 : 0));

    auto stateBytes =
        UndoMetadata::encodeStateMeta(document.stateMetadata);
    appendScalar(buffer, static_cast<std::uint32_t>(stateBytes.size()));
    appendBytes(buffer, std::span(stateBytes.data(), stateBytes.size()));

    encodeEntry(buffer, document.liveEntry);

    appendScalar(buffer, static_cast<std::uint32_t>(document.undoEntries.size()));
    for (auto const& entry : document.undoEntries) {
        encodeEntry(buffer, entry);
    }

    appendScalar(buffer, static_cast<std::uint32_t>(document.redoEntries.size()));
    for (auto const& entry : document.redoEntries) {
        encodeEntry(buffer, entry);
    }

    return buffer;
}

auto decode(std::span<const std::byte> data) -> Expected<Document> {
    auto buffer = data;
    auto magicExpected = readScalar<std::uint32_t>(buffer);
    if (!magicExpected)
        return std::unexpected(magicExpected.error());
    if (*magicExpected != SavefileMagic) {
        return std::unexpected(Error{Error::Code::UnknownError, "Unrecognized savefile magic"});
    }

    auto versionExpected = readScalar<std::uint32_t>(buffer);
    if (!versionExpected)
        return std::unexpected(versionExpected.error());
    if (*versionExpected != SavefileVersion) {
        return std::unexpected(Error{Error::Code::UnknownError, "Unsupported savefile version"});
    }

    auto rootPathExpected = readString(buffer);
    if (!rootPathExpected)
        return std::unexpected(rootPathExpected.error());

    Document document;
    document.rootPath = std::move(rootPathExpected.value());

    auto maxEntriesExpected      = readScalar<std::uint64_t>(buffer);
    auto maxBytesExpected        = readScalar<std::uint64_t>(buffer);
    auto ramCacheExpected        = readScalar<std::uint64_t>(buffer);
    auto maxDiskExpected         = readScalar<std::uint64_t>(buffer);
    auto keepLatestForExpected   = readScalar<std::uint64_t>(buffer);
    auto manualGcExpected        = readScalar<std::uint8_t>(buffer);

    if (!maxEntriesExpected || !maxBytesExpected || !ramCacheExpected || !maxDiskExpected
        || !keepLatestForExpected || !manualGcExpected) {
        return std::unexpected(Error{Error::Code::UnknownError, "Savefile truncated"});
    }

    document.options.maxEntries        = static_cast<std::size_t>(*maxEntriesExpected);
    document.options.maxBytesRetained  = static_cast<std::size_t>(*maxBytesExpected);
    document.options.ramCacheEntries   = static_cast<std::size_t>(*ramCacheExpected);
    document.options.maxDiskBytes      = static_cast<std::size_t>(*maxDiskExpected);
    document.options.keepLatestForMs   = *keepLatestForExpected;
    document.options.manualGarbageCollect = (*manualGcExpected != 0);

    auto stateSizeExpected = readScalar<std::uint32_t>(buffer);
    if (!stateSizeExpected)
        return std::unexpected(stateSizeExpected.error());
    auto stateBytesExpected = readBytes(buffer, *stateSizeExpected);
    if (!stateBytesExpected)
        return std::unexpected(stateBytesExpected.error());
    auto stateParsed = UndoMetadata::parseStateMeta(
        std::span<const std::byte>(stateBytesExpected->data(), stateBytesExpected->size()));
    if (!stateParsed)
        return std::unexpected(stateParsed.error());
    document.stateMetadata = std::move(stateParsed.value());

    auto liveEntryExpected = decodeEntry(buffer);
    if (!liveEntryExpected)
        return std::unexpected(liveEntryExpected.error());
    document.liveEntry = std::move(liveEntryExpected.value());

    auto decodeWithoutFn = [](EntryBlock const&) {};

    auto undoEntriesExpected = decodeEntries(buffer, decodeWithoutFn);
    if (!undoEntriesExpected)
        return std::unexpected(undoEntriesExpected.error());
    document.undoEntries = std::move(undoEntriesExpected.value());

    auto redoEntriesExpected = decodeEntries(buffer, decodeWithoutFn);
    if (!redoEntriesExpected)
        return std::unexpected(redoEntriesExpected.error());
    document.redoEntries = std::move(redoEntriesExpected.value());

    return document;
}

} // namespace SP::History::UndoSavefile

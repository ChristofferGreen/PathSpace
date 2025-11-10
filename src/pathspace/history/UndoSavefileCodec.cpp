#include "history/UndoSavefileCodec.hpp"

#include "history/UndoJournalEntry.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace {

using SP::Error;
using SP::Expected;
using SP::History::UndoJournal::JournalEntry;

template <typename T>
void appendScalar(std::vector<std::byte>& buffer, T value) {
    static_assert(std::is_trivially_copyable_v<T>, "appendScalar requires trivially copyable type");
    std::uint8_t local[sizeof(T)];
    std::memcpy(local, &value, sizeof(T));
    auto const base = reinterpret_cast<const std::byte*>(local);
    buffer.insert(buffer.end(), base, base + sizeof(T));
}

void appendBytes(std::vector<std::byte>& buffer, std::span<const std::byte> bytes) {
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

void appendString(std::vector<std::byte>& buffer, std::string const& value) {
    appendScalar<std::uint32_t>(buffer, static_cast<std::uint32_t>(value.size()));
    auto const span = std::span(reinterpret_cast<const std::byte*>(value.data()), value.size());
    appendBytes(buffer, span);
}

template <typename T>
auto readScalar(std::span<const std::byte>& buffer) -> Expected<T> {
    if (buffer.size() < sizeof(T)) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Savefile truncated"});
    }
    T value{};
    std::memcpy(&value, buffer.data(), sizeof(T));
    buffer = buffer.subspan(sizeof(T));
    return value;
}

auto readBytes(std::span<const std::byte>& buffer, std::size_t size)
    -> Expected<std::span<const std::byte>> {
    if (buffer.size() < size) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Savefile truncated"});
    }
    auto view = buffer.subspan(0, size);
    buffer    = buffer.subspan(size);
    return view;
}

auto readString(std::span<const std::byte>& buffer) -> Expected<std::string> {
    auto sizeExpected = readScalar<std::uint32_t>(buffer);
    if (!sizeExpected)
        return std::unexpected(sizeExpected.error());
    auto viewExpected = readBytes(buffer, static_cast<std::size_t>(*sizeExpected));
    if (!viewExpected)
        return std::unexpected(viewExpected.error());
    std::string value(viewExpected->size(), '\0');
    std::memcpy(value.data(), viewExpected->data(), viewExpected->size());
    return value;
}

auto makeError(std::string message) -> Error {
    return Error{Error::Code::UnknownError, std::move(message)};
}

} // namespace

namespace SP::History::UndoSavefile {

auto encode(Document const& document) -> Expected<std::vector<std::byte>> {
    std::vector<std::byte> buffer;
    buffer.reserve(4096);

    appendScalar<std::uint32_t>(buffer, SavefileMagic);
    appendScalar<std::uint32_t>(buffer, SavefileVersion);

    if (document.rootPath.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(makeError("Root path exceeds encodable length"));
    }
    appendString(buffer, document.rootPath);
    appendScalar<std::uint64_t>(buffer, static_cast<std::uint64_t>(document.options.maxEntries));
    appendScalar<std::uint64_t>(buffer, static_cast<std::uint64_t>(document.options.maxBytesRetained));
    appendScalar<std::uint64_t>(buffer, static_cast<std::uint64_t>(document.options.maxDiskBytes));
    appendScalar<std::uint64_t>(buffer, document.options.keepLatestForMs);
    appendScalar<std::uint8_t>(buffer, static_cast<std::uint8_t>(document.options.manualGarbageCollect ? 1 : 0));

    appendScalar<std::uint64_t>(buffer, document.nextSequence);
    appendScalar<std::uint64_t>(buffer, static_cast<std::uint64_t>(document.undoCount));

    if (document.entries.size() > std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(makeError("Undo savefile contains too many entries"));
    }
    appendScalar<std::uint64_t>(buffer, static_cast<std::uint64_t>(document.entries.size()));

    for (auto const& entry : document.entries) {
        if (entry.path.size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(makeError("Journal entry path exceeds encodable length"));
        }
        auto encoded = UndoJournal::serializeEntry(entry);
        if (!encoded)
            return std::unexpected(encoded.error());
        if (encoded->size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(makeError("Serialized journal entry exceeds encodable length"));
        }
        appendScalar<std::uint32_t>(buffer, static_cast<std::uint32_t>(encoded->size()));
        appendBytes(buffer, std::span(encoded->data(), encoded->size()));
    }

    return buffer;
}

auto decode(std::span<const std::byte> data) -> Expected<Document> {
    auto buffer = data;
    auto magic  = readScalar<std::uint32_t>(buffer);
    if (!magic)
        return std::unexpected(magic.error());
    if (*magic != SavefileMagic) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Unrecognized savefile magic"});
    }

    auto version = readScalar<std::uint32_t>(buffer);
    if (!version)
        return std::unexpected(version.error());
    if (*version != SavefileVersion) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Unsupported savefile version"});
    }

    Document document;

    auto rootPath = readString(buffer);
    if (!rootPath)
        return std::unexpected(rootPath.error());
    document.rootPath = std::move(rootPath.value());

    auto maxEntries      = readScalar<std::uint64_t>(buffer);
    auto maxBytes        = readScalar<std::uint64_t>(buffer);
    auto maxDisk         = readScalar<std::uint64_t>(buffer);
    auto keepLatest      = readScalar<std::uint64_t>(buffer);
    auto manualGcFlag    = readScalar<std::uint8_t>(buffer);
    if (!maxEntries || !maxBytes || !maxDisk || !keepLatest || !manualGcFlag)
        return std::unexpected(Error{Error::Code::MalformedInput, "Savefile truncated"});

    document.options.maxEntries           = static_cast<std::size_t>(*maxEntries);
    document.options.maxBytesRetained     = static_cast<std::size_t>(*maxBytes);
    document.options.maxDiskBytes         = static_cast<std::size_t>(*maxDisk);
    document.options.keepLatestForMs      = *keepLatest;
    document.options.manualGarbageCollect = (*manualGcFlag != 0);

    auto nextSequence = readScalar<std::uint64_t>(buffer);
    if (!nextSequence)
        return std::unexpected(nextSequence.error());
    document.nextSequence = *nextSequence;

    auto undoCount = readScalar<std::uint64_t>(buffer);
    if (!undoCount)
        return std::unexpected(undoCount.error());
    document.undoCount = static_cast<std::size_t>(*undoCount);

    auto entryCount = readScalar<std::uint64_t>(buffer);
    if (!entryCount)
        return std::unexpected(entryCount.error());

    if (*entryCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(makeError("Undo savefile entry count exceeds platform limits"));
    }

    document.entries.reserve(static_cast<std::size_t>(*entryCount));
    for (std::uint64_t idx = 0; idx < *entryCount; ++idx) {
        auto encodedSize = readScalar<std::uint32_t>(buffer);
        if (!encodedSize)
            return std::unexpected(encodedSize.error());
        auto entryBytes = readBytes(buffer, static_cast<std::size_t>(*encodedSize));
        if (!entryBytes)
            return std::unexpected(entryBytes.error());
        auto entryExpected = UndoJournal::deserializeEntry(*entryBytes);
        if (!entryExpected)
            return std::unexpected(entryExpected.error());
        document.entries.push_back(std::move(entryExpected.value()));
    }

    if (document.undoCount > document.entries.size()) {
        return std::unexpected(makeError("Undo count exceeds total entries"));
    }

    return document;
}

} // namespace SP::History::UndoSavefile

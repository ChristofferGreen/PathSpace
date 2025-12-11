#include "history/UndoJournalEntry.hpp"

#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>

namespace {

using SP::Error;
using SP::Expected;
using SP::History::UndoJournal::JournalEntry;
using SP::History::UndoJournal::SerializedPayload;

constexpr std::uint8_t kBarrierFlag = 0x01;

template <typename T>
inline void appendScalar(std::vector<std::byte>& buffer, T value) {
    static_assert(std::is_trivially_copyable_v<T>, "appendScalar requires trivially copyable type");
    std::uint8_t local[sizeof(T)];
    std::memcpy(local, &value, sizeof(T));
    auto const base = reinterpret_cast<const std::byte*>(local);
    buffer.insert(buffer.end(), base, base + sizeof(T));
}

template <typename T>
inline auto readScalar(std::span<const std::byte>& bytes) -> std::optional<T> {
    if (bytes.size() < sizeof(T))
        return std::nullopt;
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    bytes = bytes.subspan(sizeof(T));
    return value;
}

inline auto appendPayload(std::vector<std::byte>& buffer, SerializedPayload const& payload)
    -> void {
    buffer.push_back(payload.present ? std::byte{1} : std::byte{0});
    auto const size = payload.bytes.size();
    appendScalar<std::uint32_t>(buffer, static_cast<std::uint32_t>(size));
    if (size > 0) {
        buffer.insert(buffer.end(), payload.bytes.begin(), payload.bytes.end());
    }
}

inline auto readPayload(std::span<const std::byte>& bytes) -> Expected<SerializedPayload> {
    auto presentOpt = readScalar<std::uint8_t>(bytes);
    if (!presentOpt.has_value()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (payload flag)"});
    }
    auto lengthOpt = readScalar<std::uint32_t>(bytes);
    if (!lengthOpt.has_value()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (payload length)"});
    }
    auto length = static_cast<std::size_t>(*lengthOpt);
    if (bytes.size() < length) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (payload bytes)"});
    }
    SerializedPayload payload;
    payload.present = (*presentOpt != 0);
    if (length > 0) {
        payload.bytes.resize(length);
        std::memcpy(payload.bytes.data(), bytes.data(), length);
        bytes = bytes.subspan(length);
    }
    if (!payload.present && !payload.bytes.empty()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal payload flagged absent but bytes provided"});
    }
    if (!payload.present && length != 0) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal payload absent but non-zero length"});
    }
    return payload;
}

inline auto makeError(std::string message) -> Error {
    return Error{Error::Code::UnknownError, std::move(message)};
}

} // namespace

namespace SP::History::UndoJournal {

auto serializeEntry(JournalEntry const& entry) -> Expected<std::vector<std::byte>> {
    if (entry.path.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(makeError("Journal entry path exceeds encodable length"));
    if (entry.tag.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(makeError("Journal entry tag exceeds encodable length"));
    if (entry.value.bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(makeError("Journal entry value payload exceeds encodable length"));
    if (entry.inverseValue.bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(makeError("Journal entry inverse payload exceeds encodable length"));

    std::vector<std::byte> buffer;
    buffer.reserve(64 + entry.path.size() + entry.tag.size() + entry.value.bytes.size()
                   + entry.inverseValue.bytes.size());

    appendScalar<std::uint32_t>(buffer, kJournalMagic);
    appendScalar<std::uint16_t>(buffer, kJournalVersion);

    appendScalar<std::uint8_t>(buffer, static_cast<std::uint8_t>(entry.operation));
    std::uint8_t flags = entry.barrier ? kBarrierFlag : 0u;
    appendScalar<std::uint8_t>(buffer, flags);
    appendScalar<std::uint16_t>(buffer, 0u); // reserved for future use

    appendScalar<std::uint64_t>(buffer, entry.timestampMs);
    appendScalar<std::uint64_t>(buffer, entry.monotonicNs);
    appendScalar<std::uint64_t>(buffer, entry.sequence);

    appendScalar<std::uint32_t>(buffer, static_cast<std::uint32_t>(entry.path.size()));
    if (!entry.path.empty()) {
        auto const* bytes = reinterpret_cast<std::byte const*>(entry.path.data());
        buffer.insert(buffer.end(), bytes, bytes + entry.path.size());
    }

    appendPayload(buffer, entry.value);
    appendPayload(buffer, entry.inverseValue);

    appendScalar<std::uint32_t>(buffer, static_cast<std::uint32_t>(entry.tag.size()));
    if (!entry.tag.empty()) {
        auto const* bytes = reinterpret_cast<std::byte const*>(entry.tag.data());
        buffer.insert(buffer.end(), bytes, bytes + entry.tag.size());
    }

    return buffer;
}

auto deserializeEntry(std::span<const std::byte> bytes) -> Expected<JournalEntry> {
    auto data = bytes;
    auto magic = readScalar<std::uint32_t>(data);
    if (!magic.has_value() || *magic != kJournalMagic) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry missing magic header"});
    }

    auto version = readScalar<std::uint16_t>(data);
    if (!version.has_value()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry missing version"});
    }
    if (*version < 1 || *version > kJournalVersion) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Unsupported journal entry version"});
    }

    auto opByte = readScalar<std::uint8_t>(data);
    auto flagByte = readScalar<std::uint8_t>(data);
    auto reserved = readScalar<std::uint16_t>(data);
    if (!opByte.has_value() || !flagByte.has_value() || !reserved.has_value()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (operation fields)"});
    }

    JournalEntry entry;
    if (*opByte > static_cast<std::uint8_t>(OperationKind::Take)) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Unknown journal operation kind"});
    }
    entry.operation = static_cast<OperationKind>(*opByte);
    entry.barrier   = ((*flagByte & kBarrierFlag) != 0);

    auto timestamp = readScalar<std::uint64_t>(data);
    auto monotonic = readScalar<std::uint64_t>(data);
    auto sequence  = readScalar<std::uint64_t>(data);
    if (!timestamp.has_value() || !monotonic.has_value() || !sequence.has_value()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (metadata)"});
    }
    entry.timestampMs = *timestamp;
    entry.monotonicNs = *monotonic;
    entry.sequence    = *sequence;

    auto pathLength = readScalar<std::uint32_t>(data);
    if (!pathLength.has_value()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (path length)"});
    }
    auto remaining = data.size();
    if (remaining < *pathLength) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (path bytes)"});
    }
    entry.path.assign(reinterpret_cast<char const*>(data.data()), *pathLength);
    data = data.subspan(*pathLength);

    auto valuePayload   = readPayload(data);
    if (!valuePayload)
        return std::unexpected(valuePayload.error());
    entry.value = std::move(valuePayload.value());

    auto inversePayload = readPayload(data);
    if (!inversePayload)
        return std::unexpected(inversePayload.error());
    entry.inverseValue = std::move(inversePayload.value());

    if (*version >= 2) {
        auto tagLength = readScalar<std::uint32_t>(data);
        if (!tagLength.has_value()) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (tag length)"});
        }
        auto length = static_cast<std::size_t>(*tagLength);
        if (data.size() < length) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Journal entry truncated (tag bytes)"});
        }
        if (length > 0) {
            entry.tag.assign(reinterpret_cast<char const*>(data.data()), length);
            data = data.subspan(length);
        } else {
            entry.tag.clear();
        }
    }

    return entry;
}

auto encodeNodeDataPayload(NodeData const& node) -> Expected<SerializedPayload> {
    auto bytesOpt = node.serializeSnapshot();
    if (!bytesOpt.has_value()) {
        return std::unexpected(makeError("Unable to serialize NodeData payload for journal (unsupported content)"));
    }
    SerializedPayload payload;
    payload.present = true;
    payload.bytes   = std::move(bytesOpt.value());
    return payload;
}

auto decodeNodeDataPayload(SerializedPayload const& payload) -> Expected<NodeData> {
    if (!payload.present) {
        return std::unexpected(makeError("Journal payload missing NodeData content"));
    }
    auto span = std::span<const std::byte>{payload.bytes.data(), payload.bytes.size()};
    auto nodeOpt = NodeData::deserializeSnapshot(span);
    if (!nodeOpt.has_value()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Unable to decode NodeData from journal payload"});
    }
    return nodeOpt.value();
}

} // namespace SP::History::UndoJournal

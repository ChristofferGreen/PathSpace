#pragma once

#include "core/Error.hpp"
#include "core/NodeData.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SP::History::UndoJournal {

inline constexpr std::uint32_t JournalMagic   = 0x50534A4C; // 'PSJL'
inline constexpr std::uint16_t JournalVersion = 2;

enum class OperationKind : std::uint8_t {
    Insert = 0,
    Take   = 1,
};

struct SerializedPayload {
    bool                     present = false;
    std::vector<std::byte>   bytes;
};

struct JournalEntry {
    OperationKind operation      = OperationKind::Insert;
    std::string   path;
    std::string   tag;
    SerializedPayload value;
    SerializedPayload inverseValue;
    std::uint64_t timestampMs    = 0;
    std::uint64_t monotonicNs    = 0;
    std::uint64_t sequence       = 0;
    bool          barrier        = false;
};

[[nodiscard]] auto serializeEntry(JournalEntry const& entry) -> Expected<std::vector<std::byte>>;
[[nodiscard]] auto deserializeEntry(std::span<const std::byte> bytes) -> Expected<JournalEntry>;

[[nodiscard]] auto encodeNodeDataPayload(NodeData const& node) -> Expected<SerializedPayload>;
[[nodiscard]] auto decodeNodeDataPayload(SerializedPayload const& payload) -> Expected<NodeData>;

} // namespace SP::History::UndoJournal

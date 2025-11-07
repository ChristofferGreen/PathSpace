#include "history/UndoSnapshotCodec.hpp"

#include "history/UndoHistoryUtils.hpp"

#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>

namespace SP::History::UndoSnapshotCodec {

using SP::Error;
using SP::Expected;
namespace UndoUtilsAlias = SP::History::UndoUtils;

namespace {

template <typename T>
void appendScalar(std::vector<std::byte>& buffer, T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                  "appendScalar requires integral type");
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT uvalue = static_cast<UnsignedT>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        auto byteValue = static_cast<unsigned int>((uvalue >> (i * 8)) & 0xFFu);
        buffer.push_back(static_cast<std::byte>(byteValue));
    }
}

template <typename T>
auto readScalar(std::span<const std::byte>& buffer) -> std::optional<T> {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                  "readScalar requires integral type");
    if (buffer.size() < sizeof(T))
        return std::nullopt;
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<UnsignedT>(std::to_integer<unsigned int>(buffer[i])) << (i * 8);
    }
    buffer = buffer.subspan(sizeof(T));
    return static_cast<T>(value);
}

struct SnapshotEntryData {
    std::vector<std::string> components;
    std::vector<std::byte>   payload;
};

void collectSnapshotEntries(std::shared_ptr<const CowSubtreePrototype::Node> const& node,
                            std::vector<std::string>&                               components,
                            std::vector<SnapshotEntryData>&                         out) {
    if (!node)
        return;
    if (node->payload.bytes && !node->payload.bytes->empty()) {
        SnapshotEntryData entry;
        entry.components = components;
        entry.payload.assign(node->payload.bytes->begin(), node->payload.bytes->end());
        out.push_back(std::move(entry));
    }
    for (auto const& [childName, childNode] : node->children) {
        if (!childNode)
            continue;
        components.push_back(childName);
        collectSnapshotEntries(childNode, components, out);
        components.pop_back();
    }
}

} // namespace

auto encodeSnapshot(CowSubtreePrototype::Snapshot const& snapshot)
    -> Expected<std::vector<std::byte>> {
    std::vector<std::byte> buffer;
    appendScalar(buffer, UndoUtilsAlias::SnapshotMagic);
    appendScalar(buffer, UndoUtilsAlias::SnapshotVersion);
    appendScalar(buffer, static_cast<std::uint64_t>(snapshot.generation));

    std::vector<SnapshotEntryData> entries;
    if (snapshot.root) {
        std::vector<std::string> path;
        collectSnapshotEntries(snapshot.root, path, entries);
    }
    appendScalar(buffer, static_cast<std::uint32_t>(entries.size()));

    for (auto const& entry : entries) {
        appendScalar(buffer, static_cast<std::uint32_t>(entry.components.size()));
        for (auto const& component : entry.components) {
            appendScalar(buffer, static_cast<std::uint32_t>(component.size()));
            auto offset = buffer.size();
            buffer.resize(offset + component.size());
            std::memcpy(buffer.data() + static_cast<std::ptrdiff_t>(offset),
                        component.data(),
                        component.size());
        }
        appendScalar(buffer, static_cast<std::uint32_t>(entry.payload.size()));
        buffer.insert(buffer.end(), entry.payload.begin(), entry.payload.end());
    }

    return buffer;
}

auto decodeSnapshot(CowSubtreePrototype& prototype, std::span<const std::byte> data)
    -> Expected<CowSubtreePrototype::Snapshot> {
    auto buffer = data;

    auto magicOpt = readScalar<std::uint32_t>(buffer);
    if (!magicOpt || *magicOpt != UndoUtilsAlias::SnapshotMagic) {
        return std::unexpected(Error{Error::Code::UnknownError, "Invalid snapshot magic"});
    }
    auto versionOpt = readScalar<std::uint32_t>(buffer);
    if (!versionOpt || *versionOpt != UndoUtilsAlias::SnapshotVersion) {
        return std::unexpected(Error{Error::Code::UnknownError, "Unsupported snapshot version"});
    }
    auto generationOpt = readScalar<std::uint64_t>(buffer);
    if (!generationOpt) {
        return std::unexpected(Error{Error::Code::UnknownError, "Snapshot missing generation"});
    }
    auto countOpt = readScalar<std::uint32_t>(buffer);
    if (!countOpt) {
        return std::unexpected(Error{Error::Code::UnknownError, "Snapshot missing entry count"});
    }

    std::vector<CowSubtreePrototype::Mutation> mutations;
    mutations.reserve(*countOpt);

    for (std::uint32_t i = 0; i < *countOpt; ++i) {
        auto componentCountOpt = readScalar<std::uint32_t>(buffer);
        if (!componentCountOpt) {
            return std::unexpected(
                Error{Error::Code::UnknownError, "Snapshot malformed component count"});
        }
        std::vector<std::string> components;
        components.reserve(*componentCountOpt);
        for (std::uint32_t c = 0; c < *componentCountOpt; ++c) {
            auto lenOpt = readScalar<std::uint32_t>(buffer);
            if (!lenOpt || buffer.size() < *lenOpt) {
                return std::unexpected(
                    Error{Error::Code::UnknownError, "Snapshot malformed component"});
            }
            std::string comp(static_cast<std::size_t>(*lenOpt), '\0');
            std::memcpy(comp.data(), buffer.data(), *lenOpt);
            components.push_back(std::move(comp));
            buffer = buffer.subspan(*lenOpt);
        }

        auto payloadLenOpt = readScalar<std::uint32_t>(buffer);
        if (!payloadLenOpt || buffer.size() < *payloadLenOpt) {
            return std::unexpected(
                Error{Error::Code::UnknownError, "Snapshot malformed payload length"});
        }
        std::vector<std::byte> payload;
        payload.insert(payload.end(), buffer.begin(), buffer.begin() + *payloadLenOpt);
        buffer = buffer.subspan(*payloadLenOpt);

        CowSubtreePrototype::Mutation mutation;
        mutation.components = std::move(components);
        mutation.payload    = CowSubtreePrototype::Payload(std::move(payload));
        mutations.push_back(std::move(mutation));
    }

    auto snapshot = prototype.emptySnapshot();
    snapshot.generation = *generationOpt;
    for (auto& mutation : mutations) {
        snapshot = prototype.apply(snapshot, mutation);
    }
    return snapshot;
}

auto snapshotFileStem(std::size_t generation) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << generation;
    return oss.str();
}

auto loadSnapshotFromFile(CowSubtreePrototype& prototype,
                          std::filesystem::path const& path)
    -> Expected<CowSubtreePrototype::Snapshot> {
    auto data = UndoUtilsAlias::readBinaryFile(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    return decodeSnapshot(prototype, std::span<const std::byte>(data->data(), data->size()));
}

} // namespace SP::History::UndoSnapshotCodec

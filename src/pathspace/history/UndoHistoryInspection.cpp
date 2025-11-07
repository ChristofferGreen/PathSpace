#include "history/UndoHistoryInspection.hpp"

#include "core/NodeData.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "type/DataCategory.hpp"
#include "type/serialization.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

namespace {

using SP::History::CowSubtreePrototype;
using SP::History::HistoryLastOperation;
using SP::History::HistoryStats;
using SP::History::Inspection::DecodedValue;
using SP::History::Inspection::ModifiedValue;
using SP::History::Inspection::SnapshotDiff;
using SP::History::Inspection::SnapshotSummary;
using SP::History::UndoableSpace;

auto demangle(std::string const& name) -> std::string {
#if defined(__GNUG__)
    int   status  = 0;
    char* dem     = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (status == 0 && dem) {
        std::string out{dem};
        std::free(dem);
        return out;
    }
    if (dem) {
        std::free(dem);
    }
#endif
    return name;
}

auto categoryToString(SP::DataCategory category) -> std::string {
    switch (category) {
    case SP::DataCategory::None: return "none";
    case SP::DataCategory::SerializedData: return "serialized";
    case SP::DataCategory::Execution: return "execution";
    case SP::DataCategory::FunctionPointer: return "function";
    case SP::DataCategory::Fundamental: return "fundamental";
    case SP::DataCategory::SerializationLibraryCompatible: return "serializable";
    case SP::DataCategory::UniquePtr: return "unique_ptr";
    }
    return "unknown";
}

auto formatHexPreview(std::span<const std::uint8_t> bytes) -> std::string {
    constexpr std::size_t kPreviewBytes = 32;
    std::ostringstream    oss;
    oss << "hex[";
    auto limit = std::min<std::size_t>(bytes.size(), kPreviewBytes);
    for (std::size_t i = 0; i < limit; ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(bytes[i]);
    }
    if (bytes.size() > limit) {
        oss << " …";
    }
    oss << "]";
    return oss.str();
}

auto makeBufferCopy(SP::NodeData const& node) -> SP::SlidingBuffer {
    auto raw    = node.rawBuffer();
    auto offset = node.rawBufferFrontOffset();
    std::vector<std::uint8_t> copy(raw.begin(), raw.end());

    SP::SlidingBuffer buffer;
    buffer.assignRaw(std::move(copy), offset);
    return buffer;
}

auto activeBytes(SP::NodeData const& node) -> std::span<const std::uint8_t> {
    auto raw    = node.rawBuffer();
    auto offset = node.rawBufferFrontOffset();
    if (offset >= raw.size()) {
        return {};
    }
    return raw.subspan(offset);
}

template <typename T>
auto decodeValue(SP::SlidingBuffer buffer) -> std::optional<std::string> {
    auto decoded = SP::deserialize_pop<T>(buffer);
    if (!decoded) {
        return std::nullopt;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        return "\"" + *decoded + "\"";
    } else if constexpr (std::is_same_v<T, bool>) {
        return *decoded ? "true" : "false";
    } else if constexpr (std::is_floating_point_v<T>) {
        std::ostringstream oss;
        oss << *decoded;
        return oss.str();
    } else {
        return std::to_string(*decoded);
    }
}

auto tryDecodeSerialized(SP::NodeData const& node,
                         SP::ElementType const& type,
                         std::string const& typeName)
    -> std::optional<std::string> {
    auto matches = [&](std::type_info const& candidate,
                       std::initializer_list<std::string_view> names) -> bool {
        if (type.typeInfo == &candidate) {
            return true;
        }
        for (auto const& name : names) {
            if (typeName == name || typeName.find(name) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    auto decodeIf = [&](auto const& candidate,
                        std::initializer_list<std::string_view> names)
        -> std::optional<std::string> {
        using ValueT = std::decay_t<decltype(candidate)>;
        if (!matches(typeid(ValueT), names)) {
            return std::nullopt;
        }
        return decodeValue<ValueT>(makeBufferCopy(node));
    };

    if (auto decoded = decodeIf(std::string{}, {"basic_string", "std::string"}); decoded) {
        return decoded;
    }
    if (auto decoded = decodeIf(bool{}, {"bool"}); decoded) {
        return decoded;
    }
    if (auto decoded = decodeIf(std::int32_t{}, {"int", "std::int32_t"}); decoded) {
        return decoded;
    }
    if (auto decoded = decodeIf(std::uint32_t{}, {"unsigned int", "std::uint32_t"}); decoded) {
        return decoded;
    }
    if (auto decoded = decodeIf(std::int64_t{}, {"long long", "std::int64_t"}); decoded) {
        return decoded;
    }
    if (auto decoded = decodeIf(std::uint64_t{}, {"unsigned long long", "std::uint64_t"}); decoded) {
        return decoded;
    }
    if (auto decoded = decodeIf(float{}, {"float"}); decoded) {
        return decoded;
    }
    if (auto decoded = decodeIf(double{}, {"double"}); decoded) {
        return decoded;
    }

    if (typeName.find("basic_string") != std::string::npos ||
        typeName.find("std::string") != std::string::npos) {
        auto bytes = activeBytes(node);
        if (bytes.size() >= sizeof(std::uint32_t)) {
            std::uint32_t length = 0;
            std::memcpy(&length, bytes.data(), sizeof(std::uint32_t));
            if (bytes.size() >= sizeof(std::uint32_t) + static_cast<std::size_t>(length)) {
                auto dataPtr = reinterpret_cast<char const*>(bytes.data() + sizeof(std::uint32_t));
                std::string str(dataPtr, dataPtr + length);
                return "\"" + str + "\"";
            }
        }
    }

    return std::nullopt;
}

auto computeDigest(std::span<const std::uint8_t> bytes) -> std::uint64_t {
    constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
    constexpr std::uint64_t kFNVPrime  = 1099511628211ull;

    std::uint64_t hash = kFNVOffset;
    for (auto byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFNVPrime;
    }
    return hash;
}

auto joinPath(std::string_view root, std::vector<std::string> const& components) -> std::string {
    std::string result{root};
    if (result.empty()) {
        result = "/";
    }
    if (result.back() == '/' && result.size() > 1) {
        result.pop_back();
    }
    for (auto const& component : components) {
        if (result.empty() || result.back() != '/') {
            result.push_back('/');
        } else if (result.size() == 1 && result[0] == '/') {
            // root '/' already handled
        }
        if (component == ".") {
            continue;
        }
        result.append(component);
    }
    if (result.empty()) {
        return "/";
    }
    return result;
}

auto decodeNodePayload(CowSubtreePrototype::Node const& node,
                       std::string_view rootPath,
                       std::vector<std::string> const& components)
    -> std::optional<DecodedValue> {
    if (!node.payload.bytes) {
        return std::nullopt;
    }

    auto dataOpt = SP::NodeData::deserializeSnapshot(
        std::span<const std::byte>(node.payload.bytes->data(), node.payload.bytes->size()));
    DecodedValue decoded;
    decoded.path  = joinPath(rootPath, components);
    decoded.bytes = node.payload.size();

    if (!dataOpt) {
        decoded.typeName = "<unavailable>";
        decoded.category = "unknown";
        decoded.summary  = "Failed to deserialize node payload";
        decoded.digest   = computeDigest(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(node.payload.bytes->data()),
            node.payload.bytes->size()));
        return decoded;
    }

    auto const& nodeData = *dataOpt;
    auto const& types    = nodeData.typeSummary();

    if (types.empty()) {
        decoded.typeName = "<empty>";
        decoded.category = "empty";
        decoded.summary  = "(no values)";
        decoded.digest   = computeDigest(nodeData.rawBuffer());
        return decoded;
    }

    auto const& front = types.front();
    decoded.typeName  = front.typeInfo ? demangle(front.typeInfo->name()) : "<null>";
    decoded.category  = categoryToString(front.category);
    decoded.digest    = computeDigest(nodeData.rawBuffer());

    switch (front.category) {
    case SP::DataCategory::SerializedData: {
        auto value = tryDecodeSerialized(nodeData, front, decoded.typeName);
        if (value) {
            decoded.summary = *value;
        } else {
            decoded.summary = formatHexPreview(nodeData.rawBuffer());
        }
        break;
    }
    case SP::DataCategory::Execution:
        decoded.summary = "<execution payload>";
        break;
    default:
        decoded.summary = "(unsupported category)";
        break;
    }

    if (front.elements > 1) {
        decoded.summary.append(" (+");
        decoded.summary.append(std::to_string(front.elements - 1));
        decoded.summary.append(" queued)");
    }

    return decoded;
}

void traverseSnapshot(CowSubtreePrototype::NodePtr const& node,
                      std::string_view rootPath,
                      std::vector<std::string>& components,
                      std::vector<DecodedValue>& out) {
    if (!node) {
        return;
    }

    if (auto decoded = decodeNodePayload(*node, rootPath, components)) {
        out.push_back(std::move(*decoded));
    }

    for (auto const& [key, child] : node->children) {
        components.push_back(key);
        traverseSnapshot(child, rootPath, components, out);
        components.pop_back();
    }
}

auto decodeSnapshotToMap(CowSubtreePrototype::Snapshot const& snapshot,
                         std::string_view rootPath)
    -> std::unordered_map<std::string, DecodedValue> {
    std::unordered_map<std::string, DecodedValue> map;
    auto summary = SP::History::Inspection::decodeSnapshot(snapshot, rootPath);
    for (auto& value : summary.values) {
        map.emplace(value.path, value);
    }
    return map;
}

auto serializeStatsField(std::string_view key, std::string_view value) -> std::string {
    std::ostringstream oss;
    oss << "\"" << key << "\": " << value;
    return oss.str();
}

auto serializeUint(std::uint64_t value) -> std::string {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

auto escapeJson(std::string const& input) -> std::string {
    std::ostringstream oss;
    for (auto ch : input) {
        switch (ch) {
        case '\"': oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\b': oss << "\\b"; break;
        case '\f': oss << "\\f"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
            } else {
                oss << ch;
            }
            break;
        }
    }
    return oss.str();
}

} // namespace

namespace SP::History::Inspection {

auto decodeSnapshot(CowSubtreePrototype::Snapshot const& snapshot,
                    std::string_view rootPath) -> SnapshotSummary {
    SnapshotSummary summary;
    if (!snapshot.valid()) {
        return summary;
    }

    std::vector<std::string> components;
    traverseSnapshot(snapshot.root, rootPath, components, summary.values);
    std::sort(summary.values.begin(), summary.values.end(),
              [](DecodedValue const& a, DecodedValue const& b) {
                  return a.path < b.path;
              });
    return summary;
}

auto diffSnapshots(CowSubtreePrototype::Snapshot const& baseline,
                   CowSubtreePrototype::Snapshot const& updated,
                   std::string_view rootPath) -> SnapshotDiff {
    SnapshotDiff diff;
    auto before = decodeSnapshotToMap(baseline, rootPath);
    auto after  = decodeSnapshotToMap(updated, rootPath);

    for (auto const& [path, value] : after) {
        auto it = before.find(path);
        if (it == before.end()) {
            diff.added.push_back(value);
        } else {
            auto const& previous = it->second;
            if (previous.digest != value.digest || previous.summary != value.summary
                || previous.bytes != value.bytes) {
                diff.modified.push_back(ModifiedValue{previous, value});
            }
        }
    }
    for (auto const& [path, value] : before) {
        if (!after.contains(path)) {
            diff.removed.push_back(value);
        }
    }

    auto sortValues = [](auto& container) {
        std::sort(container.begin(), container.end(),
                  [](auto const& lhs, auto const& rhs) {
                      return lhs.path < rhs.path;
                  });
    };

    sortValues(diff.added);
    sortValues(diff.removed);
    std::sort(diff.modified.begin(), diff.modified.end(),
              [](ModifiedValue const& a, ModifiedValue const& b) {
                  return a.before.path < b.before.path;
              });

    return diff;
}

auto historyStatsToJson(HistoryStats const& stats) -> std::string {
    std::vector<std::string> fields;
    fields.push_back(serializeStatsField("undoCount", serializeUint(stats.counts.undo)));
    fields.push_back(serializeStatsField("redoCount", serializeUint(stats.counts.redo)));
    fields.push_back(serializeStatsField("undoBytes", serializeUint(stats.bytes.undo)));
    fields.push_back(serializeStatsField("redoBytes", serializeUint(stats.bytes.redo)));
    fields.push_back(serializeStatsField("liveBytes", serializeUint(stats.bytes.live)));
    fields.push_back(serializeStatsField("bytesRetained", serializeUint(stats.bytes.total)));
    fields.push_back(serializeStatsField("manualGcEnabled",
                                         stats.counts.manualGarbageCollect ? "true" : "false"));
    fields.push_back(serializeStatsField("trimOperationCount",
                                         serializeUint(stats.trim.operationCount)));
    fields.push_back(serializeStatsField("trimmedEntries",
                                         serializeUint(stats.trim.entries)));
    fields.push_back(serializeStatsField("trimmedBytes",
                                         serializeUint(stats.trim.bytes)));
    fields.push_back(serializeStatsField("lastTrimTimestampMs",
                                         serializeUint(stats.trim.lastTimestampMs)));
    fields.push_back(serializeStatsField("diskBytes", serializeUint(stats.bytes.disk)));
    fields.push_back(serializeStatsField("diskEntries", serializeUint(stats.counts.diskEntries)));
    fields.push_back(serializeStatsField("cachedUndo", serializeUint(stats.counts.cachedUndo)));
    fields.push_back(serializeStatsField("cachedRedo", serializeUint(stats.counts.cachedRedo)));

    std::ostringstream oss;
    oss << "{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << fields[i];
    }
    oss << "}";
    return oss.str();
}

auto lastOperationToJson(std::optional<HistoryLastOperation> const& op) -> std::string {
    if (!op) {
        return "null";
    }
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"" << escapeJson(op->type) << "\","
        << "\"timestampMs\":" << op->timestampMs << ","
        << "\"durationMs\":" << op->durationMs << ","
        << "\"success\":" << (op->success ? "true" : "false") << ","
        << "\"undoCountBefore\":" << op->undoCountBefore << ","
        << "\"undoCountAfter\":" << op->undoCountAfter << ","
        << "\"redoCountBefore\":" << op->redoCountBefore << ","
        << "\"redoCountAfter\":" << op->redoCountAfter << ","
        << "\"bytesBefore\":" << op->bytesBefore << ","
        << "\"bytesAfter\":" << op->bytesAfter << ","
        << "\"message\":\"" << escapeJson(op->message) << "\""
        << "}";
    return oss.str();
}

} // namespace SP::History::Inspection
#include <charconv>
#include <initializer_list>

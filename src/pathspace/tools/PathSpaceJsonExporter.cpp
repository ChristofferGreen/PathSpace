#include "tools/PathSpaceJsonExporter.hpp"

#include "PathSpaceBase.hpp"
#include "core/ElementType.hpp"
#include "core/NodeData.hpp"
#include "tools/PathSpaceJsonConverters.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace SP {

namespace {

using Json = nlohmann::json;

auto dataCategoryToString(DataCategory category) -> std::string_view {
    switch (category) {
    case DataCategory::None:
        return "None";
    case DataCategory::SerializedData:
        return "SerializedData";
    case DataCategory::Execution:
        return "Execution";
    case DataCategory::FunctionPointer:
        return "FunctionPointer";
    case DataCategory::Fundamental:
        return "Fundamental";
    case DataCategory::SerializationLibraryCompatible:
        return "SerializationLibraryCompatible";
    case DataCategory::UniquePtr:
        return "UniquePtr";
    }
    return "Unknown";
}

struct ConverterEntry {
    detail::PathSpaceJsonConverterFn fn;
    std::string                     typeName;
};

class NodeDataValueReader final : public detail::PathSpaceJsonValueReader {
public:
    explicit NodeDataValueReader(NodeData data)
        : snapshot_(std::move(data)) {
        auto const& summary = snapshot_.typeSummary();
        types_.assign(summary.begin(), summary.end());
    }

    auto queueTypes() const -> std::vector<ElementType> const& { return types_; }

private:
    auto popImpl(void* destination, InputMetadata const& metadata) -> std::optional<Error> override {
        if (auto error = snapshot_.deserializePop(destination, metadata)) {
            return error;
        }
        if (nextIndex_ < types_.size()) {
            ++nextIndex_;
        }
        return std::nullopt;
    }

    NodeData                 snapshot_;
    std::vector<ElementType> types_;
    std::size_t              nextIndex_ = 0;
};

auto makeReader(ValueHandle const& handle) -> std::unique_ptr<NodeDataValueReader> {
    auto serialized = VisitDetail::Access::SerializeNodeData(handle);
    if (!serialized) {
        return nullptr;
    }
    std::span<const std::byte> bytes(serialized->data(), serialized->size());
    auto snapshot = NodeData::deserializeSnapshot(bytes);
    if (!snapshot) {
        return nullptr;
    }
    return std::make_unique<NodeDataValueReader>(std::move(*snapshot));
}

std::unordered_map<std::type_index, ConverterEntry> gConverters;
std::mutex                                         gConverterMutex;
std::once_flag                                     gDefaultConvertersFlag;

void registerDefaultConverters() {
    std::call_once(gDefaultConvertersFlag, [] {
        PathSpaceJsonRegisterConverterAs<bool>("bool", [](bool value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::int8_t>("int8_t", [](std::int8_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::uint8_t>("uint8_t", [](std::uint8_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::int16_t>("int16_t", [](std::int16_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::uint16_t>("uint16_t", [](std::uint16_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::int32_t>("int32_t", [](std::int32_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::uint32_t>("uint32_t", [](std::uint32_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::int64_t>("int64_t", [](std::int64_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::uint64_t>("uint64_t", [](std::uint64_t value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<float>("float", [](float value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<double>("double", [](double value) { return Json(value); });
        PathSpaceJsonRegisterConverterAs<std::string>("std::string", [](std::string const& value) { return Json(value); });
    });
}

struct ExportStats {
    std::size_t nodeCount             = 0;
    std::size_t valuesExported        = 0;
    std::size_t childrenTruncated     = 0;
    std::size_t valuesTruncated       = 0;
    std::size_t depthLimited          = 0;
};

auto visitLimit(std::size_t limit) -> Json {
    if (limit == std::numeric_limits<std::size_t>::max()) {
        return Json("unlimited");
    }
    return Json(limit);
}

auto placeholderFor(DataCategory category, std::string_view typeName, std::string_view reason) -> Json {
    Json placeholder = Json::object();
    placeholder["placeholder"] = "opaque";
    placeholder["category"]    = dataCategoryToString(category);
    placeholder["type"]        = typeName.empty() ? Json(nullptr) : Json(typeName);
    placeholder["reason"]      = reason;
    return placeholder;
}

auto placeholderForExecution(std::string_view state) -> Json {
    Json placeholder = Json::object();
    placeholder["placeholder"] = "execution";
    placeholder["state"]       = state;
    return placeholder;
}

auto describeType(std::type_info const* info) -> std::string {
    if (!info) {
        return "unknown";
    }
    return detail::DescribeRegisteredType(std::type_index(*info));
}

void attachDiagnostics(Json& node, ValueSnapshot const& snapshot, bool includeDiagnostics) {
    if (!includeDiagnostics) {
        return;
    }
    Json diagnostics = Json::object();
    diagnostics["queue_depth"]            = snapshot.queueDepth;
    diagnostics["raw_bytes"]              = snapshot.rawBufferBytes;
    diagnostics["has_execution_payload"]  = snapshot.hasExecutionPayload;
    diagnostics["has_serialized_payload"] = snapshot.hasSerializedPayload;
    node["diagnostics"]                   = std::move(diagnostics);
}

auto buildValueEntry(ElementType const* element,
                     std::size_t        index,
                     NodeDataValueReader* reader,
                     PathSpaceJsonOptions const& options,
                     ExportStats&               stats) -> Json {
    Json value = Json::object();
    value["index"]    = index;
    auto typeLabel    = describeType(element ? element->typeInfo : nullptr);
    value["type"]     = typeLabel;
    value["category"] = element ? dataCategoryToString(element->category) : "Unknown";

    if (!reader) {
        if (options.includeOpaquePlaceholders) {
            if (element && element->category == DataCategory::Execution) {
                value.update(placeholderForExecution("pending"));
            } else {
                auto category = element ? element->category : DataCategory::None;
                value.update(placeholderFor(category, typeLabel, "sampling-disabled"));
            }
        }
        return value;
    }

    if (!element || !element->typeInfo) {
        if (options.includeOpaquePlaceholders) {
            auto category = element ? element->category : DataCategory::None;
            value.update(placeholderFor(category, typeLabel, "missing-type-info"));
        }
        return value;
    }

    auto maybeJson = detail::ConvertWithRegisteredConverter(std::type_index(*element->typeInfo), *reader);
    if (maybeJson) {
        value["value"] = std::move(*maybeJson);
        ++stats.valuesExported;
        return value;
    }

    if (options.includeOpaquePlaceholders) {
        value.update(placeholderFor(element->category, typeLabel, "converter-missing"));
    }
    return value;
}

struct TreeNode {
    Json data = Json::object();
    std::map<std::string, std::unique_ptr<TreeNode>> children;
};

auto splitComponents(std::string const& path) -> Expected<std::vector<std::string>> {
    ConcretePathStringView view{path};
    auto                   components = view.components();
    if (!components) {
        return std::unexpected(components.error());
    }
    return components.value();
}

auto ensureTreeNode(TreeNode& root,
                    std::vector<std::string> const& rootComponents,
                    std::vector<std::string> const& pathComponents) -> Expected<TreeNode*> {
    if (pathComponents.size() < rootComponents.size()) {
        return std::unexpected(Error{Error::Code::InvalidPath, "entry path outside export root"});
    }
    for (std::size_t idx = 0; idx < rootComponents.size(); ++idx) {
        if (pathComponents[idx] != rootComponents[idx]) {
            return std::unexpected(Error{Error::Code::InvalidPath, "entry path outside export root"});
        }
    }

    TreeNode* current = &root;
    for (std::size_t idx = rootComponents.size(); idx < pathComponents.size(); ++idx) {
        auto const& name = pathComponents[idx];
        auto        it   = current->children.find(name);
        if (it == current->children.end()) {
            it = current->children.emplace(name, std::make_unique<TreeNode>()).first;
        }
        current = it->second.get();
    }
    return current;
}

auto buildNode(PathEntry const& entry,
               ValueHandle&        handle,
               std::size_t         relativeDepth,
               PathSpaceJsonOptions const& options,
               ExportStats&               stats) -> Json {
    Json node = Json::object();
    bool includeStructure = options.includeStructureFields || options.includeDiagnostics;
    bool childLimitHit = entry.hasChildren && options.visit.childLimitEnabled()
                      && entry.approxChildCount > options.visit.maxChildren;
    bool depthLimited  = entry.hasChildren
                      && options.visit.maxDepth != VisitOptions::kUnlimitedDepth
                      && relativeDepth == options.visit.maxDepth;
    bool childrenTruncated = childLimitHit || depthLimited;
    if (childrenTruncated) {
        ++stats.childrenTruncated;
    }
    if (depthLimited) {
        ++stats.depthLimited;
    }

    node["values"] = Json::array();

    auto snapshot = handle.snapshot();
    if (!snapshot) {
        if (includeStructure) {
            node["value_error"] = describeError(snapshot.error());
        }
        return node;
    }

    attachDiagnostics(node, *snapshot, options.includeDiagnostics);

    auto reader = makeReader(handle);
    auto readerQueueSize = reader ? reader->queueTypes().size() : static_cast<std::size_t>(snapshot->queueDepth);

    if (!options.visit.includeValues || options.maxQueueEntries == 0) {
        bool truncated = entry.hasValue && readerQueueSize > 0 && options.maxQueueEntries == 0;
        if (truncated) {
            ++stats.valuesTruncated;
        }
        if (includeStructure) {
            node["values_truncated"] = truncated;
            node["values_sampled"]   = options.visit.includeValues;
        }
    } else if (entry.hasValue) {
        auto limit = std::min(readerQueueSize, options.maxQueueEntries);
        bool valuesTruncated = readerQueueSize > limit;
        if (valuesTruncated) {
            ++stats.valuesTruncated;
        }
        if (includeStructure) {
            node["values_truncated"] = valuesTruncated;
            node["values_sampled"]   = options.visit.includeValues;
        }

        for (std::size_t idx = 0; idx < limit; ++idx) {
            ElementType const* element = nullptr;
            if (reader && idx < reader->queueTypes().size()) {
                element = &reader->queueTypes()[idx];
            } else if (idx < snapshot->types.size()) {
                element = &snapshot->types[idx];
            }

            auto valueEntry = buildValueEntry(element, idx, reader.get(), options, stats);
            node["values"].push_back(std::move(valueEntry));
        }
    } else {
        if (includeStructure) {
            node["values_truncated"] = false;
            node["values_sampled"]   = options.visit.includeValues;
        }
    }

    if (includeStructure) {
        node["has_value"]        = entry.hasValue;
        node["has_children"]     = entry.hasChildren;
        node["has_nested_space"] = entry.hasNestedSpace;
        node["child_count"]      = entry.approxChildCount;
        node["category"]         = dataCategoryToString(entry.frontCategory);
        node["children_truncated"] = childrenTruncated;
        node["depth_truncated"]    = depthLimited;
    }

    return node;
}

auto emitTree(TreeNode const& node) -> Json {
    Json out = node.data.is_object() ? node.data : Json::object();
    Json children = Json::object();
    for (auto const& [name, child] : node.children) {
        children[name] = emitTree(*child);
    }
    // Drop empty values/children to keep dumps concise, but keep present/empty strings.
    if (out.contains("values") && out["values"].is_array() && out["values"].empty()) {
        out.erase("values");
    }
    if (!children.empty()) {
        out["children"] = std::move(children);
    }
    return out;
}

} // namespace

namespace detail {

auto RegisterPathSpaceJsonConverter(std::type_index type,
                                    std::string_view typeName,
                                    PathSpaceJsonConverterFn fn) -> void {
    std::lock_guard<std::mutex> guard(gConverterMutex);
    gConverters[type] = ConverterEntry{.fn = std::move(fn), .typeName = std::string(typeName)};
}

auto ConvertWithRegisteredConverter(std::type_index type, PathSpaceJsonValueReader& reader)
    -> std::optional<nlohmann::json> {
    std::optional<ConverterEntry> entry;
    {
        std::lock_guard<std::mutex> guard(gConverterMutex);
        if (auto it = gConverters.find(type); it != gConverters.end()) {
            entry = it->second;
        }
    }
    if (!entry) {
        return std::nullopt;
    }
    return entry->fn(reader);
}

auto DescribeRegisteredType(std::type_index type) -> std::string {
    std::lock_guard<std::mutex> guard(gConverterMutex);
    if (auto it = gConverters.find(type); it != gConverters.end()) {
        return it->second.typeName;
    }
    return type.name();
}

} // namespace detail

auto PathSpaceJsonExporter::Export(PathSpaceBase& space, PathSpaceJsonOptions const& options)
    -> Expected<std::string> {
    registerDefaultConverters();

    PathSpaceJsonOptions opts = options;
    if (opts.mode == PathSpaceJsonOptions::Mode::Debug) {
        opts.includeDiagnostics        = true;
        opts.includeOpaquePlaceholders = true;
        opts.includeStructureFields    = true;
        opts.includeMetadata           = true;
    } else {
        opts.includeDiagnostics        = false;
        opts.includeOpaquePlaceholders = false;
        opts.mode                      = PathSpaceJsonOptions::Mode::Minimal;
    }

    auto rootComponents = splitComponents(opts.visit.root);
    if (!rootComponents) {
        return std::unexpected(rootComponents.error());
    }

    TreeNode    rootNode;
    ExportStats stats{};
    std::optional<Error> visitError;

    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            auto entryComponents = splitComponents(entry.path);
            if (!entryComponents) {
                visitError = entryComponents.error();
                return VisitControl::Stop;
            }

            auto nodePtr = ensureTreeNode(rootNode, *rootComponents, *entryComponents);
            if (!nodePtr) {
                visitError = nodePtr.error();
                return VisitControl::Stop;
            }

            auto relativeDepth = entryComponents->size() >= rootComponents->size()
                                      ? entryComponents->size() - rootComponents->size()
                                      : 0;
            nodePtr.value()->data = buildNode(entry, handle, relativeDepth, opts, stats);
            ++stats.nodeCount;
            return VisitControl::Continue;
        },
        opts.visit);

    if (!visitResult) {
        return std::unexpected(visitResult.error());
    }
    if (visitError) {
        return std::unexpected(*visitError);
    }

    Json limits = Json::object();
    limits["max_depth"]         = visitLimit(opts.visit.maxDepth);
    limits["max_children"]      = VisitOptions::isUnlimitedChildren(opts.visit.maxChildren)
                                       ? Json("unlimited")
                                       : Json(opts.visit.maxChildren);
    limits["max_queue_entries"] = opts.maxQueueEntries;

    Json flags = Json::object();
    flags["include_nested_spaces"]       = opts.visit.includeNestedSpaces;
    flags["include_values"]              = opts.visit.includeValues;
    flags["include_opaque_placeholders"] = opts.includeOpaquePlaceholders;
    flags["include_diagnostics"]         = opts.includeDiagnostics;
    flags["include_structure_fields"]    = opts.includeStructureFields;
    flags["include_metadata"]            = opts.includeMetadata;
    std::string modeLabel = opts.mode == PathSpaceJsonOptions::Mode::Debug ? "debug" : "minimal";
    flags["mode"] = std::move(modeLabel);

    Json statsJson = Json::object();
    statsJson["node_count"]         = stats.nodeCount;
    statsJson["value_entries"]      = stats.valuesExported;
    statsJson["children_truncated"] = stats.childrenTruncated;
    statsJson["depth_truncated"]    = stats.depthLimited;
    statsJson["values_truncated"]   = stats.valuesTruncated;

    Json root = Json::object();
    if (opts.includeMetadata) {
        Json meta = Json::object();
        meta["schema"] = "hierarchical";
        meta["root"]   = opts.visit.root;
        meta["limits"] = std::move(limits);
        meta["flags"]  = std::move(flags);
        meta["stats"]  = std::move(statsJson);
        root["_meta"]  = std::move(meta);
    }
    root[opts.visit.root] = emitTree(rootNode);

    auto indent = opts.dumpIndent;
    if (indent < 0) {
        return root.dump();
    }
    return root.dump(indent);
}

auto PathSpaceBase::toJSON(PathSpaceJsonOptions const& options) -> Expected<std::string> {
    return PathSpaceJsonExporter::Export(*this, options);
}

} // namespace SP

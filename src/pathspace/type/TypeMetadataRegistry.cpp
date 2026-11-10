#include "type/TypeMetadataRegistry.hpp"

#include <mutex>

namespace SP {

void RegisterBuiltinTypeMetadata(TypeMetadataRegistry& registry);

auto TypeMetadataRegistry::make_view(TypeMetadataRegistry::Entry const* entry)
    -> std::optional<TypeMetadataView> {
    if (entry == nullptr) {
        return std::nullopt;
    }
    return TypeMetadataView{entry->type_name, entry->metadata, entry->operations};
}

TypeMetadataRegistry& TypeMetadataRegistry::instance() {
    static TypeMetadataRegistry registry;
    // call_once blocks concurrent first callers until the builtins are in, so no caller can see
    // the registry half filled, and later calls do not register again.
    static std::once_flag builtinsRegistered;
    std::call_once(builtinsRegistered, [] { RegisterBuiltinTypeMetadata(registry); });
    return registry;
}

std::optional<TypeMetadataView> TypeMetadataRegistry::findByName(std::string_view type_name) const {
    std::lock_guard<std::mutex> lock(registryMutex);
    auto                        it = byName.find(type_name);
    if (it == byName.end()) {
        return std::nullopt;
    }
    return make_view(it->second);
}

std::optional<TypeMetadataView> TypeMetadataRegistry::findByType(std::type_index type) const {
    std::lock_guard<std::mutex> lock(registryMutex);
    auto                        it = byType.find(type);
    if (it == byType.end()) {
        return std::nullopt;
    }
    return make_view(it->second);
}

bool TypeMetadataRegistry::registerEntry(std::type_index      type_index,
                                         std::string          type_name,
                                         const InputMetadata& metadata,
                                         TypeOperations       operations) {
    if (metadata.typeInfo == nullptr || type_name.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(registryMutex);
    if (byName.find(type_name) != byName.end()) {
        return false;
    }
    if (byType.find(type_index) != byType.end()) {
        return false;
    }

    auto entry        = std::make_unique<Entry>();
    entry->type_name = std::move(type_name);
    entry->metadata   = metadata;
    entry->operations = operations;

    auto* raw = entry.get();
    entries.push_back(std::move(entry));
    byName.emplace(raw->type_name, raw);
    byType.emplace(type_index, raw);
    return true;
}

} // namespace SP

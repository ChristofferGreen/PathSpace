#include "type/TypeMetadataRegistry.hpp"

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
    RegisterBuiltinTypeMetadata(registry);
    return registry;
}

std::optional<TypeMetadataView> TypeMetadataRegistry::findByName(std::string_view type_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = by_name_.find(type_name);
    if (it == by_name_.end()) {
        return std::nullopt;
    }
    return make_view(it->second);
}

std::optional<TypeMetadataView> TypeMetadataRegistry::findByType(std::type_index type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = by_type_.find(type);
    if (it == by_type_.end()) {
        return std::nullopt;
    }
    return make_view(it->second);
}

bool TypeMetadataRegistry::registerEntry(std::type_index type_index,
                                         std::string type_name,
                                         InputMetadata metadata,
                                         TypeOperations operations) {
    if (metadata.typeInfo == nullptr || type_name.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (by_name_.find(type_name) != by_name_.end()) {
        return false;
    }
    if (by_type_.find(type_index) != by_type_.end()) {
        return false;
    }

    auto entry        = std::make_unique<Entry>();
    entry->type_name = std::move(type_name);
    entry->metadata  = metadata;
    entry->operations = operations;

    auto* raw = entry.get();
    entries_.push_back(std::move(entry));
    by_name_.emplace(raw->type_name, raw);
    by_type_.emplace(type_index, raw);
    return true;
}

} // namespace SP

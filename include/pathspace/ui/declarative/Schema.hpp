#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace SP::UI::Declarative {

enum class Requirement {
    Required,
    Optional,
    RuntimeManaged,
};

enum class NodeKind {
    Directory,
    Value,
    Callable,
    Queue,
    Flag,
};

struct SchemaEntry {
    std::string_view path;
    NodeKind kind;
    Requirement requirement;
    std::string_view description;
};

struct SchemaEntryView {
    SchemaEntry const* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] auto begin() const -> SchemaEntry const* { return data; }
    [[nodiscard]] auto end() const -> SchemaEntry const* { return data + size; }
    [[nodiscard]] auto empty() const -> bool { return size == 0; }
    [[nodiscard]] auto span() const -> std::span<SchemaEntry const> { return {data, size}; }
};

struct NamespaceSchema {
    std::string_view name;
    std::string_view description;
    SchemaEntryView entries;
};

struct WidgetSchema {
    std::string_view kind;
    std::string_view description;
    SchemaEntryView common;
    SchemaEntryView specifics;
};

[[nodiscard]] auto declarative_namespaces() -> std::span<NamespaceSchema const>;
[[nodiscard]] auto widget_schemas() -> std::span<WidgetSchema const>;
[[nodiscard]] auto find_namespace_schema(std::string_view name) -> NamespaceSchema const*;
[[nodiscard]] auto find_widget_schema(std::string_view kind) -> WidgetSchema const*;

} // namespace SP::UI::Declarative


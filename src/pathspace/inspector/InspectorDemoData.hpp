#pragma once

#include <string>

namespace SP {

class PathSpace;

namespace Inspector {

// Seeds the provided PathSpace with the demo tree used by the inspector UI and
// JSON tooling so that CLIs and tests can exercise the exporter without
// reaching into private Node structures.
auto SeedInspectorDemoData(PathSpace& space) -> void;

} // namespace Inspector
} // namespace SP


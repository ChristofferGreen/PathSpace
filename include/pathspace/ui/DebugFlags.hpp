#pragma once

#include <string_view>

namespace SP::UI {

// Returns true when extended PathSpace diagnostics/metrics/hints should be
// written. Defaults to false; enable via PATHSPACE_UI_DEBUG_TREE=1 (aliases
// PATHSPACE_UI_DEBUG_DIAGNOSTICS or PATHSPACE_UI_DEBUG_PATHSPACE).
[[nodiscard]] auto DebugTreeWritesEnabled() -> bool;

} // namespace SP::UI

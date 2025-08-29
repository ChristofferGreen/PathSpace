#pragma once
//
// Deprecated: macOS-specific PathIO backend skeletons have been removed.
//
// Use the unified providers instead:
//   - layer/PathIOMouse.hpp
//   - layer/PathIOKeyboard.hpp
//
// Platform selection and OS integration are now handled internally by these
// unified classes via compile-time conditionals (e.g., PATHIO_BACKEND_MACOS).
// This header remains as a compatibility include and intentionally defines
// no macOS-specific classes (e.g., PathIOMouseMacOS, PathIOKeyboardMacOS).
//
// To silence the deprecation notice from this header, define:
//   PATHSPACE_SUPPRESS_DEPRECATED_BACKENDS
//

#ifndef PATHSPACE_SUPPRESS_DEPRECATED_BACKENDS
  #if defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
    #pragma message("PathSpace: 'layer/macos/PathIO_macos.hpp' is deprecated. Include 'layer/io/PathIOMouse.hpp' and/or 'layer/io/PathIOKeyboard.hpp' instead.")
  #endif
#endif

#include "layer/io/PathIOMouse.hpp"
#include "layer/io/PathIOKeyboard.hpp"

namespace SP {
// Intentionally empty: no macOS-specific types are declared here anymore.
} // namespace SP
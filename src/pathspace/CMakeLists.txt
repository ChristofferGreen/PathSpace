# if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
# endif()

include(FetchContent)

# FetchContent_Declare(
# nlohmann
# GIT_REPOSITORY https://github.com/nlohmann/json.git
# GIT_TAG v3.11.3
# )
# FetchContent_MakeAvailable(nlohmann)
FetchContent_Declare(
    ParallelHashmap
    GIT_REPOSITORY https://github.com/greg7mdp/parallel-hashmap.git
    GIT_TAG v2.0.0
)
FetchContent_MakeAvailable(ParallelHashmap)

# FetchContent_Declare(
# glaze
# GIT_REPOSITORY https://github.com/stephenberry/glaze.git
# GIT_TAG main
# GIT_SHALLOW TRUE
# )
# FetchContent_MakeAvailable(glaze)
FetchContent_Declare(
    alpaca
    GIT_TAG 6bed0a0
    GIT_REPOSITORY https://github.com/p-ranav/alpaca.git
)
FetchContent_MakeAvailable(alpaca)

# FetchContent_Declare(
# foonathan_memory
# GIT_TAG 016c9fb
# GIT_REPOSITORY https://github.com/foonathan/memory
# )
# FetchContent_MakeAvailable(foonathan_memory)

# Indicate that we only want to compile source files into a library
add_library(PathSpace
    path/Path.cpp
    path/ConcreteName.cpp
    path/ConcretePathIterator.cpp
    path/ConcretePath.cpp
    path/GlobName.cpp
    path/GlobPathIterator.cpp
    path/GlobPath.cpp
    path/UnvalidatedPath.cpp
    path/Iterator.cpp
    path/utils.cpp
    core/NodeData.cpp
    core/WaitMap.cpp
    core/WatchRegistry.hpp
    core/Leaf.cpp
    task/Task.cpp
    task/TaskPool.cpp
    task/TaskStateAtomic.cpp
    task/Future.cpp
    type/SlidingBuffer.cpp
    log/TaggedLogger.cpp
    PathSpace.cpp
)

if(PATHSPACE_ENABLE_APP)
    target_sources(PathSpace PRIVATE app/AppPaths.cpp)
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_ENABLE_APP=1)
else()
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_ENABLE_APP=0)
endif()

if(PATHSPACE_ENABLE_EXTRA)
    target_sources(PathSpace PRIVATE
        layer/PathView.cpp
        layer/io/PathFileSystem.cpp
        layer/io/PathIOGamepad.cpp
        layer/io/PathIOPointerMixer.cpp
        layer/io/PathIOMouse.cpp
        layer/io/PathIOKeyboard.cpp
        layer/io/PathIODeviceDiscovery.cpp
        layer/io/PathIOStdOut.cpp
    )
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_ENABLE_EXTRA=1)
else()
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_ENABLE_EXTRA=0)
endif()

if(PATHSPACE_ENABLE_UI)
    target_sources(PathSpace PRIVATE
        ui/Helpers.cpp
        ui/Builders.cpp
        ui/SceneSnapshotBuilder.cpp
        ui/ProgressiveSurfaceBuffer.cpp
        ui/PathSurfaceSoftware.cpp
        ui/PathRenderer2D.cpp
        ui/PathWindowView.cpp
        ui/ImageCache.cpp
    )
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_ENABLE_UI=1)
    if(PATHSPACE_UI_SOFTWARE)
        target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_SOFTWARE=1)
    else()
        target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_SOFTWARE=0)
    endif()
    if(PATHSPACE_UI_METAL)
        target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_METAL=1)
    else()
        target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_METAL=0)
    endif()
    if(APPLE)
        target_link_libraries(PathSpace PRIVATE "-framework IOSurface")
    endif()
else()
    target_compile_definitions(PathSpace PUBLIC
        PATHSPACE_ENABLE_UI=0
        PATHSPACE_UI_SOFTWARE=0
        PATHSPACE_UI_METAL=0
    )
endif()

# macOS backend options and integration
if(APPLE)
    if(PATHSPACE_ENABLE_EXTRA)
        # Option to enable CGEventTap-based mouse/keyboard OS backend in providers
        option(PATHSPACE_ENABLE_MOUSE_OS_TAP "Enable macOS CGEventTap backend in IO providers" OFF)

        # Define PATHIO_BACKEND_MACOS only when OS backends are enabled (prevents CGEventTap code in unit tests)
        if(PATHSPACE_ENABLE_MOUSE_OS_TAP)
            target_compile_definitions(PathSpace PRIVATE PATHIO_BACKEND_MACOS=1)
        endif()

        # Add ObjC++ GameController helper and enable ARC (always safe; uses weak imports and dynamic selectors)
        target_sources(PathSpace PRIVATE layer/macos/GameControllerHaptics.mm)
        set_source_files_properties(layer/macos/GameControllerHaptics.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

        # Link required Apple frameworks
        # - Foundation, Cocoa for Objective‑C runtime and windowing helpers (used by examples and helpers)
        # - GameController for haptics/input helper (weak/dynamic – linking is safe if framework present)
        # - ApplicationServices only when CGEventTap backend is enabled (provides CGEventTap symbols)
        target_link_libraries(PathSpace PRIVATE
            "-framework Foundation"
            "-framework Cocoa"
            "-framework GameController"
        )
        if(PATHSPACE_ENABLE_MOUSE_OS_TAP)
            target_link_libraries(PathSpace PRIVATE "-framework ApplicationServices")
        endif()
    endif()
endif()

target_compile_options(PathSpace PUBLIC "-Wno-deprecated")

target_include_directories(PathSpace
    PUBLIC
    ${CMAKE_SOURCE_DIR}/src/pathspace
    ${CMAKE_SOURCE_DIR}/src/
    ${CMAKE_SOURCE_DIR}/include
    ${parallelhashmap_SOURCE_DIR}
    ${nlohmann_SOURCE_DIR}/include
    ${cereal_SOURCE_DIR}/include
    ${glaze_SOURCE_DIR}/include
    ${cista_SOURCE_DIR}/include
    ${alpaca_SOURCE_DIR}/include
    ${foonathan_memory_SOURCE_DIR}/include
)

# Opt-in debug logging to keep test runs fast by default
option(PATHSPACE_ENABLE_LOG_DEBUG "Enable SP_LOG_DEBUG logging" OFF)
if(PATHSPACE_ENABLE_LOG_DEBUG)
    target_compile_definitions(PathSpace PRIVATE SP_LOG_DEBUG)
endif()

target_link_libraries(PathSpace
    PRIVATE

    # glaze::glaze
    # foonathan_memory
)

set(_pathspace_public_headers "PathSpace.hpp")
if(PATHSPACE_ENABLE_EXTRA)
    list(APPEND _pathspace_public_headers
        layer/io/PathIOMouse.hpp
        layer/io/PathIOKeyboard.hpp
        layer/io/PathIOStdOut.hpp
        layer/PathAlias.hpp
        layer/io/PathIODeviceDiscovery.hpp
        layer/io/PathIOPointerMixer.hpp
        layer/io/PathIOGamepad.hpp
    )
endif()
list(JOIN _pathspace_public_headers ";" _pathspace_public_header_string)
set_target_properties(PathSpace PROPERTIES PUBLIC_HEADER "${_pathspace_public_header_string}")

install(TARGETS PathSpace
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

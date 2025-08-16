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
    path/Iterator.cpp
    path/utils.cpp
    core/NodeData.cpp
    core/WaitMap.cpp
    core/WatchRegistry.hpp
    core/Leaf.cpp
    layer/PathView.cpp
    layer/PathFileSystem.cpp
    layer/PathIO.cpp
    task/Task.cpp
    task/TaskPool.cpp
    task/TaskStateAtomic.cpp
    task/Future.cpp
    type/SlidingBuffer.cpp
    log/TaggedLogger.cpp
    PathSpace.cpp
)

# macOS-specific Objective-C++ implementation for PathIO backends (optional)
if(APPLE AND ENABLE_PATHIO_MACOS)
    target_sources(PathSpace PRIVATE layer/macos/PathIO_macos.mm)
    # Ensure Objective-C++ language for .mm on Apple/Clang
    set_source_files_properties(layer/macos/PathIO_macos.mm PROPERTIES LANGUAGE CXX)
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

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(PathSpace PRIVATE SP_LOG_DEBUG)
endif()

target_link_libraries(PathSpace
    PRIVATE

    # glaze::glaze
    # foonathan_memory
)

set_target_properties(PathSpace PROPERTIES PUBLIC_HEADER "PathSpace.hpp;layer/PathIO.hpp;layer/PathIOMice.hpp;layer/PathIOKeyboard.hpp;layer/PathIOStdOut.hpp;layer/PathAlias.hpp;layer/PathIODeviceDiscovery.hpp;layer/PathIOPointerMixer.hpp")

install(TARGETS PathSpace
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

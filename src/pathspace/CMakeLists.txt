# if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
# endif()

include(FetchContent)

FetchContent_Declare(
    nlohmann
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann)
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

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(asio)

FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cpp_httplib)

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
    PathSpaceVisit.cpp
    tools/PathSpaceJsonExporter.cpp
    history/UndoHistoryUtils.cpp
    history/UndoJournalEntry.cpp
    history/UndoJournalState.cpp
    history/UndoJournalPersistence.cpp
    history/UndoSavefileCodec.cpp
    history/UndoableSpace.cpp
    history/UndoableSpaceHistory.cpp
    history/UndoableSpacePersistence.cpp
    history/UndoableSpaceSavefile.cpp
    history/UndoableSpaceTransactions.cpp
    task/Task.cpp
    task/TaskPool.cpp
    task/TaskStateAtomic.cpp
    task/Future.cpp
    type/SlidingBuffer.cpp
    type/TypeMetadataBootstrap.cpp
    type/TypeMetadataRegistry.cpp
    log/TaggedLogger.cpp
    PathSpace.cpp
    layer/PathSpaceTrellis.cpp
    layer/PathView.cpp
)

target_compile_options(PathSpace PUBLIC "-Wno-deprecated")

target_include_directories(PathSpace
    PUBLIC
    ${PROJECT_SOURCE_DIR}/src/pathspace
    ${PROJECT_SOURCE_DIR}/src/
    ${PROJECT_SOURCE_DIR}/include
    ${parallelhashmap_SOURCE_DIR}
    ${nlohmann_SOURCE_DIR}/include
    ${cereal_SOURCE_DIR}/include
    ${glaze_SOURCE_DIR}/include
    ${cista_SOURCE_DIR}/include
    ${alpaca_SOURCE_DIR}/include
    ${foonathan_memory_SOURCE_DIR}/include
    ${cpp_httplib_SOURCE_DIR}
    ${asio_SOURCE_DIR}/asio/include
)

target_compile_definitions(PathSpace PRIVATE ASIO_STANDALONE=1)

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

set(_pathspace_public_headers
    PathSpace.hpp
    layer/PathSpaceTrellis.hpp
)
if(PATHSPACE_ENABLE_EXTRA)
    list(APPEND _pathspace_public_headers
        layer/PathAlias.hpp
    )
endif()

list(JOIN _pathspace_public_headers ";" _pathspace_public_header_string)
set_target_properties(PathSpace PROPERTIES PUBLIC_HEADER "${_pathspace_public_header_string}")

install(TARGETS PathSpace
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

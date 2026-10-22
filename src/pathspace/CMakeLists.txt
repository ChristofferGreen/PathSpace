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

FetchContent_Declare(
    alpaca
    GIT_TAG 6bed0a0
    GIT_REPOSITORY https://github.com/p-ranav/alpaca.git
)
FetchContent_MakeAvailable(alpaca)




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
    layer/SnapshotCachedPathSpace.cpp
)

if(PATHSPACE_NEEDS_EXPECTED_SHIM)
    target_sources(PathSpace PRIVATE core/ExpectedShim.cpp)
    target_compile_definitions(PathSpace PRIVATE PATHSPACE_NEEDS_EXPECTED_SHIM=1)
endif()

target_compile_options(PathSpace PUBLIC "-Wno-deprecated")
pathspace_enable_warnings(PathSpace SHADOW)

if(PATHSPACE_CLANG_TIDY)
    find_program(PATHSPACE_CLANG_TIDY_EXE NAMES clang-tidy
        HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
    if(PATHSPACE_CLANG_TIDY_EXE)
        # Checks come from the repository's .clang-tidy file.
        set_target_properties(PathSpace PROPERTIES CXX_CLANG_TIDY "${PATHSPACE_CLANG_TIDY_EXE}")
    else()
        message(WARNING "PATHSPACE_CLANG_TIDY=ON but clang-tidy was not found; install LLVM (brew install llvm)")
    endif()
endif()

target_include_directories(PathSpace
    PUBLIC
    ${PROJECT_SOURCE_DIR}/src/pathspace
    ${PROJECT_SOURCE_DIR}/src/
    ${PROJECT_SOURCE_DIR}/include
)

# Third-party headers are SYSTEM so their warnings do not surface in our builds.
target_include_directories(PathSpace SYSTEM
    PUBLIC
    ${parallelhashmap_SOURCE_DIR}
    ${nlohmann_SOURCE_DIR}/include
    ${alpaca_SOURCE_DIR}/include
)

# Opt-in debug logging to keep test runs fast by default
option(PATHSPACE_ENABLE_LOG_DEBUG "Enable SP_LOG_DEBUG logging" OFF)
if(PATHSPACE_ENABLE_LOG_DEBUG)
    target_compile_definitions(PathSpace PRIVATE SP_LOG_DEBUG)
endif()

set(_pathspace_public_headers
    PathSpace.hpp
    layer/PathSpaceTrellis.hpp
    layer/SnapshotCachedPathSpace.hpp
)
list(APPEND _pathspace_public_headers
    layer/PathAlias.hpp
)

list(JOIN _pathspace_public_headers ";" _pathspace_public_header_string)
set_target_properties(PathSpace PROPERTIES PUBLIC_HEADER "${_pathspace_public_header_string}")

install(TARGETS PathSpace
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

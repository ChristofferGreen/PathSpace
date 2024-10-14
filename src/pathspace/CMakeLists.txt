#if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
#    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
#endif()

include(FetchContent)

FetchContent_Declare(
  nlohmann
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann)

FetchContent_Declare(
  ParallelHashmap
  GIT_REPOSITORY https://github.com/greg7mdp/parallel-hashmap.git
  GIT_TAG v1.3.11
)
FetchContent_MakeAvailable(ParallelHashmap)

FetchContent_Declare(
  glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG main
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

FetchContent_Declare(
    alpaca
    GIT_TAG 6bed0a0
    GIT_REPOSITORY https://github.com/p-ranav/alpaca.git
)
FetchContent_MakeAvailable(alpaca)

FetchContent_Declare(
  foonathan_memory
    GIT_TAG 016c9fb
    GIT_REPOSITORY https://github.com/foonathan/memory
)
FetchContent_MakeAvailable(foonathan_memory)

# Indicate that we only want to compile source files into a library
add_library(PathSpace
    path/ConcretePath.cpp
    path/ConcreteName.cpp
    path/ConcretePathIterator.cpp
    path/GlobPath.cpp
    path/GlobPathIterator.cpp
    path/GlobName.cpp
    path/Path.cpp
    taskpool/Task.cpp
    taskpool/TaskPool.cpp
    PathSpace.cpp
    PathSpaceLeaf.cpp
)

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

target_link_libraries(PathSpace
PRIVATE
    glaze::glaze
    foonathan_memory
)

set_target_properties(PathSpace PROPERTIES PUBLIC_HEADER "PathSpace.hpp")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(PathSpace PRIVATE SP_LOG_DEBUG)
endif()

install(TARGETS PathSpace
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

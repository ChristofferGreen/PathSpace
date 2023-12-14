if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

include(FetchContent)

FetchContent_Declare(
  ParallelHashmap
  GIT_REPOSITORY https://github.com/greg7mdp/parallel-hashmap.git
  GIT_TAG v1.3.11
)
FetchContent_MakeAvailable(ParallelHashmap)

# Indicate that we only want to compile source files into a library
add_library(PathSpace
    core/PathSpace.cpp
    core/GlobName.cpp
    core/GlobPath.cpp
    core/Path.cpp
    core/BasePath.cpp
)

target_include_directories(PathSpace
PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${parallelhashmap_SOURCE_DIR}
)

target_link_libraries(PathSpace
PRIVATE
)

set_target_properties(PathSpace PROPERTIES PUBLIC_HEADER "../include/PathSpace.hpp")

install(TARGETS PathSpace
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

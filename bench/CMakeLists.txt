add_executable(PathSpaceBench
    pathspace_bench.cpp
)

target_link_libraries(PathSpaceBench
    PRIVATE
    PathSpace
)

# Keep includes aligned with main library usage

target_include_directories(PathSpaceBench
    PRIVATE
    ${PROJECT_SOURCE_DIR}/src/pathspace
    ${PROJECT_SOURCE_DIR}/src
)

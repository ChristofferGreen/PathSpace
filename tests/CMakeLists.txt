add_executable(PathSpaceTest
  main.cpp
  catch.cpp

  typeinfo.cpp
)

target_include_directories(PathSpaceTest 
  PUBLIC
    ../src
    ../include
    ../ext
)

target_include_directories(PathSpaceTest 
  SYSTEM
  PUBLIC
  PRIVATE
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
target_compile_options(PathSpaceTest PRIVATE
  -Wno-deprecated-declarations
)
endif()

target_compile_definitions(PathSpaceTest
  PUBLIC
    CATCH_AMALGAMATED_CUSTOM_MAIN
)

target_link_libraries(PathSpaceTest
  PUBLIC
)


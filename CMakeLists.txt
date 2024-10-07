cmake_minimum_required(VERSION 3.15)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Define the project name and the language used
project(PathSpaceTests LANGUAGES CXX)

# Define the C++ standard to use
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Check if Clang is used, set it to use libc++
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  #add_compile_options(-D__cpp_concepts=202002L)
  #add_compile_options(-Wno-builtin-macro-redefined)
  add_compile_options(-stdlib=libc++)
  #add_link_options(-stdlib=libc++)
  #add_link_options(-lc++abi)
  #set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
endif()

enable_testing()
add_subdirectory(src/pathspace)
add_subdirectory(tests)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  add_custom_target(
    copy-compile-commands ALL
    ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
    DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
  )
endif()
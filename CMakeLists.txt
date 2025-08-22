cmake_minimum_required(VERSION 3.15)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Fix for GCC on macOS - set library paths BEFORE project() call
if(APPLE)
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib")
endif()

# Define the project name and the language used
project(PathSpaceTests LANGUAGES CXX OBJCXX)

# Additional GCC settings for macOS
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Add the include path to avoid header issues
    include_directories(SYSTEM /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include)
endif()

# Define the C++ standard to use
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add options for sanitizers
option(ENABLE_THREAD_SANITIZER "Enable Thread Sanitizer" OFF)
option(ENABLE_ADDRESS_SANITIZER "Enable Address Sanitizer" OFF)
option(ENABLE_UNDEFINED_SANITIZER "Enable Undefined Behavior Sanitizer" OFF)

# PathIO platform backends
option(ENABLE_PATHIO_MACOS "Enable macOS backend paths in unified providers" ON)

# Check if Clang is used, set it to use libc++
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    #add_compile_options(-D__cpp_concepts=202002L)
    #add_compile_options(-Wno-builtin-macro-redefined)
    add_compile_options(-stdlib=libc++)
    #add_link_options(-stdlib=libc++)
    #add_link_options(-lc++abi)
    #set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
endif()

# Apply sanitizer flags if enabled
if(ENABLE_THREAD_SANITIZER)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
elseif(ENABLE_ADDRESS_SANITIZER)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
elseif(ENABLE_UNDEFINED_SANITIZER)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

# Architectural features permanently enabled (no gating)


enable_testing()
add_subdirectory(src/pathspace)
add_subdirectory(tests)

# Platform-specific defines for unified PathIO backends
if(APPLE AND ENABLE_PATHIO_MACOS)
    add_compile_definitions(PATHIO_BACKEND_MACOS=1)
endif()

# Examples (optional)
option(BUILD_PATHSPACE_EXAMPLES "Build PathSpace examples" ON)
if(BUILD_PATHSPACE_EXAMPLES)
    add_executable(devices_example examples/devices_example.cpp)
    target_link_libraries(devices_example PRIVATE PathSpace)
    if(APPLE)
        target_sources(devices_example PRIVATE examples/macos/WindowEventPump.mm)
        if(CMAKE_OBJCXX_COMPILER_ID MATCHES "Clang")
            set_source_files_properties(examples/macos/WindowEventPump.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        endif()
        target_link_libraries(devices_example PRIVATE "-framework Cocoa")
    endif()
    if(APPLE AND ENABLE_PATHIO_MACOS)
        target_compile_definitions(devices_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()
endif()

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    add_custom_target(
        copy-compile-commands ALL
        ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_SOURCE_DIR}/compile_commands.json
        DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
    )
endif()

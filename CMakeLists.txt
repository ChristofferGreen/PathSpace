cmake_minimum_required(VERSION 3.15)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Fix for GCC on macOS - set library paths BEFORE project() call
if(APPLE)
    # Prefer Apple clang toolchain for C/C++/ObjC++ to support ObjC blocks and frameworks
    if(NOT CMAKE_C_COMPILER)
        set(CMAKE_C_COMPILER "/usr/bin/clang" CACHE FILEPATH "" FORCE)
    endif()
    if(NOT CMAKE_CXX_COMPILER)
        set(CMAKE_CXX_COMPILER "/usr/bin/clang++" CACHE FILEPATH "" FORCE)
    endif()
    if(NOT CMAKE_OBJCXX_COMPILER)
        set(CMAKE_OBJCXX_COMPILER "/usr/bin/clang++" CACHE FILEPATH "" FORCE)
    endif()

    set(CMAKE_EXE_LINKER_FLAGS_INIT "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib")
endif()

# Define the project name and the language used
# Keep platform independence by enabling ObjC++ only on Apple (for example window helper)
if(APPLE)
  project(PathSpaceTests LANGUAGES CXX OBJCXX)
else()
  project(PathSpaceTests LANGUAGES CXX)
endif()

# Additional GCC settings for macOS
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "On macOS, please use clang/clang++ (Apple Clang) instead of GCC to build ObjC++ sources and Apple frameworks.")
endif()

# Define the C++ standard to use
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UI/renderer feature gating
option(PATHSPACE_ENABLE_UI "Build UI (surfaces/renderers/windows)" OFF)
option(PATHSPACE_UI_SOFTWARE "Enable software surface backend" ON)
option(PATHSPACE_UI_METAL "Enable Metal surface/presenter (Apple)" ${APPLE})
option(PATHSPACE_ENABLE_APP "Build app-layer helpers" ON)
option(PATHSPACE_ENABLE_EXTRA "Build optional runtime extras (PathIO providers, discovery, etc.)" ON)

# Add options for sanitizers
option(ENABLE_THREAD_SANITIZER "Enable Thread Sanitizer" OFF)
option(ENABLE_ADDRESS_SANITIZER "Enable Address Sanitizer" OFF)
option(ENABLE_UNDEFINED_SANITIZER "Enable Undefined Behavior Sanitizer" OFF)

# PathIO platform backends
# Build macOS backends automatically when on Apple; no user toggle required.

# Check if Clang is used, set it to use libc++
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    #add_compile_options(-D__cpp_concepts=202002L)
    #add_compile_options(-Wno-builtin-macro-redefined)
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
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

add_executable(pathspace_history_inspect tools/history_inspect.cpp)
target_link_libraries(pathspace_history_inspect PRIVATE PathSpace)

add_executable(pathspace_history_savefile tools/history_savefile.cpp)
target_link_libraries(pathspace_history_savefile PRIVATE PathSpace)

add_executable(pathspace_history_cli_roundtrip tools/history_cli_roundtrip.cpp)
target_link_libraries(pathspace_history_cli_roundtrip PRIVATE PathSpace)

add_executable(pathspace_paint_screenshot_card tools/paint_screenshot_card.cpp)
target_link_libraries(pathspace_paint_screenshot_card PRIVATE PathSpace)

if(PATHSPACE_ENABLE_UI)
    add_executable(pathspace_hsat_inspect tools/hsat_inspect.cpp)
    target_link_libraries(pathspace_hsat_inspect PRIVATE PathSpace)
endif()

add_subdirectory(tests)

# Platform-specific defines for unified PathIO backends
if(APPLE)
    add_compile_definitions(PATHIO_BACKEND_MACOS=1)
endif()

# Examples (optional)
option(BUILD_PATHSPACE_EXAMPLES "Build PathSpace examples" OFF)
if(BUILD_PATHSPACE_EXAMPLES)
    add_executable(devices_example examples/devices_example.cpp)
    target_link_libraries(devices_example PRIVATE PathSpace)
    if(APPLE)
        if(CMAKE_OBJCXX_COMPILER_ID MATCHES "Clang")
            set_source_files_properties(
                examples/macos/GameControllerHaptics.mm
                PROPERTIES COMPILE_FLAGS "-fobjc-arc"
            )
        endif()
        target_link_libraries(devices_example PRIVATE "-framework Cocoa" "-framework GameController")
        target_link_libraries(devices_example PRIVATE
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(devices_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()

    add_executable(paint_example
        examples/paint_example.cpp)
    target_link_libraries(paint_example PRIVATE PathSpace)
    if(APPLE)
        if(CMAKE_OBJCXX_COMPILER_ID MATCHES "Clang")
            set_source_files_properties(
                examples/macos/GameControllerHaptics.mm
                PROPERTIES COMPILE_FLAGS "-fobjc-arc"
            )
        endif()
        target_link_libraries(paint_example PRIVATE
            "-framework Cocoa"
            "-framework GameController"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(paint_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()

    add_executable(html_replay_example examples/html_replay_example.cpp)
    target_link_libraries(html_replay_example PRIVATE PathSpace)
    if(APPLE)
        target_link_libraries(html_replay_example PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(html_replay_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()

    add_executable(widgets_example examples/widgets_example.cpp)
    target_link_libraries(widgets_example PRIVATE PathSpace)
    if(APPLE)
        target_link_libraries(widgets_example PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(widgets_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()

    add_executable(widgets_example_minimal examples/widgets_example_minimal.cpp)
    target_link_libraries(widgets_example_minimal PRIVATE PathSpace)
    if(APPLE)
        target_link_libraries(widgets_example_minimal PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(widgets_example_minimal PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()

    add_executable(declarative_theme_example examples/declarative_theme_example.cpp)
    target_link_libraries(declarative_theme_example PRIVATE PathSpace)
    if(APPLE)
        target_link_libraries(declarative_theme_example PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(declarative_theme_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()

    add_executable(declarative_hello_example examples/declarative_hello_example.cpp)
    target_link_libraries(declarative_hello_example PRIVATE PathSpace)
    if(APPLE)
        target_link_libraries(declarative_hello_example PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(declarative_hello_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()

    add_executable(pixel_noise_example examples/pixel_noise_example.cpp)
    target_link_libraries(pixel_noise_example PRIVATE PathSpace)
    if(APPLE)
        target_link_libraries(pixel_noise_example PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOSurface")
        target_compile_definitions(pixel_noise_example PRIVATE PATHIO_BACKEND_MACOS=1)
    endif()
endif()

# Documentation (Doxygen)
option(ENABLE_DOXYGEN "Enable generating Doxygen API documentation (target: docs)" OFF)
find_package(Doxygen QUIET)

option(BUILD_PATHSPACE_BENCHMARKS "Build PathSpace benchmark binaries" OFF)
if(BUILD_PATHSPACE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(ENABLE_DOXYGEN AND DOXYGEN_FOUND)
    set(DOXYGEN_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/docs/doxygen")
    file(MAKE_DIRECTORY "${DOXYGEN_OUTPUT_DIR}")

    set(DOXYFILE_PATH "${CMAKE_BINARY_DIR}/Doxyfile")

    file(WRITE "${DOXYFILE_PATH}"
"PROJECT_NAME           = PathSpace
PROJECT_NUMBER         = ${PROJECT_VERSION}
OUTPUT_DIRECTORY       = ${DOXYGEN_OUTPUT_DIR}
GENERATE_HTML          = YES
HTML_OUTPUT            = html
GENERATE_LATEX         = NO
QUIET                  = YES
WARN_AS_ERROR          = NO
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = NO
INPUT                  = ${CMAKE_SOURCE_DIR}/src/pathspace ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/docs
RECURSIVE              = YES
FILE_PATTERNS          = *.hpp *.h *.cpp *.md
USE_MDFILE_AS_MAINPAGE = ${CMAKE_SOURCE_DIR}/README.md
")

    add_custom_command(
        OUTPUT "${DOXYGEN_OUTPUT_DIR}/html/index.html"
        COMMAND "${DOXYGEN_EXECUTABLE}" "${DOXYFILE_PATH}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )

    add_custom_target(docs DEPENDS "${DOXYGEN_OUTPUT_DIR}/html/index.html")
elseif(ENABLE_DOXYGEN)
    message(WARNING "Doxygen requested but not found; 'docs' target will be unavailable.")
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

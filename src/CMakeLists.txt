if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

# Indicate that we only want to compile source files into a library
add_library(PathSpace
    PathSpace.cpp
)

target_include_directories(PathSpace PUBLIC ../include)

# Link any dependencies that your library needs
# For example: If you have a dependency on a threading library, it might look like this:
# find_package(Threads REQUIRED)
# target_link_libraries(PathSpace Threads::Threads)

# Link any other necessary libraries, such as if you use external libraries like ASIO or Catch2 for tests
# For example:
# target_link_libraries(PathSpace ASIO_lib Catch2_lib)

# If the library has public headers, specify them here
set_target_properties(PathSpace PROPERTIES PUBLIC_HEADER "../include/PathSpace.hpp")

# Specify any compile options or definitions, for example, if you need to compile with -fPIC
# target_compile_options(PathSpace PRIVATE -fPIC)

# Set up installation rules
install(TARGETS PathSpace
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

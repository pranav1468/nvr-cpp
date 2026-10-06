cmake_minimum_required(VERSION 3.16)
project(nvr_backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -pthread)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Core backend source files
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER SOURCES EXCLUDE REGEX "src/main.cpp$")

# Core static library
add_library(nvr_core STATIC ${SOURCES})
target_include_directories(nvr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Executable target
add_executable(nvr_server src/main.cpp)
target_link_libraries(nvr_server PRIVATE nvr_core pthread)

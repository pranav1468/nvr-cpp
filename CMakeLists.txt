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
set(SOURCES
    src/main.cpp
    src/ingress/camera_manager.cpp
    src/ingress/stream_session.cpp
    src/media/stream_broker.cpp
    src/media/video_decoder.cpp
    src/recording/segmenter.cpp
    src/recording/atomic_writer.cpp
    src/live/live_queue.cpp
    src/ai/ai_scheduler.cpp
    src/ai/motion_gate.cpp
    src/storage/database_manager.cpp
    src/storage/retention_manager.cpp
    src/dispatcher/command_dispatcher.cpp
    src/voice/voice_service.cpp
)

# Core static library
add_library(nvr_core STATIC ${SOURCES})
target_include_directories(nvr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Executable target
add_executable(nvr_server src/main.cpp)
target_link_libraries(nvr_server PRIVATE nvr_core pthread)

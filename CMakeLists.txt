cmake_minimum_required(VERSION 3.16)
project(nvr_backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler options
add_compile_options(-Wall -Wextra -Wpedantic -O2 -pthread)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/3rdparty
)

# Resolve SQLite3 library
find_package(SQLite3)
if (TARGET SQLite3::SQLite3)
    set(SQLITE3_LIB SQLite3::SQLite3)
else()
    find_library(SQLITE3_LIB NAMES sqlite3 libsqlite3.so)
    if (NOT SQLITE3_LIB)
        set(SQLITE3_LIB sqlite3)
    endif()
endif()

# Core backend source files
set(CORE_SOURCES
    src/common/logger.cpp
    src/common/deep_tracer.cpp
    src/common/time_utils.cpp
    src/common/thread_pool.cpp
    src/storage/database_manager.cpp
    src/storage/segment_index.cpp
    src/storage/retention_manager.cpp
    src/media/stream_broker.cpp
    src/media/buffer_allocator.cpp
    src/media/video_decoder.cpp
    src/media/video_scaler.cpp
    src/recording/atomic_writer.cpp
    src/recording/segmenter.cpp
    src/recording/recording_scheduler.cpp
    src/ingress/rtp_depacketizer.cpp
    src/ingress/stream_session.cpp
    src/ingress/camera_manager.cpp
    src/live/live_queue.cpp
    src/live/live_controller.cpp
    src/live/qml_video_bridge.cpp
    src/dispatcher/rbac_validator.cpp
    src/dispatcher/command_dispatcher.cpp
    src/api/rest_server.cpp
    src/api/fmp4_muxer.cpp
)

# Core static library
add_library(nvr_core STATIC ${CORE_SOURCES})
target_include_directories(nvr_core PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/3rdparty
)
target_link_libraries(nvr_core PUBLIC ${SQLITE3_LIB} pthread dl)

# Server executable target
add_executable(nvr_server src/main.cpp)
target_link_libraries(nvr_server PRIVATE nvr_core)

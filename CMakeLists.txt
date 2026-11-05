cmake_minimum_required(VERSION 3.16)
project(nvr_backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(TARGET_IMX95 "Apply NXP i.MX 95 Cortex-A55 architecture tuning flags" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -pthread -DNVR_ENABLE_DEEP_MONITORING)
add_link_options(-rdynamic)

if(TARGET_IMX95)
    add_compile_options(-mcpu=cortex-a55 -march=armv8.2-a+fp16 -O3)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
elseif(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

set(CMAKE_AUTOMOC ON)

set(CORE_SOURCES
    src/common/logger.cpp
    src/common/deep_tracer.cpp
    src/common/time_utils.cpp
    src/common/thread_pool.cpp
    src/common/config_loader.cpp
    src/common/crypto_utils.cpp
    src/database/database_manager.cpp
    src/storage/retention_manager.cpp
    src/media/stream_broker.cpp
    src/media/buffer_allocator.cpp
    src/media/video_decoder.cpp
    src/media/image_utils.cpp
    src/media/audio_transcoder.cpp
    src/recording/atomic_writer.cpp
    src/recording/segmenter.cpp
    src/recording/recording_scheduler.cpp
    src/ingress/rtp_depacketizer.cpp
    src/ingress/stream_session.cpp
    src/ingress/camera_manager.cpp
    src/ingress/onvif_client.cpp
    src/live/live_queue.cpp
    src/live/live_controller.cpp
    src/live/qml_video_bridge.cpp
    src/dispatcher/command_dispatcher.cpp
    src/api/rest_server.cpp
    src/api/fmp4_muxer.cpp
    src/ai/motion_gate.cpp
    src/playback/seek_controller.cpp
    src/playback/clip_exporter.cpp
    src/playback/playback_engine.cpp
)

add_library(nvr_core STATIC ${CORE_SOURCES})
target_include_directories(nvr_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/3rdparty
)
target_link_libraries(nvr_core PUBLIC pthread dl sqlite3 crypto)

add_executable(nvr_server src/main.cpp)
target_link_libraries(nvr_server PRIVATE nvr_core)

find_package(Qt6 COMPONENTS Core Gui Qml Quick QuickControls2 QUIET)
if(Qt6_FOUND)
    add_executable(nvr_gui
        src/ui_main.cpp
        src/ui/nvr_bridge.cpp
        include/nvr/ui/nvr_bridge.h
        include/nvr/live/live_bridge_controller.h
    )
    target_link_libraries(nvr_gui PRIVATE nvr_core Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    install(TARGETS nvr_gui DESTINATION bin OPTIONAL)
endif()

install(TARGETS nvr_server DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/nvr FILES_MATCHING PATTERN "*.json")
install(DIRECTORY web/ DESTINATION share/nvr/web)

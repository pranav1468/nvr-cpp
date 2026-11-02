CXX ?= g++
AR ?= ar

# Enable parallel builds automatically when invoked without explicit -j
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 4)
endif

NVR_INCLUDES = -I./include -I./include/3rdparty
NVR_DEFINES = -DNVR_ENABLE_DEEP_MONITORING

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread
override CXXFLAGS += -std=c++17 $(NVR_INCLUDES) $(NVR_DEFINES) -MMD -MP -pthread

LDFLAGS ?= -pthread -ldl -rdynamic
override LDFLAGS += -pthread -ldl -rdynamic

SRCS = src/common/logger.cpp \
       src/common/deep_tracer.cpp \
       src/common/time_utils.cpp \
       src/common/thread_pool.cpp \
       src/common/config_loader.cpp \
       src/storage/retention_manager.cpp \
       src/media/stream_broker.cpp \
       src/media/buffer_allocator.cpp \
       src/media/video_decoder.cpp \
       src/media/video_scaler.cpp \
       src/media/image_utils.cpp \
       src/recording/atomic_writer.cpp \
       src/recording/segmenter.cpp \
       src/recording/recording_scheduler.cpp \
       src/ingress/rtp_depacketizer.cpp \
       src/ingress/stream_session.cpp \
       src/ingress/camera_manager.cpp \
       src/ingress/onvif_client.cpp \
       src/live/live_queue.cpp \
       src/live/live_controller.cpp \
       src/live/qml_video_bridge.cpp \
       src/dispatcher/command_dispatcher.cpp \
       src/api/rest_server.cpp \
       src/api/fmp4_muxer.cpp \
       src/ai/motion_gate.cpp \
       src/playback/seek_controller.cpp \
       src/playback/clip_exporter.cpp \
       src/playback/playback_engine.cpp

OBJS = $(patsubst src/%.cpp, build/obj/%.o, $(SRCS))
CORE_LIB = build/libnvr_core.a
DEPS = $(OBJS:.o=.d) build/obj/main.d

QT_INCLUDES ?= -I/usr/include/x86_64-linux-gnu/qt6 -I/usr/include/x86_64-linux-gnu/qt6/QtCore -I/usr/include/x86_64-linux-gnu/qt6/QtGui -I/usr/include/x86_64-linux-gnu/qt6/QtQml -I/usr/include/x86_64-linux-gnu/qt6/QtQuick -I/usr/include/x86_64-linux-gnu/qt6/QtQuickControls2
QT_LIBS ?= -lQt6Quick -lQt6Qml -lQt6Gui -lQt6Core -lQt6QuickControls2

MOC ?= $(shell which /usr/lib/qt6/libexec/moc 2>/dev/null || which moc)

TARGET = bin/nvr_server
GUI_TARGET = bin/nvr_gui

all: $(TARGET) $(GUI_TARGET)

$(CORE_LIB): $(OBJS) | build
	$(AR) rcs $@ $(OBJS)

$(TARGET): build/obj/main.o $(CORE_LIB) | bin
	$(CXX) $(CXXFLAGS) -o $@ build/obj/main.o $(CORE_LIB) $(LDFLAGS)

$(GUI_TARGET): build/obj/ui_main.o build/obj/ui/nvr_bridge.o build/obj/moc_nvr_bridge.o build/obj/moc_live_bridge_controller.o $(CORE_LIB) | bin
	$(CXX) $(CXXFLAGS) -o $@ build/obj/ui_main.o build/obj/ui/nvr_bridge.o build/obj/moc_nvr_bridge.o build/obj/moc_live_bridge_controller.o $(CORE_LIB) $(QT_LIBS) $(LDFLAGS)

build/moc_nvr_bridge.cpp: include/nvr/ui/nvr_bridge.h | build
	$(MOC) $(NVR_INCLUDES) $(QT_INCLUDES) $< -o $@

build/obj/moc_nvr_bridge.o: build/moc_nvr_bridge.cpp | build/obj
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_INCLUDES) -c $< -o $@

build/obj/ui/nvr_bridge.o: src/ui/nvr_bridge.cpp | build/obj
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_INCLUDES) -c $< -o $@

build/moc_live_bridge_controller.cpp: include/nvr/live/live_bridge_controller.h | build
	$(MOC) $(NVR_INCLUDES) $(QT_INCLUDES) $< -o $@

build/obj/moc_live_bridge_controller.o: build/moc_live_bridge_controller.cpp | build/obj
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_INCLUDES) -c $< -o $@

build/obj/ui_main.o: src/ui_main.cpp | build/obj
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_INCLUDES) -c $< -o $@

build/obj/%.o: src/%.cpp | build/obj
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

bin build build/obj:
	mkdir -p $@

clean:
	rm -rf build
	rm -f bin/nvr_server bin/nvr_gui

IMX_SDK_ENV ?= /home/mantra/Desktop/NVR_1/imx-sdk/environment-setup-armv8a-poky-linux

cross:
	@if [ ! -f $(IMX_SDK_ENV) ]; then \
		echo "Error: i.MX SDK environment script not found at $(IMX_SDK_ENV)"; \
		exit 1; \
	fi
	@echo "=== Cross-compiling NVR Engine for i.MX (ARM64 / AArch64) ==="
	@bash -c "source $(IMX_SDK_ENV) && $(MAKE) clean && $(MAKE) all"
	@mkdir -p bin/arm64
	@cp bin/nvr_server bin/arm64/
	@echo "=== Verifying ARM64 Target Binaries ==="
	@file bin/arm64/nvr_server

web:
	@echo "=== Web Console Ready (Static Lightweight UI) ==="
	@test -f web/index.html && echo "web/index.html verified."

web-clean:
	@echo "=== Web Console Clean ==="

-include $(DEPS)

.PHONY: all clean cross web web-clean

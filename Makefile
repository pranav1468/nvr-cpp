CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread -I./include -I./include/3rdparty -MMD -MP -DNVR_ENABLE_DEEP_MONITORING
LDFLAGS ?= -pthread -ldl -rdynamic

SQLITE_LIB ?= $(shell if $(CXX) -shared -x c /dev/null -o /dev/null -lsqlite3 >/dev/null 2>&1; then echo "-lsqlite3"; \
                      elif [ -f /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 ]; then echo "/usr/lib/x86_64-linux-gnu/libsqlite3.so.0"; \
                      elif [ -f /usr/lib/aarch64-linux-gnu/libsqlite3.so.0 ]; then echo "/usr/lib/aarch64-linux-gnu/libsqlite3.so.0"; \
                      else echo "-lsqlite3"; fi)

SRCS = src/common/logger.cpp \
       src/common/deep_tracer.cpp \
       src/common/time_utils.cpp \
       src/common/thread_pool.cpp \
       src/storage/database_manager.cpp \
       src/storage/segment_index.cpp \
       src/storage/retention_manager.cpp \
       src/media/stream_broker.cpp \
       src/media/buffer_allocator.cpp \
       src/media/video_decoder.cpp \
       src/media/video_scaler.cpp \
       src/recording/atomic_writer.cpp \
       src/recording/segmenter.cpp \
       src/recording/recording_scheduler.cpp \
       src/ingress/rtp_depacketizer.cpp \
       src/ingress/stream_session.cpp \
       src/ingress/camera_manager.cpp \
       src/live/live_queue.cpp \
       src/live/live_controller.cpp \
       src/live/qml_video_bridge.cpp \
       src/dispatcher/rbac_validator.cpp \
       src/dispatcher/command_dispatcher.cpp \
       src/api/rest_server.cpp \
       src/api/fmp4_muxer.cpp

OBJS = $(patsubst src/%.cpp, build/obj/%.o, $(SRCS))
DEPS = $(OBJS:.o=.d) build/obj/main.d

TARGET = bin/nvr_server
TEST_DISPATCHER = bin/test_command_dispatcher
TEST_REST = bin/test_rest_server

all: $(TARGET) $(TEST_DISPATCHER) $(TEST_REST)

$(TARGET): $(OBJS) build/obj/main.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE_LIB) $(LDFLAGS)

$(TEST_DISPATCHER): $(OBJS) tests/test_command_dispatcher.cpp | bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE_LIB) $(LDFLAGS)

$(TEST_REST): $(OBJS) tests/test_rest_server.cpp | bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE_LIB) $(LDFLAGS)

build/obj/%.o: src/%.cpp | build/obj
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

bin build/obj:
	mkdir -p $@

clean:
	rm -rf build bin recordings test_recordings nvr_metadata.db*

-include $(wildcard build/obj/*.d build/obj/*/*.d)

.PHONY: all clean

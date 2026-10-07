CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread -I./include -I./include/3rdparty
LDFLAGS ?= -pthread -ldl

SQLITE_LIB := $(shell if [ -f /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 ]; then echo "/usr/lib/x86_64-linux-gnu/libsqlite3.so.0"; else echo "-lsqlite3"; fi)

SRCS = src/common/logger.cpp \
       src/common/time_utils.cpp \
       src/common/thread_pool.cpp \
       src/storage/database_manager.cpp \
       src/storage/segment_index.cpp \
       src/storage/retention_manager.cpp \
       src/media/stream_broker.cpp \
       src/recording/atomic_writer.cpp \
       src/recording/segmenter.cpp \
       src/recording/recording_scheduler.cpp \
       src/ingress/rtp_depacketizer.cpp \
       src/ingress/stream_session.cpp \
       src/ingress/camera_manager.cpp

OBJS = $(patsubst src/%.cpp, build/obj/%.o, $(SRCS))

TARGET = bin/nvr_server
TEST_TARGET = bin/test_recording_pipeline
BATTLE_TARGET = bin/battle_test_suite

all: $(TARGET) $(TEST_TARGET) $(BATTLE_TARGET)

$(TARGET): $(OBJS) build/obj/main.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE_LIB) $(LDFLAGS)

$(TEST_TARGET): $(OBJS) build/obj/tests/test_recording_pipeline.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE_LIB) $(LDFLAGS)

$(BATTLE_TARGET): $(OBJS) build/obj/tests/battle_test_suite.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE_LIB) $(LDFLAGS)

build/obj/%.o: src/%.cpp | build/obj
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/obj/tests/%.o: tests/%.cpp | build/obj/tests
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

bin build/obj build/obj/tests:
	mkdir -p $@

clean:
	rm -rf build bin recordings nvr_metadata.db*

.PHONY: all clean

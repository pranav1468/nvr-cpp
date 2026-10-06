# NVR Backend Engine (C++)

High-performance, modular Network Video Recorder (NVR) backend engine designed for multi-channel IP camera streaming, hardware-accelerated video processing, and low-latency live display.

## Architecture Overview

The system uses a decoupled multi-cadence pipeline architecture directly mapping to the 18 core architectural subsystems:

- **Ingress (`include/nvr/ingress/`, `src/ingress/`):** Manages physical camera connections, ONVIF discovery, and network sessions. Maintains a single RTSP/RTP ingress session per camera.
- **Media (`include/nvr/media/`, `src/media/`):** In-memory stream broker with fan-out tees for compressed H.264/H.265 bitstreams, V4L2 VPU decoder abstraction (`IVideoDecoder`), and DMA-BUF buffer pool allocator.
- **Recording (`include/nvr/recording/`, `src/recording/`):** Stream-copy recording into container segments. Uses two-phase atomic file commits (`.tmp` to `.mp4`) and GOP-aligned segmenting.
- **Live (`include/nvr/live/`, `src/live/`):** Freshness-first bounded queues with head-drop policy to prevent latency buildup in live grid view.
- **Playback & Export (`include/nvr/playback/`, `src/playback/`):** Video segment file reader, hardware VPU demuxer, seek rate controller (1x, 2x, 4x, reverse), and stream-copy clip exporter.
- **AI Vision (`include/nvr/ai/`, `src/ai/`):** Bounded frame mailbox, motion gating, dynamic capacity scheduler ($\rho < 1.0$), NPU INT8 model interface, ByteTrack object tracking, and rule engine.
- **Storage & Metadata (`include/nvr/storage/`, `src/storage/`):** SQLite WAL database indexing segments, events, and tracks. Enforces circular FIFO disk retention.
- **Health & Resources (`include/nvr/health/`, `src/health/`):** Telemetry collector for CPU, RAM, VPU, NPU, and thermals. Closed-loop adaptive load shedder.
- **Unified Command Dispatcher (`include/nvr/dispatcher/`, `src/dispatcher/`):** Centralized command validation and RBAC enforcement for UI, API, and voice inputs.
- **API Server (`include/nvr/api/`, `src/api/`):** Embedded REST server and real-time WebSocket event/telemetry gateway.
- **Notifications (`include/nvr/notifications/`, `src/notifications/`):** Multi-target notification manager supporting SMTP email alerts, buzzer/GPIO alarms, and snapshot uploads.
- **Voice Control (`include/nvr/voice/`, `src/voice/`):** Isolated audio capture (ALSA 16 kHz Mono), VAD, local quantized ASR, and finite-state grammar intent parser.

## Project Directory Structure

```
nvr-cpp/
├── CMakeLists.txt                      # Root build configuration
├── README.md                           # Architecture overview
├── config/
│   ├── nvr_config.json                 # Default runtime settings
│   └── camera_profiles.json            # Camera profile definitions
├── include/nvr/                        # Modular public interfaces
│   ├── common/                         # Core types, time utilities, thread pool, logger
│   ├── ingress/                        # Camera manager, stream session, ONVIF client, RTP depay
│   ├── media/                          # Stream broker, video decoder, scaler, buffer allocator
│   ├── recording/                      # Stream segmenter, atomic writer, recording scheduler
│   ├── live/                           # Live queue, display backend interface
│   ├── playback/                       # Playback engine, seek controller, clip exporter
│   ├── ai/                             # Motion gate, scheduler, NPU backend, tracker, rules
│   ├── storage/                        # SQLite manager, segment index, event index, retention
│   ├── health/                         # Metrics collector, adaptive load shedder
│   ├── dispatcher/                     # Command dispatcher, command types, RBAC validator
│   ├── api/                            # REST server, WebSocket server
│   ├── notifications/                  # Notification manager, email notifier, GPIO alarms
│   └── voice/                          # Audio capture, VAD, ASR engine, grammar parser
└── src/                                # Engine implementations
    ├── main.cpp                        # NVR core entry point
    ├── common/
    ├── ingress/
    ├── media/
    ├── recording/
    ├── live/
    ├── playback/
    ├── ai/
    ├── storage/
    ├── health/
    ├── dispatcher/
    ├── api/
    ├── notifications/
    └── voice/
```

## Build Instructions

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

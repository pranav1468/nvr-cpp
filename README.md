# NVR Backend Engine (C++)

High-performance, modular Network Video Recorder (NVR) backend engine designed for multi-channel IP camera streaming, hardware-accelerated video processing, and low-latency live display.

## Architecture Overview

The system uses a decoupled multi-cadence pipeline architecture where camera ingestion, recording, live view, and analytics operate independently:

- **Ingress (`include/nvr/ingress/`, `src/ingress/`):** Manages physical camera connections and network sessions. Maintains a single RTSP/RTP ingress session per camera.
- **Media (`include/nvr/media/`, `src/media/`):** In-memory stream broker with fan-out tees for compressed H.264/H.265 bitstreams and hardware decoder abstractions.
- **Recording (`include/nvr/recording/`, `src/recording/`):** Stream-copy recording into container segments. Uses two-phase atomic file commits (`.tmp` to `.mp4`).
- **Live (`include/nvr/live/`, `src/live/`):** Freshness-first bounded queues with head-drop policy to prevent latency buildup in live grid view.
- **AI Analytics (`include/nvr/ai/`, `src/ai/`):** Bounded frame mailbox, motion gating, and dynamic capacity scheduler for hardware inference accelerators.
- **Storage & Metadata (`include/nvr/storage/`, `src/storage/`):** SQLite index for segments, events, and circular retention management.
- **Command Dispatcher (`include/nvr/dispatcher/`, `src/dispatcher/`):** Centralized command validation and access control for UI, API, and voice inputs.
- **Voice (`include/nvr/voice/`, `src/voice/`):** Isolated audio capture and grammar-based intent parser.

## Project Directory Structure

```
nvr-cpp/
├── .github/
│   └── pull_request_template.md        # Standard PR template
├── CMakeLists.txt                      # Root build configuration
├── config/
│   └── nvr_config.json                 # Default runtime settings
├── include/
│   └── nvr/                            # Modular public interfaces
│       ├── common/                     # Core types, clock, and logging
│       ├── ingress/                    # Camera connection & session management
│       ├── media/                      # Stream broker & decoder abstractions
│       ├── recording/                  # Segmenting & atomic storage writes
│       ├── live/                       # Live queue & display buffering
│       ├── ai/                         # Motion gating, scheduler & inference interfaces
│       ├── storage/                    # Database & retention interfaces
│       ├── dispatcher/                 # Unified command dispatcher
│       └── voice/                      # Audio capture & grammar parser
└── src/                                # Engine implementations
    ├── main.cpp                        # NVR core entry point
    ├── ingress/
    ├── media/
    ├── recording/
    ├── live/
    ├── ai/
    ├── storage/
    ├── dispatcher/
    └── voice/
```

## Build Instructions

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

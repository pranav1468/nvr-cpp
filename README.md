# NVR Core Backend Engine (C++)

Welcome to the backend engine for the Network Video Recorder (NVR). This C++ service runs 24/7 in the background on the device..

---

## What Does the Backend Do? (The Big Picture)

The backend has four main jobs:
1. **Connects to IP Cameras:** Pulls video streams from up to 8 cameras over the local network using RTSP.
2. **Records Without Lag:** Saves video straight to the hard drive in compressed format (zero re-encoding), so CPU usage stays below 50%.
3. **Hardware Decoding & AI:** Decodes low-resolution video using hardware chips to show live video on screen and runs AI detection (person/vehicle) on the hardware NPU.
4. **Responds to the Frontend:** Listens for commands from the UI (like "show camera 1" or "find recordings from 2 PM") and sends back video and data.

```
+-----------------------------------------------------------------------------------+
|                                  NVR BACKEND ENGINE                               |
|                                                                                   |
|  [ IP Cameras ] ---> ( Ingress: RTSP / RTP )                                      |
|                                |                                                  |
|                                v                                                  |
|                     ( Media: Stream Broker )                                      |
|                               /        \                                          |
|                              /          \                                         |
|      [MAIN Stream: Compressed]          [SUB Stream: Low-Res]                     |
|                 |                                   |                             |
|                 v                                   v                             |
|       ( Recording Engine )                 ( Hardware Decoder )                   |
|                 |                               /          \                      |
|                 v                              v            v                     |
|       [ Hard Drive / SQLite ]            [ Live Display ]  ( AI Engine: NPU )     |
|                                                 |                  |              |
|                                                 |                  v              |
|                                                 |           [ Alerts / Rules ]    |
|                                                 \                  /              |
|                                                  v                v               |
|                                                [ Command Dispatcher ]             |
|                                                          |                        |
+----------------------------------------------------------|------------------------+
                                                           |
                                                           v
                                            [ Frontend UI (Qt 6 / QML) ]
```

---

## Directory Structure Overview

```
nvr-cpp/
├── CMakeLists.txt              # Root build configuration
├── README.md                   # This backend guide
├── config/
│   └── nvr_config.json         # Default system configuration settings
├── include/nvr/                # Public header files (.h)
│   ├── ai/                     # AI detection, scheduling, and rules
│   ├── api/                    # HTTP REST and WebSocket remote servers
│   ├── common/                 # Global types, logging, and utilities
│   ├── dispatcher/             # Central command router and permissions
│   ├── health/                 # CPU, RAM, and temperature monitoring
│   ├── ingress/                # Camera RTSP connection and ONVIF discovery
│   ├── live/                   # Real-time live video frame queues
│   ├── media/                  # Video splitting and hardware decoding
│   ├── notifications/          # Email and hardware buzzer alerts
│   ├── playback/               # Recorded video player and clip export
│   ├── recording/              # Safe file segmentation and writing
│   ├── storage/                # SQLite database and disk space manager
│   └── voice/                  # Microphone input and voice commands
└── src/                        # C++ source implementations (.cpp)
    ├── ai/
    ├── api/
    ├── common/
    ├── dispatcher/
    ├── health/
    ├── ingress/
    ├── live/
    ├── media/
    ├── notifications/
    ├── playback/
    ├── recording/
    ├── storage/
    ├── voice/
    └── main.cpp                # Application entry point
```

---

## Simple File-by-File Guide: Which File Is For What

Every subsystem has header files (`include/nvr/...`) declaring what functions exist, and source files (`src/...`) implementing how they work.

### 1. Ingress: Connecting to Cameras
Connects to IP cameras over the local network and unpacks video packets.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/ingress/camera_manager.h`<br>`src/ingress/camera_manager.cpp` | Manages the list of all connected cameras. | Keeps track of who is online, detects if a camera cable is unplugged, and automatically reconnects when it comes back. |
| `include/nvr/ingress/stream_session.h`<br>`src/ingress/stream_session.cpp` | Handles the single network RTSP connection per camera. | Pulls both the 1080p MAIN stream and the 360p SUB stream over **one** network connection so the camera is never overloaded. |
| `include/nvr/ingress/onvif_client.h`<br>`src/ingress/onvif_client.cpp` | Scans the local network for cameras using ONVIF. | Allows one-click camera discovery without typing IP addresses manually, and sends Pan-Tilt-Zoom (PTZ) motor commands. |
| `include/nvr/ingress/rtp_depacketizer.h`<br>`src/ingress/rtp_depacketizer.cpp` | Unpacks raw network packets into clean video frames. | Camera video arrives chopped up into small network packets (RTP). This reassembles them into clean H.264/H.265 frames. |

---

### 2. Media: Video Splitting & Hardware Decoding
Takes incoming video, splits it into multiple destinations, and decodes it without burning CPU.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/media/stream_broker.h`<br>`src/media/stream_broker.cpp` | The central video splitter in memory. | Takes a single incoming camera stream and delivers copies to recording, live display, and AI simultaneously without extra network traffic. |
| `include/nvr/media/video_decoder.h`<br>`src/media/video_decoder.cpp` | Hardware video decoder chip driver. | Uses the board's hardware chip (V4L2) to decode compressed video into picture frames for the monitor without making the CPU hot. |
| `include/nvr/media/video_scaler.h`<br>`src/media/video_scaler.cpp` | Hardware image resizer and format converter. | Shrinks or stretches video frames to fit different screen grid sizes ($1\times 1$, $2\times 2$, $2\times 4$). |
| `include/nvr/media/buffer_allocator.h`<br>`src/media/buffer_allocator.cpp` | Video memory manager (DMA-BUF). | Lets the camera, decoder, screen, and AI share the exact same video memory with zero copying, saving memory bandwidth. |

---

### 3. Recording: Saving Video to Disk
Saves video files cleanly without ever converting or re-compressing them.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/recording/segmenter.h`<br>`src/recording/segmenter.cpp` | Slices video into 1-minute `.mp4` chunks. | Cuts the compressed stream cleanly on keyframes (I-frames) so each 1-minute video file can be played independently. |
| `include/nvr/recording/atomic_writer.h`<br>`src/recording/atomic_writer.cpp` | Crash-proof file writer. | Writes to a temporary `.tmp` file, flushes to disk, then renames to `.mp4`. If power is suddenly cut, video files are never corrupted. |
| `include/nvr/recording/recording_scheduler.h`<br>`src/recording/recording_scheduler.cpp` | Recording timetable manager. | Checks configuration to decide when each camera should record (continuous 24/7 vs. only when motion or AI triggers). |

---

### 4. Live: Real-Time Grid Display
Feeds decoded video frames to the screen smoothly and without latency.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/live/live_queue.h`<br>`src/live/live_queue.cpp` | Real-time video frame queue. | Holds frames ready for display. If the screen is slow, it drops older frames ("head-drop") so live video **never falls behind real time**. |

---

### 5. Playback: Watching Past Recordings
Searches and plays back video files that were previously recorded to disk.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/playback/playback_engine.h`<br>`src/playback/playback_engine.cpp` | Past video file reader. | Opens saved `.mp4` files from disk, feeds them to the hardware decoder, and sends frames to the screen. |
| `include/nvr/playback/seek_controller.h`<br>`src/playback/seek_controller.cpp` | Player control manager. | Handles scrubbing the timeline bar, pause, slow motion, frame-by-step, and fast forward ($2\times, 4\times, 8\times$, reverse). |
| `include/nvr/playback/clip_exporter.h`<br>`src/playback/clip_exporter.cpp` | Video export tool. | Cuts a chosen time range (e.g., 2:00 PM to 2:15 PM) and copies it directly to a USB stick without re-encoding. |

---

### 6. AI: Smart Detection on Hardware NPU
Finds people, cars, and security violations using the dedicated AI accelerator chip.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/ai/motion_gate.h`<br>`src/ai/motion_gate.cpp` | Ultra-fast scene motion checker. | Quickly compares consecutive video frames. If nothing is moving in the room, it skips AI completely to save power. |
| `include/nvr/ai/ai_scheduler.h`<br>`src/ai/ai_scheduler.cpp` | AI traffic cop. | Controls how often each camera sends frames to the AI chip so the hardware NPU never gets overloaded. |
| `include/nvr/ai/inference_backend.h`<br>`src/ai/inference_backend.cpp` | Hardware NPU neural network driver. | Runs quantized INT8 object detection models directly on the dedicated 8 eTOPS NPU chip. |
| `include/nvr/ai/object_tracker.h`<br>`src/ai/object_tracker.cpp` | Object tracker across video frames. | Follows a person or car as they move across the camera view, giving them a single ID so one person doesn't trigger 50 alerts. |
| `include/nvr/ai/rule_engine.h`<br>`src/ai/rule_engine.cpp` | Security rule checker. | Checks if a tracked object crossed a virtual tripwire line, entered a forbidden zone, or loitered too long. |

---

### 7. Storage: Database & Disk Management
Keeps an index of all videos and manages hard drive space.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/storage/database_manager.h`<br>`src/storage/database_manager.cpp` | SQLite database connection. | Uses Write-Ahead Logging (WAL) mode for fast, crash-safe saving and querying of recording and event logs. |
| `include/nvr/storage/segment_index.h`<br>`src/storage/segment_index.cpp` | Recording index table. | Stores the start time, end time, and file path of every recorded `.mp4` file so the timeline can find them instantly. |
| `include/nvr/storage/event_index.h`<br>`src/storage/event_index.cpp` | AI event search index. | Stores every detection alert (camera, timestamp, object type, bounding box coordinates) for quick searching. |
| `include/nvr/storage/retention_manager.h`<br>`src/storage/retention_manager.cpp` | Disk space cleaner. | Monitors free hard drive space. When the drive is 90% full, deletes the oldest non-event video files so the NVR never stops recording. |

---

### 8. Health: System Protection & Load Control
Prevents the device from overheating or freezing under heavy load.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/health/metrics_collector.h`<br>`src/health/metrics_collector.cpp` | System vitals monitor. | Reads live CPU usage, RAM usage, storage space, and chip temperatures from Linux kernel files. |
| `include/nvr/health/load_shedder.h`<br>`src/health/load_shedder.cpp` | Emergency safety protector. | If the CPU exceeds 50% or the chip gets too hot, automatically drops the AI detection rate to keep recording running smoothly. |

---

### 9. Dispatcher: Central Command Router
The central command desk of the NVR. All actions go through here.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/dispatcher/command_dispatcher.h`<br>`src/dispatcher/command_dispatcher.cpp` | Central command router. | The single place where all requests (from UI, Web API, or Voice) are received, validated, and executed. |
| `include/nvr/dispatcher/commands.h` | List of all allowed commands. | Defines standard JSON command formats (e.g. `START_RECORDING`, `SET_LAYOUT`, `ADD_CAMERA`, `PTZ_MOVE`). |
| `include/nvr/dispatcher/rbac_validator.h`<br>`src/dispatcher/rbac_validator.cpp` | Security permission checker. | Checks user login credentials and roles (Admin vs. Viewer) before allowing any command to run. |

---

### 10. API: Remote Access Gateways
Allows phones, web browsers, and external software to communicate with the NVR.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/api/rest_server.h`<br>`src/api/rest_server.cpp` | Embedded web server (HTTP REST). | Lets remote web browsers view camera lists, check system status, and change settings. |
| `include/nvr/api/websocket_server.h`<br>`src/api/websocket_server.cpp` | Real-time WebSocket server. | Pushes live AI alert notifications immediately to connected web browsers or mobile apps without polling. |

---

### 11. Notifications: Alarms & Alerts
Alerts security personnel when an event triggers.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/notifications/notification_manager.h`<br>`src/notifications/notification_manager.cpp` | Alert coordinator. | Receives security events from the rule engine and sends them out to email, buzzer, and UI toast popups. |
| `include/nvr/notifications/email_notifier.h`<br>`src/notifications/email_notifier.cpp` | Email sender (SMTP). | Sends an email with an attached picture snapshot whenever an alert triggers. |
| `include/nvr/notifications/gpio_alarm.h`<br>`src/notifications/gpio_alarm.cpp` | Physical hardware alarm controller. | Turns on an external physical buzzer, siren, or relay warning light via GPIO pins on the hardware board. |

---

### 12. Voice: On-Device Voice Commands
Allows hands-free voice control (e.g., *"show camera 3"*).

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/voice/audio_capture.h`<br>`src/voice/audio_capture.cpp` | Microphone sound capture. | Records live audio from the connected microphone using standard Linux ALSA (16 kHz mono). |
| `include/nvr/voice/vad_detector.h`<br>`src/voice/vad_detector.cpp` | Voice activity detector. | Detects when someone begins speaking so the system does not waste CPU analyzing room silence. |
| `include/nvr/voice/asr_engine.h`<br>`src/voice/asr_engine.cpp` | Speech-to-text converter. | Converts spoken words into text completely locally on the device (no internet connection needed). |
| `include/nvr/voice/grammar_parser.h`<br>`src/voice/grammar_parser.cpp` | Command grammar matcher. | Matches recognized words against allowed phrases (e.g., *"switch to grid view"*, *"show camera 2"*). |
| `include/nvr/voice/voice_service.h`<br>`src/voice/voice_service.cpp` | Voice pipeline coordinator. | Glues audio capture, speech detection, text conversion, and command execution together. |

---

### 13. Common: Shared Tools & Utilities
Fundamental helper classes used across all backend modules.

| File Path | What It Does (In Plain English) | Why We Need It |
| :--- | :--- | :--- |
| `include/nvr/common/types.h` | Common data structures. | Defines shared structures like `CameraId`, `FrameBuffer`, `VideoCodec`, and `BoundingBox`. |
| `include/nvr/common/time_utils.h` | Safe clock helpers. | Uses monotonic clock for timers (never skips if clock resets) and wall clock for video timestamps. |
| `include/nvr/common/thread_pool.h`<br>`src/common/thread_pool.cpp` | Background worker threads. | Runs background tasks (like writing to disk or sending emails) without slowing down video streams. |
| `include/nvr/common/logger.h`<br>`src/common/logger.cpp` | Fast, thread-safe logger. | Prints timestamped debug, info, and error messages to the terminal and log files. |

---

### 14. Entry Point & Config
| File Path | What It Does (In Plain English) |
| :--- | :--- |
| `src/main.cpp` | Starts the backend service: loads `config/nvr_config.json`, starts camera connections, launches background threads, and listens for commands. |
| `config/nvr_config.json` | Stores initial settings: default camera RTSP URLs, recording directory paths, database path, and network ports. |
| `CMakeLists.txt` | Build instructions for CMake telling the compiler how to build the executable and link libraries. |

---

## How Video Flows Through the Backend (4 Main Pipelines)

### 1. The Recording Pipeline (Never Decodes Video)
```
Camera RTSP Feed
      │
      ▼
stream_session.cpp (pulls network stream)
      │
      ▼
stream_broker.cpp (routes MAIN 1080p stream)
      │
      ▼
segmenter.cpp (splits stream into 1-minute chunks on I-frames)
      │
      ▼
atomic_writer.cpp (writes video.tmp -> flushes -> renames to video.mp4)
      │
      ▼
segment_index.cpp (logs file path and timestamps into SQLite database)
```

### 2. The Live View Pipeline (Zero-Lag Display)
```
Camera RTSP Feed
      │
      ▼
stream_session.cpp (pulls SUB 360p stream)
      │
      ▼
stream_broker.cpp (routes SUB stream to decoder)
      │
      ▼
video_decoder.cpp (hardware VPU chip decodes to raw picture)
      │
      ▼
live_queue.cpp (holds frames; drops old ones if screen is busy)
      │
      ▼
Sent to UI Video Surface -> Displayed on screen
```

### 3. The AI Detection Pipeline (Runs on NPU)
```
Decoded Video Frame
      │
      ▼
motion_gate.cpp (checks if any pixels moved; if still, skips AI)
      │
      ▼
ai_scheduler.cpp (checks NPU capacity headroom)
      │
      ▼
inference_backend.cpp (runs INT8 model on 8 eTOPS NPU chip)
      │
      ▼
object_tracker.cpp (tracks person/car across frames with persistent ID)
      │
      ▼
rule_engine.cpp (checks tripwires and restricted zones)
      │
      ▼
event_index.cpp (saves event to database)
      │
      ├──> websocket_server.cpp (pushes instant alert to UI and phone)
      └──> notification_manager.cpp (sends email / triggers buzzer)
```

### 4. The Playback Pipeline (Past Footage)
```
User clicks timeline in UI
      │
      ▼
command_dispatcher.cpp (receives SEEK command)
      │
      ▼
playback_engine.cpp (reads .mp4 file from disk)
      │
      ▼
seek_controller.cpp (jumps to exact keyframe)
      │
      ▼
video_decoder.cpp (hardware decodes frames)
      │
      ▼
Sent to UI Video Surface -> Screen shows recorded video
```

---

## How to Build and Run the Backend

```bash
# 1. Create a build directory
mkdir build
cd build

# 2. Configure with CMake
cmake ..

# 3. Compile
make -j$(nproc)

# 4. Start the backend service
./nvr_server
```

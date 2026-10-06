# NVR Core Backend Engine (C++)

This is the high-performance backend server for the NVR system. It handles camera connections, compressed video recording, hardware decoding, AI detection, storage retention, and commands from the UI.

---

## Simple File-by-File Guide: What Each File Does

### 1. Ingress: Camera Connections (`include/nvr/ingress/` & `src/ingress/`)
* **`camera_manager.h / .cpp`**: Manages all connected cameras. Handles adding cameras, checking connection health, and automatically reconnecting when a camera drops off the network.
* **`stream_session.h / .cpp`**: Connects to the camera's RTSP video feed. Keeps exactly **1 network connection per camera**, handling both the high-res MAIN stream and low-res SUB stream.
* **`onvif_client.h / .cpp`**: Discovers IP cameras on your local network automatically using ONVIF, reads camera capabilities, and sends Pan-Tilt-Zoom (PTZ) commands.
* **`rtp_depacketizer.h / .cpp`**: Unpacks raw network RTP packets arriving from the camera into clean H.264 / H.265 video frames.

---

### 2. Media: Stream Broker & Hardware Decoder (`include/nvr/media/` & `src/media/`)
* **`stream_broker.h / .cpp`**: The central video splitter. Takes the compressed camera stream and creates branch copies in memory for recording and live viewing without duplicating network traffic.
* **`video_decoder.h / .cpp`**: Hardware VPU video decoder. Uses the board's V4L2 hardware chip (`v4l2h264dec`) to unpack compressed video into picture frames for display without burning CPU.
* **`video_scaler.h / .cpp`**: Hardware image resizer and color converter. Scales frames to fit different UI grid boxes.
* **`buffer_allocator.h / .cpp`**: Video memory manager. Uses Linux DMA-BUF to share video frames between decoder, display, and AI with zero memory copies.

---

### 3. Recording: Zero-Transcoding Engine (`include/nvr/recording/` & `src/recording/`)
* **`stream_segmenter.h / .cpp`**: Splits the incoming compressed 1080p stream into 1-minute video files on keyframe boundaries. Does **zero decoding/re-encoding** (stream copy), keeping CPU under 1%.
* **`atomic_writer.h / .cpp`**: Safe file writer. Writes video to a temporary `.tmp` file first, flushes to disk, and atomically renames to `.mp4`. If power is pulled, files never corrupt.
* **`recording_scheduler.h / .cpp`**: Checks configured schedules to decide when each camera should record (continuous 24/7 vs. only on motion).

---

### 4. Live: Real-Time Grid Display (`include/nvr/live/` & `src/live/`)
* **`live_queue.h / .cpp`**: Bounded live video queue. Drops older frames if the display is slow (head-drop policy) so live video **never lags behind real time**.
* **`display_backend.h`**: Hardware-agnostic screen display interface. Renders video frames onto the monitor using the system windowing/compositor (Wayland/Weston).

---

### 5. Playback: Past Video Player (`include/nvr/playback/` & `src/playback/`)
* **`playback_engine.h / .cpp`**: Opens stored `.mp4` video files from disk and sends them to the hardware decoder to watch past footage.
* **`seek_controller.h / .cpp`**: Handles timeline scrub jumps, pause, slow motion, and multi-speed playback ($1\times, 2\times, 4\times$, reverse).
* **`clip_exporter.h / .cpp`**: Cuts a selected time range from disk and saves it to a USB drive or file download without re-encoding.

---

### 6. AI: Vision & Rule Engine (`include/nvr/ai/` & `src/ai/`)
* **`motion_gate.h / .cpp`**: Fast activity checker. Quickly compares video frames; if nothing moved in the scene, skips expensive AI processing.
* **`ai_scheduler.h / .cpp`**: Capacity admission controller. Makes sure AI jobs run only when processing headroom is available ($\rho < 1.0$), dropping stale frames if overloaded.
* **`inference_backend.h / .cpp`**: Runs quantized INT8 AI models on the dedicated 8 eTOPS hardware NPU chip.
* **`object_tracker.h / .cpp`**: Tracks detected people or cars across consecutive frames (ByteTrack / OC-SORT) so a single person generates one continuous track.
* **`rule_engine.h / .cpp`**: Checks security rules (line crossing, restricted zone entry, loitering dwell time) to trigger alerts.

---

### 7. Storage: Database & Disk Retention (`include/nvr/storage/` & `src/storage/`)
* **`database_manager.h / .cpp`**: SQLite database connection using Write-Ahead Logging (WAL) for fast, crash-safe metadata queries.
* **`segment_index.h / .cpp`**: Stores and searches recorded `.mp4` file paths, start times, and end times.
* **`event_index.h / .cpp`**: Stores AI detection history (camera number, event type, timestamp, bounding box).
* **`retention_manager.h / .cpp`**: Disk space monitor. Automatically deletes the oldest recordings when the disk fills up, but strictly protects locked/event clips.

---

### 8. Health: Telemetry & Load Shedding (`include/nvr/health/` & `src/health/`)
* **`metrics_collector.h / .cpp`**: Reads CPU usage, memory consumption, decoder load, and chip temperatures from `/sys/class/thermal`.
* **`load_shedder.h / .cpp`**: Protective feedback loop. If the chip gets too hot or CPU exceeds 50%, automatically reduces AI frame rate to protect recording.

---

### 9. Dispatcher: Centralized Command Controller (`include/nvr/dispatcher/` & `src/dispatcher/`)
* **`command_dispatcher.h / .cpp`**: The single brain of the NVR. All actions from UI, API, or Voice must pass through here for execution.
* **`commands.h`**: Defines all structured JSON command schemas (e.g., `START_RECORDING`, `SHOW_CAMERA`, `SET_LAYOUT`).
* **`rbac_validator.h / .cpp`**: Security gatekeeper. Checks user passwords and permissions before allowing any command to run.

---

### 10. API: Remote Network Gateway (`include/nvr/api/` & `src/api/`)
* **`rest_server.h / .cpp`**: Embedded HTTP server allowing remote web browsers or mobile apps to query camera status and settings.
* **`websocket_server.h / .cpp`**: Real-time WebSocket connection that pushes live AI detection alerts to connected clients instantly.

---

### 11. Notifications: Alarms & Alerts (`include/nvr/notifications/` & `src/notifications/`)
* **`notification_manager.h / .cpp`**: Coordinates dispatching alerts to multiple targets when an event triggers.
* **`email_notifier.h / .cpp`**: Sends email alerts with snapshot attachments via SMTP.
* **`gpio_alarm.h / .cpp`**: Triggers a physical hardware buzzer or relay alarm light.

---

### 12. Voice: Isolated Voice Control (`include/nvr/voice/` & `src/voice/`)
* **`audio_capture.h / .cpp`**: Captures microphone audio using ALSA (16 kHz Mono PCM).
* **`vad_detector.h / .cpp`**: Voice Activity Detector. Detects when someone begins speaking so the speech model stays idle during silence.
* **`asr_engine.h / .cpp`**: Small on-device speech-to-text engine. Converts spoken audio into text words.
* **`grammar_parser.h / .cpp`**: Finite-State Grammar parser. Matches spoken phrases (e.g., *"show camera 3"*) into structured NVR commands.

---

### 13. Common: Base Utilities (`include/nvr/common/` & `src/common/`)
* **`types.h`**: Fundamental data types used across the system (Camera IDs, Packet buffers, Frame structures, Codecs).
* **`time_utils.h`**: Time helper separating monotonic clock (for internal interval timers) from wall-clock time (for video timestamps).
* **`thread_pool.h / .cpp`**: Worker thread pool for handling background tasks without blocking media streams.
* **`logger.h / .cpp`**: Fast, thread-safe logger for console and file log outputs.

---

## Build Instructions

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
./nvr_server
```

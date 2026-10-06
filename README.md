# NVR Backend Engine (C++)

High-performance C++ backend service for the Network Video Recorder (NVR). Runs 24/7 in the background on the device.

## Core Rule
* **Recording stays compressed:** Direct stream-copy to MP4 (zero transcoding, CPU < 50%).
* **Live View & AI use decoded SUB stream:** Low-res stream decoded via hardware VPU and analyzed on hardware NPU.

---

## File Guide: Which File For What

### 1. Camera Connections (`include/nvr/ingress/` & `src/ingress/`)
* **`camera_manager.h / .cpp`**: Tracks all cameras, checks online/offline state, auto-reconnects.
* **`stream_session.h / .cpp`**: 1 RTSP connection per camera (pulls MAIN and SUB streams).
* **`onvif_client.h / .cpp`**: Finds IP cameras on local network; sends PTZ motor commands.
* **`rtp_depacketizer.h / .cpp`**: Unpacks network RTP packets into H.264/H.265 video frames.

### 2. Video Processing & Decode (`include/nvr/media/` & `src/media/`)
* **`stream_broker.h / .cpp`**: Splits camera stream in memory to recording, live view, and AI.
* **`video_decoder.h / .cpp`**: Hardware VPU decoder (decodes video without burning CPU).
* **`video_scaler.h / .cpp`**: Resizes frames for UI grid layouts.
* **`buffer_allocator.h / .cpp`**: Shared DMA memory manager for zero-copy frame sharing.

### 3. Recording (`include/nvr/recording/` & `src/recording/`)
* **`segmenter.h / .cpp`**: Splits compressed video into 1-minute MP4 files on keyframes.
* **`atomic_writer.h / .cpp`**: Crash-safe writer (writes `.tmp` file, then renames to `.mp4`).
* **`recording_scheduler.h / .cpp`**: Checks 24/7 continuous vs motion recording schedules.

### 4. Live Display (`include/nvr/live/` & `src/live/`)
* **`live_queue.h / .cpp`**: Real-time queue that drops old frames if display is slow (zero lag).

### 5. Playback (`include/nvr/playback/` & `src/playback/`)
* **`playback_engine.h / .cpp`**: Reads saved MP4 files from disk and sends to decoder.
* **`seek_controller.h / .cpp`**: Scrubbing, pause, slow-mo, and fast-forward (2x, 4x, etc.).
* **`clip_exporter.h / .cpp`**: Exports time slices to USB without re-encoding.

### 6. AI Detection (`include/nvr/ai/` & `src/ai/`)
* **`motion_gate.h / .cpp`**: Skips AI processing if the scene is completely still.
* **`ai_scheduler.h / .cpp`**: Regulates frame rate to keep NPU load below 100%.
* **`inference_backend.h / .cpp`**: Runs INT8 models on the 8 eTOPS hardware NPU.
* **`object_tracker.h / .cpp`**: Tracks person/car across frames with persistent IDs.
* **`rule_engine.h / .cpp`**: Checks tripwire line-crossing and restricted zones.

### 7. Storage (`include/nvr/storage/` & `src/storage/`)
* **`database_manager.h / .cpp`**: SQLite connection in WAL mode for fast metadata storage.
* **`segment_index.h / .cpp`**: Stores start/end times and paths of recorded MP4 files.
* **`event_index.h / .cpp`**: Stores AI detection events (camera, timestamp, object type).
* **`retention_manager.h / .cpp`**: Deletes oldest non-event videos when disk is full.

### 8. Health & Monitoring (`include/nvr/health/` & `src/health/`)
* **`metrics_collector.h / .cpp`**: Reads CPU, RAM, disk space, and chip temperatures.
* **`load_shedder.h / .cpp`**: Throttles AI frame rate if CPU exceeds 50% or chip gets hot.

### 9. Dispatcher & API (`include/nvr/dispatcher/`, `api/`)
* **`command_dispatcher.h / .cpp`**: Central router where all UI, API, and voice commands run.
* **`commands.h`**: List of all standard JSON command schemas.
* **`rbac_validator.h / .cpp`**: Checks user login and permissions (Admin vs Viewer).
* **`rest_server.h / .cpp`**: HTTP server for remote web browsers and mobile apps.
* **`websocket_server.h / .cpp`**: Pushes live AI alert notifications to connected clients.

### 10. Notifications & Voice (`include/nvr/notifications/`, `voice/`)
* **`notification_manager.h / .cpp`**: Dispatches alerts to email, buzzer, and UI.
* **`email_notifier.h / .cpp`**: Sends alert emails with snapshot attachments.
* **`gpio_alarm.h / .cpp`**: Triggers hardware buzzer or relay siren light.
* **`voice_service.h / .cpp`**: Runs on-device voice command pipeline (mic -> VAD -> ASR -> command).

### 11. Common Utilities (`include/nvr/common/`)
* **`types.h`**: Shared data types (Camera IDs, Frames, Codecs).
* **`time_utils.h`**: Monotonic clock for timers, wall clock for timestamps.
* **`thread_pool.h / .cpp`**: Background worker threads.
* **`logger.h / .cpp`**: Thread-safe console and file logger.

---

## How to Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
./nvr_server
```

# CLAUDE.md — dms-pi

## Project Overview
**DMS Pi** — C++ Driver Monitoring System + Dash Cam for Raspberry Pi Zero 2W.
Single binary, 3 threads, stdin MJPEG pipe. Target ~50-60MB RAM.

**GitHub:** https://github.com/sreenathgopal/dms-pi

## Architecture

```
rpicam-vid (640x480@10fps MJPEG, hardware encode)
    │
    └─ stdout pipe via tee
        ├─ ffmpeg -c:v copy → /home/test/recordings/rec_*.avi (5-min segments, ~0% CPU)
        │
        └─ dms --stdin --headless (SINGLE BINARY)
              │
              ├── Thread 1: Capture (main thread)
              │   - read_mjpeg_frame() from stdin
              │   - Push raw JPEG → ring_buffer (90s, zero CPU)
              │   - Every Nth frame: imdecode → push to FrameSlot
              │
              ├── Thread 2: Detection
              │   - Wait on condition_variable for new frame
              │   - YuNet face detection (~100ms)
              │   - TFLite landmark tracking (~147ms)
              │   - TFLite INT8 classifier (~1ms)
              │   - State machine → buzz/LED decision
              │   - GPIO inline (microseconds)
              │   - On alert: spawn detached thread for clip save
              │   - Update shared AppState (for web API)
              │
              └── Thread 3: Web server (libmicrohttpd internal)
                  - GET /api/status → detection FPS, memory, disk, alerts
                  - GET /api/recordings → list recording files
                  - GET /api/alerts → list alert clips
                  - GET /api/download?f=xxx → HTTP Range download
                  - DELETE /api/file?f=xxx → delete file
                  - GET /api/config, POST /api/config → read/update config
                  - POST /api/time → sync Pi clock from phone
                  - GET /api/stream → MJPEG live preview (~2fps)
```

## CPU Budget (4 Cores)

| Core | Process/Thread | CPU |
|------|---------------|-----|
| 0 | rpicam-vid (~22%) + ffmpeg copy-mux (~0%) | ~22% |
| 1 | Capture thread — stdin read + ring buffer + JPEG decode | ~5% |
| 2 | Detection thread — YuNet + TFLite + state machine + GPIO | ~80-90% |
| 3 | OS + web server (idle) + alert clip writer (burst) | ~2% |

## Memory Budget (416MB total)

| Component | RAM |
|-----------|-----|
| Linux OS + system services | ~80MB |
| rpicam-vid | ~8MB |
| ffmpeg copy-mux | ~5MB |
| **dms binary** | **~50MB** |
| — OpenCV loaded libs | ~20MB |
| — TFLite models in memory | ~5MB |
| — Ring buffer (900 JPEG frames) | ~20MB |
| — Stack + heap + misc | ~5MB |
| Swap headroom | ~10GB available |
| **Total** | **~143MB** (vs ~235MB with 2-app setup) |

## Source Files

### Headers (`include/dms/`)
| File | Purpose |
|------|---------|
| `config.h` | Config struct + load/parse |
| `app_state.h` | FrameSlot (capture→detect handoff) + AppState (shared metrics) |
| `ring_buffer.h` | Thread-safe circular JPEG buffer |
| `face_detector.h` | YuNet face detector (Haar cascade fallback) |
| `landmark_tracker.h` | TFLite 478-point face landmark |
| `classifier.h` | TFLite INT8 drowsiness classifier |
| `state_machine.h` | Time-windowed drowsiness state machine |
| `alert_service.h` | GPIO utility functions (buzz/LED) |
| `management_service.h` | Web server start/stop |

### Sources (`src/`)
| File | Lines | Purpose |
|------|-------|---------|
| `main.cpp` | ~220 | 3-thread main, stdin capture, detection loop, alert clips |
| `config.cpp` | ~110 | JSON config + env var overrides |
| `face_detector.cpp` | ~110 | YuNet + Haar fallback |
| `landmark_tracker.cpp` | ~210 | TFLite landmark processing |
| `classifier.cpp` | ~145 | TFLite INT8 inference |
| `state_machine.cpp` | ~115 | Drowsiness detection FSM |
| `alert_service.cpp` | ~80 | libgpiod GPIO init/buzz/led/cleanup |
| `management_service.cpp` | ~400 | libmicrohttpd REST API + file serving |

## CLI Options

```
dms --stdin --headless              # Operation mode (default)
dms --stdin --headless --debug      # Debug mode (verbose FPS, per-frame stats)
dms --stdin --headless --port 9090  # Override web port
dms --stdin --headless --fps 15     # Override FPS
```

## Build

```bash
# On Pi Zero 2W (Bookworm 64-bit)
sudo bash scripts/install_deps.sh           # apt: opencv, gpiod, microhttpd, ffmpeg
bash scripts/build_tflite.sh                # one-time ~30min (or cross-build on Pi 5)
bash models/download_models.sh              # YuNet ONNX + TFLite models
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DOpenCV_DIR=/usr/local/lib/cmake/opencv4 ..
cmake --build . -j2
```

## Deploy

```bash
sudo bash scripts/deploy.sh
sudo systemctl start dms
journalctl -u dms -f
curl http://localhost:8080/api/status
```

## Dependencies

| Dependency | Type | Required |
|-----------|------|----------|
| OpenCV 4.8+ | System (built from source) | Yes |
| TFLite v2.16.1 | Built from source | Optional (DMS_NO_TFLITE) |
| libgpiod | System (apt) | Optional (DMS_NO_GPIO) |
| libmicrohttpd | System (apt) | Yes |
| nlohmann/json | FetchContent | Yes |

## Compile Guards
- `DMS_NO_TFLITE` — auto-set if TFLite not found (threshold-based fallback)
- `DMS_NO_GPIO` — auto-set if libgpiod not found (log-only alerts)

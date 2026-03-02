# CLAUDE.md — dms-pi

## Project Overview
**DMS Pi** — C++ Driver Monitoring System for Raspberry Pi Zero 2W.
Single process, 6 threads, ZeroMQ `inproc://` transport. Target ~60-80MB RAM.

**GitHub:** https://github.com/sreenathgopal/dms-pi

## Session History (2026-02-28)

### What was done this session:
1. Created `dms-pi` as a clean copy of `dms-cpp` (at `C:\Users\sreen\Desktop\dms-cpp`)
2. Stripped ALL `#ifdef _WIN32` guards from source files — this is Pi-only code
3. Cleaned CMakeLists.txt — removed WIN32 guards, ws2_32 linking, DMS_NO_GPS Windows trigger
4. Fixed `deploy.sh` — auto-detects `/boot/firmware` (Bookworm) vs `/boot`
5. Fixed `config.cpp` — fallback from `/boot/dms_config.json` to `/boot/firmware/dms_config.json`
6. Fixed `models/download_models.sh` — auto-extracts `face_landmark.tflite` from MediaPipe `.task` ZIP bundle
7. Created `.gitignore`, `README.md`
8. Initialized git, created GitHub repo, pushed initial commit
9. Installed GitHub CLI (`gh`) via winget

### Files modified (vs original dms-cpp):
- `include/dms/config.h` — removed 5 `#ifdef _WIN32` blocks
- `src/main.cpp` — removed Windows mkdir, signal, includes
- `src/storage_service.cpp` — removed Windows mkdir, localtime_s, backslash path handling
- `src/management_service.cpp` — removed windows.h/psapi.h, GetProcessMemoryInfo, _popen/_pclose
- `src/uplink_service.cpp` — removed _mkdir/_unlink macros, localtime_s
- `src/config.cpp` — added Bookworm `/boot/firmware` fallback
- `CMakeLists.txt` — removed all WIN32 conditionals
- `scripts/deploy.sh` — added BOOT_DIR detection for Bookworm
- `models/download_models.sh` — replaced placeholder with working ZIP extraction

### What's next:
- Clone on Pi Zero 2W (Bookworm 64-bit) and build
- Run `scripts/install_deps.sh`, `scripts/build_tflite.sh`, `models/download_models.sh`, `scripts/build.sh`
- Deploy with `scripts/deploy.sh`
- Test with camera + GPS hardware

## Architecture
6 threads sharing one `zmq::context_t`:

| Thread | ZMQ Role | Purpose |
|--------|----------|---------|
| Camera | PUB `inproc://detection`, SUB `inproc://gps` | YuNet + TFLite landmarks + classifier + state machine |
| GPS | PUB `inproc://gps` | POSIX serial NMEA RMC parsing |
| Alert | SUB `inproc://detection` | libgpiod buzzer + LED |
| Storage | SUB detection + gps | SQLite3 writes |
| Uplink | SUB detection + gps | libcurl fleet API |
| Management | — | libmicrohttpd HTTP :8080 |

## Build (on Pi)
```bash
sudo bash scripts/install_deps.sh
bash scripts/build_tflite.sh       # one-time ~30min
bash models/download_models.sh
bash scripts/build.sh
sudo bash scripts/deploy.sh
sudo systemctl start dms
```

## Compile Guards
- `DMS_NO_TFLITE` — set automatically if TFLite not found
- `DMS_NO_GPIO` — set automatically if libgpiod not found
- `DMS_NO_GPS` — NOT set by default (only relevant if manually defined)

## Key Dependencies
System: OpenCV, ZeroMQ, SQLite3, libcurl, libgpiod, libmicrohttpd
Header-only (FetchContent): cppzmq, nlohmann/json, msgpack-cxx
Built from source: TFLite C++ v2.16.1

## Related Project
- `C:\Users\sreen\Desktop\dms-cpp` — Windows dev version with `#ifdef _WIN32` guards (not for Pi)
- `C:\Users\sreen\Desktop\optimized` — Original Python DMS

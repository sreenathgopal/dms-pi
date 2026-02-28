# DMS C++ — Driver Monitoring System

High-performance C++ driver drowsiness detection system for Raspberry Pi Zero 2W. Detects eye closure, yawning, and gaze deviation in real time using camera + TFLite inference, triggers GPIO buzzer/LED alerts, and reports to a fleet management API.

## Target Hardware

- Raspberry Pi Zero 2W (512MB RAM, 4-core Cortex-A53)
- Camera module (CSI or USB)
- GPS module (serial `/dev/ttyACM0`)
- GPIO buzzers (pins 17, 22) + LED (pin 4)

## Architecture

Single process, 6 threads, ZeroMQ `inproc://` transport (~60-80MB RAM).

| Thread | Purpose |
|--------|---------|
| Camera | YuNet face detect + TFLite landmarks + classifier + state machine |
| GPS | NMEA RMC parsing from serial |
| Alert | Buzzer + LED via libgpiod |
| Storage | SQLite3 local database |
| Uplink | Fleet API POST + base64 image upload |
| Management | HTTP :8080 JSON API |

## Quick Start (Raspberry Pi OS Bookworm 64-bit)

```bash
# 1. Clone
git clone <your-repo-url> dms-pi
cd dms-pi

# 2. Install system dependencies
sudo bash scripts/install_deps.sh

# 3. Build TFLite C++ from source (one-time, ~30 min)
bash scripts/build_tflite.sh

# 4. Download face detection + landmark models
bash models/download_models.sh

# 5. Build DMS
bash scripts/build.sh

# 6. Deploy (installs to /opt/dms + systemd service)
sudo bash scripts/deploy.sh

# 7. Start
sudo systemctl start dms
journalctl -u dms -f

# 8. Verify
curl http://localhost:8080/health
```

## Configuration

Config is loaded from `/boot/dms_config.json` (or `/boot/firmware/dms_config.json` on Bookworm), then `DMS_*` environment variables override.

```json
{
    "frame_w": 320,
    "frame_h": 240,
    "fps": 15,
    "skip_frames": 2,
    "speed_threshold": 0,
    "use_tflite": true
}
```

Override via env: `DMS_FRAME_W=240 DMS_USE_TFLITE=false /opt/dms/bin/dms`

## Dependencies

**System (apt):** OpenCV 4.6+, ZeroMQ, SQLite3, libcurl, libgpiod, libmicrohttpd

**Header-only (CMake FetchContent):** cppzmq, nlohmann/json, msgpack-cxx

**Built from source:** TensorFlow Lite C++ API v2.16.1

## HTTP API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/health` | Health check |
| GET | `/api/status` | Uptime, memory, config |
| GET | `/api/devices` | Device ID |
| GET | `/api/users` | User info |
| GET | `/api/config` | Current configuration |
| POST | `/api/create_device` | Register with fleet API |
| POST | `/api/handshake` | API handshake |
| POST | `/api/config/speed` | Set speed threshold |
| POST | `/api/wifi` | Connect to WiFi |

## Memory Comparison

| | Python | C++ |
|--|--------|-----|
| Total RAM | ~270MB | ~60-80MB |

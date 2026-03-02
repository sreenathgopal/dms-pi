# DMS Pi — Session Log (2026-03-01)

## Overview

This session focused on getting both apps (video_recorder + DMS) running together on the Pi Zero 2W, diagnosing a critical CPU bottleneck that limited DMS to 1.5 FPS, and restructuring the video recorder to achieve **10.6 FPS** — a **7x improvement**.

---

## Problem Diagnosis

### Initial State
Two apps run on the Pi:
1. **video_recorder** (C++) — captures camera, records video, publishes frames via ZMQ
2. **dms_optimized.py** (Python) — receives frames via ZMQ, detects drowsiness

When both ran together, DMS could only process **1.5 frames per second** — far too slow for reliable drowsiness detection (yawns were being missed entirely).

### Root Cause: Triple JPEG Encoding

The video_recorder's main loop ran at 30fps and performed **3 separate JPEG encode operations** on every single 1920x1080 frame:

```
┌─────────────── ORIGINAL MAIN LOOP (every frame, 30fps) ───────────────┐
│                                                                         │
│  cap.read(frame)  →  1920x1080 raw BGR frame                          │
│       │                                                                 │
│       ├──→ cv::imencode(".jpg", frame)  →  ring buffer     [JPEG #1]  │
│       │    (full 1080p, quality 85)                                    │
│       │                                                                 │
│       ├──→ cv::resize(frame, 640x480)                                  │
│       │    cv::imencode(".jpg", small)  →  ZMQ publish     [JPEG #2]  │
│       │                                                                 │
│       └──→ writer.write(frame)          →  AVI file        [JPEG #3]  │
│            (MJPEG codec = another JPEG encode internally)              │
│                                                                         │
│  Total: 3 × JPEG encode of 1080p per frame × 30fps = ~90% CPU        │
└─────────────────────────────────────────────────────────────────────────┘
```

This consumed **~90% of one CPU core**, leaving insufficient resources for the DMS Python app.

### Why FPS Was Low

With the recorder eating 90% CPU + the DMS needing CPU for:
- YuNet face detection (~100ms per frame)
- TFLite landmark inference (~147ms per frame)
- Drowsiness classifier

The DMS was starved for CPU cycles, resulting in only **1.5 FPS**.

---

## Failed Attempts (DMS-side optimizations)

Before identifying the recorder as the bottleneck, several DMS-side optimizations were tried:

### 1. Frame Downscaling (`PROCESS_WIDTH`)
- Added `PROCESS_WIDTH = 320` (then 160) to resize frames before detection
- **Result:** Minimal improvement (1.3→1.5 FPS) — YuNet was already fast, not the bottleneck

### 2. Landmark Skipping (`LANDMARK_EVERY_N`)
- Run TFLite landmark model only every 5th frame, reuse cached EAR/MAR values
- **Result:** Minimal improvement — total loop time was dominated by CPU contention, not per-frame cost

### 3. TFLite Multi-threading (`num_threads=4`)
- Changed from 2 to 4 threads for TFLite XNNPACK delegate
- **Result:** Actually worse — 4 threads competing with the recorder on 4 cores caused more contention

### 4. INT8 Model Quantization
- Attempted to quantize the float32 landmark model (2.5MB) to INT8
- **Blocked:** No TensorFlow available (Python 3.14 on Windows, no pip on WSL, Docker not running)
- The model needs full TF converter for proper INT8 quantization

**Conclusion:** The real fix had to come from reducing the recorder's CPU usage.

---

## Solution: rpicam-vid Pipeline Architecture

### Key Insight
The Pi has `rpicam-vid` — a tool that captures from the camera using libcamera. It can output MJPEG to stdout. Instead of the C++ app opening the camera and doing all encoding itself, we pipe rpicam-vid's output to a simplified C++ app.

### Camera Sharing Constraint
Only **one process** can access the Pi camera via libcamera at a time. rpicam-vid and the C++ app cannot both open `/dev/video0`. Solution: rpicam-vid owns the camera exclusively and pipes MJPEG frames to the C++ app via stdout.

### New Architecture
```
rpicam-vid (owns camera, 640x480 @ 10fps, MJPEG output to stdout)
    │
    │  stdout pipe (MJPEG byte stream)
    │
    └──→ video_recorder --stdin --headless
              │
              ├── Parse MJPEG stream (scan for SOI/EOI markers)
              ├── Store raw JPEG bytes in ring buffer (zero CPU — no re-encode)
              ├── Pass-through JPEG bytes to ZMQ :5556 (zero CPU — no re-encode)
              ├── Receive alerts on ZMQ :5555
              └── On alert: decode ring buffer JPEGs → write AVI clip
```

### Launch Command
```bash
rpicam-vid -t 0 --width 640 --height 480 --framerate 10 \
  --codec mjpeg --nopreview -o - 2>/dev/null \
  | stdbuf -oL ./build/video_recorder --stdin --headless 2>&1 | tee /tmp/recorder.log
```

---

## Detailed Code Changes

### A. video_recorder.cpp — Complete Rewrite

**File:** `backup/video_recorder/video_recorder.cpp` (new version)
**Original:** `backup/video_recorder/video_recorder.cpp.bak`

#### Change 1: Added MJPEG stdin reader function

```cpp
bool read_mjpeg_frame(std::vector<uchar> &jpeg_out) {
    jpeg_out.clear();

    // Scan for SOI marker (0xFF 0xD8) — start of JPEG
    int prev = -1, curr;
    while ((curr = getchar_unlocked()) != EOF) {
        if (prev == 0xFF && curr == 0xD8) {
            jpeg_out.push_back(0xFF);
            jpeg_out.push_back(0xD8);
            break;
        }
        prev = curr;
    }
    if (curr == EOF) return false;

    // Read until EOI marker (0xFF 0xD9) — end of JPEG
    prev = -1;
    while ((curr = getchar_unlocked()) != EOF) {
        jpeg_out.push_back((uchar)curr);
        if (prev == 0xFF && curr == 0xD9) {
            return true;  // Complete JPEG frame
        }
        prev = curr;
    }
    return false;  // EOF before EOI
}
```

**Why:** rpicam-vid with `--codec mjpeg` outputs a continuous stream of JPEG images concatenated together. Each JPEG starts with bytes `0xFF 0xD8` (SOI = Start Of Image) and ends with `0xFF 0xD9` (EOI = End Of Image). This function reads one complete JPEG frame at a time from stdin. Uses `getchar_unlocked()` for maximum throughput (no mutex overhead).

#### Change 2: Added `--stdin` command-line flag

```cpp
bool stdin_mode = false;
// ...
else if (arg == "--stdin") { stdin_mode = true; }
```

**Why:** The app supports two modes — direct camera access (original behavior) and stdin pipe mode (new). This flag switches between them.

#### Change 3: Simplified main loop (stdin mode)

**Before (original):** Each frame required 3 operations:
```cpp
// OLD: Read frame from camera
cap.read(frame);          // ~0ms (DMA from camera)
cv::flip(frame, frame, 1);

// OLD: JPEG #1 — for ZMQ publish
cv::resize(frame, small_frame, cv::Size(640, 480));
cv::imencode(".jpg", small_frame, buf);  // ~15ms CPU
// ... send via ZMQ ...

// OLD: JPEG #2 — for ring buffer
cv::imencode(".jpg", frame, bf.jpeg_data, {QUALITY, 85});  // ~40ms CPU (1080p!)
ring_buffer.push(bf);

// OLD: JPEG #3 — for AVI recording
writer.write(frame);  // MJPEG codec = another ~40ms CPU internally
```

**After (new stdin mode):**
```cpp
// NEW: Read already-encoded JPEG from rpicam-vid pipe
read_mjpeg_frame(jpeg_buf);  // Just reads bytes, no CPU encoding

// NEW: Ring buffer — store raw JPEG bytes directly (ZERO CPU)
ring_buffer.push(jpeg_buf, now);

// NEW: ZMQ publish — pass through raw JPEG bytes (ZERO CPU)
memcpy(ptr + 8, jpeg_buf.data(), jpeg_buf.size());
frame_pub.send(zmsg, ZMQ_DONTWAIT);

// NEW: No VideoWriter at all — continuous recording removed
```

**Why:** In stdin mode, rpicam-vid already provides JPEG-encoded frames. The C++ app just passes the raw bytes to both the ring buffer and ZMQ without any re-encoding. This eliminates all 3 JPEG encode operations.

#### Change 4: Ring buffer stores raw JPEG bytes

**Before:**
```cpp
void push(const cv::Mat &frame, time_t ts) {
    // Encoded 1080p frame to JPEG every time (expensive!)
    cv::imencode(".jpg", frame, bf.jpeg_data, {QUALITY, 85});
    buffer_.push_back(bf);
}
```

**After:**
```cpp
void push(const std::vector<uchar> &jpeg, time_t ts) {
    // Store already-encoded JPEG directly (zero CPU)
    buffer_.push_back({jpeg, ts});
}
```

**Why:** The JPEG data from rpicam-vid is already encoded. Storing it directly saves ~40ms of CPU per frame that was previously spent on `cv::imencode()`.

#### Change 5: Removed VideoWriter continuous recording

**Before:** Every frame was written to an AVI file using MJPEG codec (another JPEG encode).
**After:** No continuous recording. Only alert clips are saved from the ring buffer when an alert triggers.

**Why:** This was the 3rd JPEG encode per frame. Continuous recording can be added back later using `tee` in the pipeline to save the raw MJPEG stream to disk (zero CPU).

#### Change 6: Reduced defaults

```cpp
#define VIDEO_FPS           10   // was 30
#define FRAME_SEND_WIDTH    640  // unchanged
#define FRAME_SEND_HEIGHT   480  // unchanged
```

**Why:** 10fps is sufficient for drowsiness detection (eye closures last 2+ seconds). Reducing from 30fps cuts the work by 3x.

#### Change 7: Removed Windows code

All `#ifdef _WIN32` blocks, `_WIN32` includes (`windows.h`, `direct.h`), and Windows-specific functions (`_mkdir`, `GetDiskFreeSpaceEx`, `FindFirstFile`) were removed. This is Pi-only code.

---

### B. dms_optimized.py — Configuration & Detection Changes

**File:** `backup/dms/dms_optimized.py` (modified version)
**Original:** `backup/dms/dms_optimized.py.bak`

#### Change 1: Frame downscaling before detection (line ~65-66)

```python
# Added after BUFFER_SIZE = 5
PROCESS_WIDTH = 160
LANDMARK_EVERY_N = 5
```

And in the main loop (after frame received, before detection):
```python
# Downscale for faster processing
h_orig, w_orig = frame.shape[:2]
if w_orig > PROCESS_WIDTH:
    scale = PROCESS_WIDTH / w_orig
    frame = cv2.resize(frame, (PROCESS_WIDTH, int(h_orig * scale)))
```

**Why:** The incoming frame is 640x480 from ZMQ. Downscaling to 160px wide before running YuNet face detection reduces the detection cost. YuNet dynamically adjusts its input size via `setInputSize()`.

#### Change 2: Landmark inference skipping (line ~648)

```python
# Before: ran landmarks on EVERY frame
landmarks = landmark_det.detect(frame, face)

# After: only run every LANDMARK_EVERY_N frames
if landmark_counter >= LANDMARK_EVERY_N:
    landmark_counter = 0
    landmarks = landmark_det.detect(frame, face)
    # ... compute EAR/MAR, run classifier ...
    # Cache results
    cached_ear_l = ear_l
    cached_ear_r = ear_r
    cached_mar = mar_val
    cached_eyes_closed = eyes_closed
    cached_is_yawning = is_yawning
else:
    # Reuse cached values (skip expensive landmark inference)
    ear_l = cached_ear_l
    ear_r = cached_ear_r
    mar_val = cached_mar
    eyes_closed = cached_eyes_closed
    is_yawning = cached_is_yawning
```

**Why:** TFLite landmark inference (256x256 input, float32 model) takes ~147ms per frame — the most expensive operation. By only running it every 5th frame and reusing the last EAR/MAR values, we save ~80% of landmark inference CPU. The cached values are still valid because facial state (eyes open/closed, mouth open/closed) doesn't change within 0.5 seconds.

#### Change 3: Separate yawn trigger duration (line ~60)

```python
CLOSURE_TRIGGER = 2.0   # Eye closure must last 2s to trigger
YAWN_TRIGGER = 1.0      # Yawn must last 1s to trigger (NEW)
```

#### Change 4: EventTracker accepts custom trigger_secs (line ~410)

```python
# Before:
class EventTracker:
    def __init__(self, name):
        self.name = name
        # ... used CLOSURE_TRIGGER for all trackers

# After:
class EventTracker:
    def __init__(self, name, trigger_secs=None):
        self.name = name
        self.trigger_secs = trigger_secs if trigger_secs else CLOSURE_TRIGGER
        # ...

    def update(self, is_active):
        # Before: if duration >= CLOSURE_TRIGGER:
        # After:
        if duration >= self.trigger_secs:
            # ... state transition
```

#### Change 5: Yawn tracker uses shorter trigger (line ~598)

```python
# Before:
yawn_tracker = EventTracker("YAWN")

# After:
yawn_tracker = EventTracker("YAWN", trigger_secs=YAWN_TRIGGER)
```

**Why:** The original code used `CLOSURE_TRIGGER = 2.0` for both eye and yawn detection. A yawn typically peaks and falls faster than an eye closure event. At 10.6 FPS with landmark skipping, the yawn MAR value was only above threshold for brief moments. Reducing the yawn trigger to 1 second allows detection of shorter yawns.

---

## Results Summary

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| Recorder CPU | ~90% | ~11% | **8x less** |
| DMS FPS | 1.5 | 10.6 | **7x faster** |
| System CPU idle | 23% | 64% | **3 cores free** |
| Eye detection | Working | Working | — |
| Yawn detection | Missed | Working | **Fixed** |
| Alert clips | Working | Working | — |
| RAM usage | 310MB | 235MB | **75MB less** |

---

## Files on Pi

### `/home/test/video_recorder/`
| File | Description |
|------|-------------|
| `video_recorder.cpp` | Modified source — stdin MJPEG reader, simplified pipeline |
| `video_recorder.cpp.bak` | Original source — direct camera, 3x JPEG encode, VideoWriter |
| `build/video_recorder` | Compiled binary (rebuilt after changes) |
| `CMakeLists.txt` | CMake build config (OpenCV + ZMQ) |
| `alerts/` | Alert clip output directory (cleared) |
| `output/` | Old continuous recordings directory (cleared) |

### `/home/test/dms/`
| File | Description |
|------|-------------|
| `dms_optimized.py` | Modified DMS — downscaling, landmark skip, yawn trigger |
| `dms_optimized.py.bak` | Original DMS script |
| `models/face_detection_yunet.onnx` | YuNet face detector (232KB) |
| `models/face_landmark.tflite` | 478-point landmark model, float32 (2.5MB) |
| `models/drowsiness_model_int8.tflite` | Drowsiness classifier, INT8 (5.4KB) |

---

## Pi Hardware & Network
- **Device:** Raspberry Pi Zero 2W (416MB RAM, 4 cores ARM Cortex-A53)
- **OS:** Raspberry Pi OS Bookworm 64-bit
- **Camera:** CSI camera via libcamera
- **IP:** 192.168.68.100
- **SSH:** `ssh -i .ssh/dms_pi test@192.168.68.100`
- **Swap:** 10GB configured

---

## Systemd Services (Auto-Start on Boot)

Both apps are configured as systemd services that start automatically on boot and restart on failure.

### Service 1: `dms-recorder.service`

**File:** `/etc/systemd/system/dms-recorder.service`

```ini
[Unit]
Description=DMS Video Recorder (rpicam-vid + ZMQ publisher)
After=network.target
Before=dms-detector.service

[Service]
Type=simple
User=test
WorkingDirectory=/home/test/video_recorder
ExecStart=/bin/bash -c 'rpicam-vid -t 0 --width 640 --height 480 --framerate 10 --codec mjpeg --nopreview -o - 2>/dev/null | /home/test/video_recorder/build/video_recorder --stdin --headless'
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
Environment=HOME=/home/test

[Install]
WantedBy=multi-user.target
```

**Key details:**
- `After=network.target` — starts after basic system init
- `Before=dms-detector.service` — ensures recorder starts before the DMS detector
- `Restart=always` + `RestartSec=5` — if the process dies, systemd restarts it after 5 seconds
- `User=test` — runs as the `test` user (has `video` group for camera access)
- The `ExecStart` runs the rpicam-vid pipe exactly as tested in the manual setup

### Service 2: `dms-detector.service`

**File:** `/etc/systemd/system/dms-detector.service`

```ini
[Unit]
Description=DMS Drowsiness Detector (Python)
After=dms-recorder.service
Requires=dms-recorder.service

[Service]
Type=simple
User=test
WorkingDirectory=/home/test/dms
ExecStartPre=/bin/sleep 3
ExecStart=/usr/bin/python3 -u /home/test/dms/dms_optimized.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
Environment=HOME=/home/test

[Install]
WantedBy=multi-user.target
```

**Key details:**
- `After=dms-recorder.service` + `Requires=dms-recorder.service` — won't start unless recorder is running; if recorder stops, detector stops too
- `ExecStartPre=/bin/sleep 3` — waits 3 seconds before starting to ensure the recorder's ZMQ publisher is ready
- `python3 -u` — unbuffered output so logs appear immediately in journalctl
- `Restart=always` — auto-restart on crash

### Service dependency chain:
```
boot → network.target → dms-recorder.service → (3s delay) → dms-detector.service
                              │                                      │
                         rpicam-vid pipe                    Python DMS detector
                         + video_recorder                  (YuNet + TFLite)
                         ZMQ :5556 (frames)  ←──────────→  ZMQ :5555 (alerts)
```

---

## Watchdog Cron Job

A cron job runs every 5 minutes to verify both services are active. If either is down, it restarts both.

### Watchdog Script

**File:** `/usr/local/bin/dms-watchdog.sh`

```bash
#!/bin/bash
# DMS Watchdog — checks every 5 min via cron
# Restarts services if not running

LOG=/tmp/dms-watchdog.log
DATE=$(date '+%Y-%m-%d %H:%M:%S')

RECORDER_ACTIVE=$(systemctl is-active dms-recorder.service)
DETECTOR_ACTIVE=$(systemctl is-active dms-detector.service)

if [ "$RECORDER_ACTIVE" != "active" ] || [ "$DETECTOR_ACTIVE" != "active" ]; then
    echo "[$DATE] WATCHDOG: recorder=$RECORDER_ACTIVE detector=$DETECTOR_ACTIVE — restarting" >> $LOG
    systemctl restart dms-recorder.service
    sleep 5
    systemctl restart dms-detector.service
    echo "[$DATE] WATCHDOG: services restarted" >> $LOG
else
    echo "[$DATE] WATCHDOG: OK (recorder=active, detector=active)" >> $LOG
fi

# Keep log small (last 100 lines)
tail -100 $LOG > $LOG.tmp && mv $LOG.tmp $LOG
```

**How it works:**
1. Checks `systemctl is-active` for both services
2. If either returns anything other than "active", restarts both in order (recorder first, 5s wait, then detector)
3. Logs every check to `/tmp/dms-watchdog.log` (auto-trimmed to 100 lines)

### Cron Entry

**Root crontab** (`sudo crontab -l`):
```
*/5 * * * * /usr/local/bin/dms-watchdog.sh
```

Runs every 5 minutes as root (needs root for `systemctl restart`).

---

## Managing the Services

```bash
# Check status of both services
sudo systemctl status dms-recorder dms-detector

# View live DMS detection logs
journalctl -u dms-detector -f

# View live recorder logs
journalctl -u dms-recorder -f

# View watchdog history
cat /tmp/dms-watchdog.log

# Manual restart
sudo systemctl restart dms-recorder dms-detector

# Stop both services
sudo systemctl stop dms-detector dms-recorder

# Disable auto-start on boot
sudo systemctl disable dms-recorder dms-detector

# Re-enable auto-start
sudo systemctl enable dms-recorder dms-detector
```

---

## How to Start the System (Manual — for debugging)

If you need to run outside systemd (e.g., for debugging):

```bash
# Stop services first
sudo systemctl stop dms-detector dms-recorder

# SSH into Pi
ssh -i .ssh/dms_pi test@192.168.68.100

# Terminal 1: Start recorder pipeline
cd /home/test/video_recorder
screen -dmS recorder bash -c 'rpicam-vid -t 0 --width 640 --height 480 --framerate 10 \
  --codec mjpeg --nopreview -o - 2>/dev/null \
  | stdbuf -oL ./build/video_recorder --stdin --headless 2>&1 | tee /tmp/recorder.log'

# Terminal 2: Start DMS (wait ~5s after recorder)
cd /home/test/dms
screen -dmS dms bash -c 'python3 -u dms_optimized.py > /tmp/dms.log 2>&1'

# Check status
tail -f /tmp/dms.log
top -d2
```

---

## What's Next
- Build and deploy the dms-pi C++ version (replaces Python DMS)
- Add continuous H.264 recording via `tee` in the rpicam-vid pipeline
- Consider higher resolution (1080p) with H.264 hardware encoding + tee split
- GPS hardware integration
- Alert buzzer/LED via libgpiod

#!/bin/bash
# =============================================================================
# DMS Pi — Full Setup Script
# Run this once after cloning on a fresh Raspberry Pi OS Bookworm Lite (64-bit)
#
# Usage: sudo bash scripts/setup_pi.sh
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${GREEN}[DMS]${NC} $1"; }
warn() { echo -e "${YELLOW}[DMS]${NC} $1"; }
err()  { echo -e "${RED}[DMS]${NC} $1"; }

# Must run as root
if [ "$(id -u)" -ne 0 ]; then
    err "This script must be run with sudo"
    echo "Usage: sudo bash scripts/setup_pi.sh"
    exit 1
fi

TOTAL_STEPS=7
STEP=0

next_step() {
    STEP=$((STEP + 1))
    echo ""
    echo "================================================================"
    log "[$STEP/$TOTAL_STEPS] $1"
    echo "================================================================"
}

# Track timing
START_TIME=$(date +%s)

# ─────────────────────────────────────────────────────────────────────────────
next_step "Installing system dependencies"
# ─────────────────────────────────────────────────────────────────────────────

apt-get update
apt-get install -y \
    build-essential cmake pkg-config git curl unzip \
    libopencv-dev \
    libzmq3-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libgpiod-dev \
    libmicrohttpd-dev

log "System dependencies installed"

# ─────────────────────────────────────────────────────────────────────────────
next_step "Building TensorFlow Lite C++ from source"
# ─────────────────────────────────────────────────────────────────────────────

TFLITE_VER="v2.16.1"
BUILD_DIR="/tmp/tflite-build"

if [ -f /usr/local/lib/libtensorflow-lite.a ]; then
    log "TFLite already built at /usr/local/lib/libtensorflow-lite.a — skipping"
else
    log "This will take ~30 minutes on Pi Zero 2W..."

    # Clone TF repo (sparse checkout — only what we need)
    if [ ! -d "$BUILD_DIR/tensorflow" ]; then
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        git clone --depth 1 --branch "$TFLITE_VER" \
            --filter=blob:none --sparse \
            https://github.com/tensorflow/tensorflow.git
        cd tensorflow
        git sparse-checkout set tensorflow/lite
    else
        cd "$BUILD_DIR/tensorflow"
    fi

    # Build TFLite static library
    mkdir -p build && cd build
    cmake ../tensorflow/lite \
        -DCMAKE_BUILD_TYPE=Release \
        -DTFLITE_ENABLE_GPU=OFF \
        -DTFLITE_ENABLE_XNNPACK=ON \
        -DCMAKE_CXX_FLAGS="-O2 -march=armv8-a+crc" \
        -DCMAKE_INSTALL_PREFIX=/usr/local

    make -j$(nproc)

    # Install
    cp -v libtensorflow-lite.a /usr/local/lib/
    mkdir -p /usr/local/include/tensorflow/lite
    cp -r ../tensorflow/lite/*.h /usr/local/include/tensorflow/lite/
    cp -r ../tensorflow/lite/c /usr/local/include/tensorflow/lite/
    cp -r ../tensorflow/lite/core /usr/local/include/tensorflow/lite/
    cp -r ../tensorflow/lite/kernels /usr/local/include/tensorflow/lite/
    cp -r ../tensorflow/lite/schema /usr/local/include/tensorflow/lite/

    # Install flatbuffers headers (TFLite dependency)
    if [ -d _deps/flatbuffers-src/include ]; then
        cp -r _deps/flatbuffers-src/include/flatbuffers /usr/local/include/
    fi

    log "TFLite built and installed"
fi

# ─────────────────────────────────────────────────────────────────────────────
next_step "Downloading face detection + landmark models"
# ─────────────────────────────────────────────────────────────────────────────

cd "$PROJECT_DIR/models"

# YuNet face detection ONNX (~400KB)
if [ ! -f face_detection_yunet.onnx ]; then
    log "Downloading YuNet face detector..."
    curl -L -o face_detection_yunet.onnx \
        "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
    log "YuNet: $(ls -lh face_detection_yunet.onnx | awk '{print $5}')"
else
    log "YuNet already present"
fi

# Face landmarks TFLite (~2.6MB)
if [ ! -f face_landmark.tflite ]; then
    log "Downloading face landmark model..."
    curl -L -o face_landmarker.task \
        "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/latest/face_landmarker.task"

    unzip -o face_landmarker.task "*.tflite" 2>/dev/null || true
    if [ -f face_landmarks_detector.tflite ]; then
        mv face_landmarks_detector.tflite face_landmark.tflite
        log "Extracted face_landmark.tflite"
    else
        LARGEST=$(ls -S *.tflite 2>/dev/null | grep -v drowsiness | head -1)
        if [ -n "$LARGEST" ] && [ "$LARGEST" != "face_landmark.tflite" ]; then
            mv "$LARGEST" face_landmark.tflite
            log "Extracted face_landmark.tflite (from $LARGEST)"
        fi
    fi
    rm -f face_landmarker.task face_blendshapes.tflite 2>/dev/null || true

    if [ ! -s face_landmark.tflite ]; then
        warn "Could not auto-extract face_landmark.tflite"
        warn "Manual: pip3 install mediapipe, then copy the .tflite"
    fi
else
    log "Face landmark model already present"
fi

# Verify drowsiness classifier
if [ -f drowsiness_int8.tflite ]; then
    log "Drowsiness INT8 classifier: $(ls -lh drowsiness_int8.tflite | awk '{print $5}')"
else
    err "drowsiness_int8.tflite not found!"
fi

log "Models summary:"
ls -lh *.tflite *.onnx 2>/dev/null || echo "(no models found)"

# ─────────────────────────────────────────────────────────────────────────────
next_step "Building DMS binary"
# ─────────────────────────────────────────────────────────────────────────────

cd "$PROJECT_DIR"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

log "Binary built: $(pwd)/dms"
ls -lh dms

# ─────────────────────────────────────────────────────────────────────────────
next_step "Deploying to /opt/dms"
# ─────────────────────────────────────────────────────────────────────────────

INSTALL_DIR="/opt/dms"

# Stop existing service if running
systemctl stop dms 2>/dev/null || true

# Create directories
mkdir -p "$INSTALL_DIR/bin"
mkdir -p "$INSTALL_DIR/models"
mkdir -p /var/lib/dms
mkdir -p /tmp/dms
mkdir -p /tmp/dms_images

# Copy binary
cp -v "$PROJECT_DIR/build/dms" "$INSTALL_DIR/bin/dms"
chmod +x "$INSTALL_DIR/bin/dms"

# Copy models
cp -v "$PROJECT_DIR/models/"*.tflite "$INSTALL_DIR/models/" 2>/dev/null || true
cp -v "$PROJECT_DIR/models/"*.onnx "$INSTALL_DIR/models/" 2>/dev/null || true

# Detect boot partition (Bookworm uses /boot/firmware, older uses /boot)
if [ -d /boot/firmware ]; then
    BOOT_DIR="/boot/firmware"
else
    BOOT_DIR="/boot"
fi

# Create default config if not present
if [ ! -f "$BOOT_DIR/dms_config.json" ]; then
    cat > "$BOOT_DIR/dms_config.json" << 'CONFIGEOF'
{
    "frame_w": 320,
    "frame_h": 240,
    "fps": 15,
    "skip_frames": 2,
    "speed_threshold": 0,
    "use_tflite": true
}
CONFIGEOF
    log "Created default $BOOT_DIR/dms_config.json"
fi

log "Deployed to $INSTALL_DIR"

# ─────────────────────────────────────────────────────────────────────────────
next_step "Installing systemd service"
# ─────────────────────────────────────────────────────────────────────────────

cp -v "$PROJECT_DIR/systemd/dms.service" /etc/systemd/system/dms.service
systemctl daemon-reload
systemctl enable dms

log "systemd service installed and enabled"

# ─────────────────────────────────────────────────────────────────────────────
next_step "Verifying installation"
# ─────────────────────────────────────────────────────────────────────────────

echo ""
log "Checking installed files..."
echo "  Binary:  $(ls -lh $INSTALL_DIR/bin/dms 2>/dev/null || echo 'MISSING')"
echo "  Models:  $(ls $INSTALL_DIR/models/ 2>/dev/null | tr '\n' ' ')"
echo "  Config:  $(ls $BOOT_DIR/dms_config.json 2>/dev/null || echo 'MISSING')"
echo "  Service: $(systemctl is-enabled dms 2>/dev/null || echo 'NOT INSTALLED')"
echo "  TFLite:  $(ls -lh /usr/local/lib/libtensorflow-lite.a 2>/dev/null || echo 'NOT BUILT')"

END_TIME=$(date +%s)
ELAPSED=$(( (END_TIME - START_TIME) / 60 ))

echo ""
echo "================================================================"
log "Setup complete! (${ELAPSED} minutes)"
echo "================================================================"
echo ""
echo "  Start:   sudo systemctl start dms"
echo "  Logs:    journalctl -u dms -f"
echo "  Health:  curl http://localhost:8080/health"
echo "  Status:  curl http://localhost:8080/api/status"
echo "  Config:  $BOOT_DIR/dms_config.json"
echo ""
echo "  To start now:  sudo systemctl start dms"
echo ""

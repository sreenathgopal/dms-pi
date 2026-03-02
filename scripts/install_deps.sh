#!/bin/bash
# Install system dependencies for DMS C++ build (Raspberry Pi OS Bookworm)
set -e

echo "=== Installing DMS C++ dependencies ==="

apt-get update
apt-get install -y \
    build-essential cmake pkg-config git \
    libopencv-dev \
    libgpiod-dev \
    libmicrohttpd-dev \
    ffmpeg

echo "=== Dependencies installed ==="
echo "Next: run scripts/build_tflite.sh to build TFLite from source (one-time)"

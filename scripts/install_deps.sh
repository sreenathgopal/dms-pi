#!/bin/bash
# Install system dependencies for DMS C++ build (Ubuntu 24.04 / Raspberry Pi OS)
set -e

echo "=== Installing DMS C++ dependencies ==="

apt-get update
apt-get install -y \
    build-essential cmake pkg-config git \
    libopencv-dev \
    libzmq3-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libgpiod-dev \
    libmicrohttpd-dev

echo "=== Dependencies installed ==="
echo "Next: run scripts/build_tflite.sh to build TFLite from source (one-time)"

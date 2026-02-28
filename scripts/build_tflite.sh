#!/bin/bash
# Build TensorFlow Lite C++ from source (one-time, ~30 min on Pi Zero 2W)
# Installs static lib + headers to /usr/local
set -e

TFLITE_VER="v2.16.1"
BUILD_DIR="/tmp/tflite-build"

echo "=== Building TFLite C++ ${TFLITE_VER} ==="

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
echo "=== Installing TFLite to /usr/local ==="
sudo cp -v libtensorflow-lite.a /usr/local/lib/
sudo mkdir -p /usr/local/include/tensorflow/lite
sudo cp -r ../tensorflow/lite/*.h /usr/local/include/tensorflow/lite/
sudo cp -r ../tensorflow/lite/c /usr/local/include/tensorflow/lite/
sudo cp -r ../tensorflow/lite/core /usr/local/include/tensorflow/lite/
sudo cp -r ../tensorflow/lite/kernels /usr/local/include/tensorflow/lite/
sudo cp -r ../tensorflow/lite/schema /usr/local/include/tensorflow/lite/

# Install flatbuffers headers (TFLite dependency)
if [ -d _deps/flatbuffers-src/include ]; then
    sudo cp -r _deps/flatbuffers-src/include/flatbuffers /usr/local/include/
fi

echo "=== TFLite build complete ==="
echo "Library: /usr/local/lib/libtensorflow-lite.a"
echo "Headers: /usr/local/include/tensorflow/lite/"

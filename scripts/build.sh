#!/bin/bash
# Build DMS C++ binary
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"
mkdir -p build
cd build

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo "=== Build complete ==="
echo "Binary: $(pwd)/dms"
ls -lh dms

#!/bin/bash
# Deploy DMS C++ binary, models, and systemd service
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
INSTALL_DIR="/opt/dms"

echo "=== Deploying DMS C++ ==="

# Stop existing service
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

# Copy default config if not present
if [ ! -f "$BOOT_DIR/dms_config.json" ]; then
    cat > "$BOOT_DIR/dms_config.json" << 'EOF'
{
    "frame_w": 320,
    "frame_h": 240,
    "fps": 15,
    "skip_frames": 2,
    "speed_threshold": 0,
    "use_tflite": true
}
EOF
    echo "Created default $BOOT_DIR/dms_config.json"
fi

# Install systemd service
cp -v "$PROJECT_DIR/systemd/dms.service" /etc/systemd/system/dms.service
systemctl daemon-reload
systemctl enable dms

echo "=== Deploy complete ==="
echo "Start: sudo systemctl start dms"
echo "Logs:  journalctl -u dms -f"

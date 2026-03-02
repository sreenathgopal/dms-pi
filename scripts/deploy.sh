#!/bin/bash
# Deploy DMS dash cam binary, models, and systemd service
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Deploying DMS Dash Cam ==="

# Stop existing services
systemctl stop dms 2>/dev/null || true
systemctl stop video_recorder 2>/dev/null || true
systemctl stop dms_watchdog.timer 2>/dev/null || true

# Create directories
mkdir -p /home/test/recordings
mkdir -p /home/test/dms-pi/alerts
mkdir -p /opt/dms/models

# Copy models to /opt/dms/models (detection pipeline expects them there)
cp -v "$PROJECT_DIR/models/"*.tflite /opt/dms/models/ 2>/dev/null || true
cp -v "$PROJECT_DIR/models/"*.onnx /opt/dms/models/ 2>/dev/null || true

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
    "frame_w": 640,
    "frame_h": 480,
    "fps": 10,
    "skip_frames": 2,
    "use_tflite": true,
    "ring_buffer_seconds": 90,
    "recordings_dir": "/home/test/recordings",
    "alerts_dir": "/home/test/dms-pi/alerts",
    "web_port": 8080
}
EOF
    echo "Created default $BOOT_DIR/dms_config.json"
fi

# Install systemd service (replaces both video_recorder + dms services)
cp -v "$PROJECT_DIR/systemd/dms.service" /etc/systemd/system/dms.service
systemctl daemon-reload
systemctl enable dms

# Disable old services if they exist
systemctl disable video_recorder 2>/dev/null || true
systemctl disable dms_optimized 2>/dev/null || true

echo "=== Deploy complete ==="
echo "Start: sudo systemctl start dms"
echo "Logs:  journalctl -u dms -f"
echo "API:   curl http://localhost:8080/api/status"

#!/bin/bash
# Download face detection and landmark models
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Downloading DMS models ==="

# YuNet face detection ONNX (~400KB)
YUNET_URL="https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
if [ ! -f face_detection_yunet.onnx ]; then
    echo "Downloading YuNet face detector..."
    curl -L -o face_detection_yunet.onnx "$YUNET_URL"
    echo "YuNet: $(ls -lh face_detection_yunet.onnx | awk '{print $5}')"
else
    echo "YuNet already present"
fi

# Face landmarks TFLite (~2.6MB)
# The .task file is a ZIP archive containing the raw TFLite model
if [ ! -f face_landmark.tflite ]; then
    echo "Downloading face landmark model..."
    TASK_URL="https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/latest/face_landmarker.task"
    curl -L -o face_landmarker.task "$TASK_URL"

    # Extract the face_landmark TFLite from the .task bundle (ZIP format)
    if command -v unzip &>/dev/null; then
        unzip -o face_landmarker.task "*.tflite" 2>/dev/null || true
        # The landmark detector is the largest .tflite inside
        if [ -f face_landmarks_detector.tflite ]; then
            mv face_landmarks_detector.tflite face_landmark.tflite
            echo "Extracted face_landmark.tflite from .task bundle"
        else
            # Fallback: find the largest extracted .tflite
            LARGEST=$(ls -S *.tflite 2>/dev/null | grep -v drowsiness | head -1)
            if [ -n "$LARGEST" ] && [ "$LARGEST" != "face_landmark.tflite" ]; then
                mv "$LARGEST" face_landmark.tflite
                echo "Extracted face_landmark.tflite (from $LARGEST)"
            fi
        fi
        # Clean up any other extracted .tflite files we don't need
        rm -f face_blendshapes.tflite 2>/dev/null || true
    else
        echo "ERROR: 'unzip' not found. Install with: sudo apt install unzip"
        echo "Then re-run this script."
    fi

    rm -f face_landmarker.task

    if [ ! -s face_landmark.tflite ]; then
        echo ""
        echo "WARNING: Could not auto-extract face_landmark.tflite."
        echo "Manual option: pip3 install mediapipe && python3 -c \\"
        echo "  \"import mediapipe, os; p=os.path.dirname(mediapipe.__file__); \\"
        echo "  print(os.path.join(p,'modules','face_landmark','face_landmark_with_attention.tflite'))\""
        rm -f face_landmark.tflite 2>/dev/null || true
    fi
else
    echo "Face landmark model already present"
fi

# Verify drowsiness classifier
if [ -f drowsiness_int8.tflite ]; then
    echo "Drowsiness INT8 classifier: $(ls -lh drowsiness_int8.tflite | awk '{print $5}')"
else
    echo "WARNING: drowsiness_int8.tflite not found!"
    echo "Copy from: optimized/models/drowsiness_int8.tflite"
fi

echo ""
echo "=== Models summary ==="
ls -lh *.tflite *.onnx 2>/dev/null || echo "(no models found)"

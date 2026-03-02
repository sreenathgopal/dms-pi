#!/bin/bash
cd /home/test/video_recorder
LD_PRELOAD=/usr/libexec/aarch64-linux-gnu/libcamera/v4l2-compat.so LIBCAMERA_LOG_LEVELS=*:ERROR ./build/video_recorder "$@"

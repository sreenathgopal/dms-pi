#!/bin/bash
# DMS Watchdog — runs every 5 min via cron
# Checks services + memory, restarts if needed

LOG=/tmp/dms-watchdog.log
DATE=$(date '+%Y-%m-%d %H:%M:%S')

RECORDER_ACTIVE=$(systemctl is-active dms-recorder.service)
DETECTOR_ACTIVE=$(systemctl is-active dms-detector.service)

# Get memory stats
MEM_TOTAL=$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo)
MEM_AVAIL=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
SWAP_TOTAL=$(awk '/SwapTotal/ {print int($2/1024)}' /proc/meminfo)
SWAP_FREE=$(awk '/SwapFree/ {print int($2/1024)}' /proc/meminfo)
SWAP_USED=$((SWAP_TOTAL - SWAP_FREE))
MEM_PCT=$((MEM_AVAIL * 100 / MEM_TOTAL))

# Get per-process RSS in MB via ps
REC_KB=$(ps -C video_recorder -o rss= 2>/dev/null | head -1 | tr -d ' ')
DET_KB=$(ps -C python3 -o rss= 2>/dev/null | head -1 | tr -d ' ')
REC_MB=0; [[ "$REC_KB" =~ ^[0-9]+$ ]] && REC_MB=$((REC_KB / 1024))
DET_MB=0; [[ "$DET_KB" =~ ^[0-9]+$ ]] && DET_MB=$((DET_KB / 1024))

# Log status
echo "[$DATE] MEM: ${MEM_AVAIL}MB/${MEM_TOTAL}MB free ${MEM_PCT}% SWAP: ${SWAP_USED}MB | rec=${REC_MB}MB det=${DET_MB}MB | recorder=$RECORDER_ACTIVE detector=$DETECTOR_ACTIVE" >> $LOG

# Check services
if [ "$RECORDER_ACTIVE" != "active" ] || [ "$DETECTOR_ACTIVE" != "active" ]; then
    echo "[$DATE] ACTION: Service down — restarting both" >> $LOG
    systemctl restart dms-recorder.service
    sleep 5
    systemctl restart dms-detector.service
    echo "[$DATE] ACTION: Services restarted" >> $LOG
fi

# Check critical memory (< 5% available)
if [ "$MEM_PCT" -lt 5 ]; then
    echo "[$DATE] ACTION: Critical memory ${MEM_PCT}% — clearing caches and restarting" >> $LOG
    sync
    echo 3 > /proc/sys/vm/drop_caches
    systemctl restart dms-recorder.service
    sleep 5
    systemctl restart dms-detector.service
    echo "[$DATE] ACTION: Caches cleared, services restarted" >> $LOG
fi

# Check high swap (> 2GB used — sign of memory pressure)
if [ "$SWAP_USED" -gt 2048 ]; then
    echo "[$DATE] WARNING: High swap usage ${SWAP_USED}MB" >> $LOG
fi

# Keep log small
tail -200 $LOG > $LOG.tmp && mv $LOG.tmp $LOG

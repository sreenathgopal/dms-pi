#!/bin/bash
# DMS Disk Cleanup — runs every hour via cron
# Deletes oldest recordings when disk usage > 80%

REC_DIR=/home/test/recordings
ALERT_DIR=/home/test/video_recorder/alerts
LOG=/tmp/dms-watchdog.log
DATE=$(date '+%Y-%m-%d %H:%M:%S')
DISK_PCT=$(df /home/test --output=pcent | tail -1 | tr -d ' %')

if [ "$DISK_PCT" -gt 80 ]; then
    # Count files before cleanup
    COUNT=$(ls -1 $REC_DIR/*.avi 2>/dev/null | wc -l)
    # Delete oldest recordings (keep last 12 = 1 hour)
    ls -1t $REC_DIR/*.avi 2>/dev/null | tail -n +13 | xargs rm -f 2>/dev/null
    DELETED=$(($COUNT - $(ls -1 $REC_DIR/*.avi 2>/dev/null | wc -l)))
    echo "[$DATE] CLEANUP: Disk ${DISK_PCT}% — deleted ${DELETED} old recordings" >> $LOG
fi

# Also clean alert clips older than 7 days
find $ALERT_DIR -name '*.avi' -mtime +7 -delete 2>/dev/null

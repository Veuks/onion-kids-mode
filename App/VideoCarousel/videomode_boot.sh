#!/bin/sh
[ -f /mnt/SDCARD/.videocarousel ] || exit 0
looper=/mnt/SDCARD/App/VideoCarousel/video_mode_loop.sh
if [ -f "$looper" ]; then
    exec sh "$looper" run
fi
rm -f /mnt/SDCARD/.videocarousel

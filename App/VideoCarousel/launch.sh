#!/bin/sh
appdir=/mnt/SDCARD/App/VideoCarousel
result=/tmp/videocarousel_ui_result

mkdir -p /mnt/SDCARD/Media/VideoKidsMode/Imgs

rm -f "$result"
"$appdir/bin/videoui" --pick-timer -t "Video play timer" >/tmp/videocarousel_ui.log 2>&1
if [ $? -eq 5 ] && [ "$(sed -n 1p "$result")" = TIMER ]; then
    minutes="$(sed -n 2p "$result")"
else
    exit 0
fi

mkdir -p /mnt/SDCARD/Saves/videocarousel /mnt/SDCARD/.tmp_update/startup
cp "$appdir/videomode_boot.sh" /mnt/SDCARD/.tmp_update/startup/videomode_boot.sh
printf '%s\n' "$minutes" > /mnt/SDCARD/Saves/videocarousel/timer_minutes
touch /mnt/SDCARD/.videocarousel
exec sh "$appdir/video_mode_loop.sh" arm

#!/bin/sh
# Kids Mode boot hook — runs from /mnt/SDCARD/.tmp_update/startup/
# before Onion launches MainUI (see runtime.sh "Startup scripts"). If the
# flag is present this blocks here in the kid launcher loop; MainUI only
# starts after a successful PIN unlock removed the flag.
#
# This file ships in App/KidsMode/ and is copied into
# .tmp_update/startup/ automatically the first time the mode is armed.
#
# Recovery from a computer: delete /mnt/SDCARD/.kidmode (or the copy of
# this file in .tmp_update/startup/) and the device boots normal Onion.

flagfile=/mnt/SDCARD/.kidmode
looper=/mnt/SDCARD/App/KidsMode/kid_mode_loop.sh

[ -f "$flagfile" ] || exit 0

if [ -f "$looper" ]; then
    sh "$looper" run
else
    # App folder missing — fail open so the device stays usable
    rm -f "$flagfile"
fi

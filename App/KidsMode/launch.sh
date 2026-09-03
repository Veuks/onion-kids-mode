#!/bin/sh
# Kids Mode arm app — one tap in Onion's Apps tab arms it and drops
# straight into the kid launcher. Unlock from inside the kid UI by holding
# SELECT+START for 3 seconds and entering the PIN.

progdir="$(CDPATH= cd -- "$(dirname "$0")" > /dev/null 2>&1 && pwd -P)"
cd "$progdir" || exit 1

# Kids Mode normally owns the screen for the whole armed session. If Onion's
# Apps page becomes visible during a framebuffer transition, pressing A must
# never start a second independent loop. Stop the hidden kidui instance so its
# existing shell loop immediately recreates a clean one instead.
if [ -f /mnt/SDCARD/.kidmode ]; then
    if pgrep -f 'kid_mode_loop.sh' > /dev/null 2>&1; then
        killall kidui 2> /dev/null
        exit 0
    fi
    # A stale armed flag after an interrupted process is recoverable without
    # re-running profile or timer setup.
    exec sh ./kid_mode_loop.sh run
fi

exec sh ./kid_mode_loop.sh arm

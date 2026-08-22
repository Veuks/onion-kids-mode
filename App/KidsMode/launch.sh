#!/bin/sh
# Kids Mode arm app — one tap in Onion's Apps tab arms it and drops
# straight into the kid launcher. Unlock from inside the kid UI by holding
# SELECT+START for 3 seconds and entering the PIN.

progdir="$(CDPATH= cd -- "$(dirname "$0")" > /dev/null 2>&1 && pwd -P)"
cd "$progdir" || exit 1

sh ./kid_mode_loop.sh arm

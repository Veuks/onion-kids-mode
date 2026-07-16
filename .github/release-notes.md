Kids Mode now looks and feels like part of Onion OS — every screen renders
through Onion's own theme engine — plus smarter timer controls and a
PIN system that can't lock you out.

## Highlights

- **Native Onion look**: your active theme's background, fonts, and colors
  everywhere — header bar with battery, footer button hints, and an
  Apps-menu-style parent menu with full-width rows
- **Add play time inline**: on the parent menu row, **◀ ▶** picks +5…+50 min
  and **A** applies — with a live preview of what the remaining time becomes
- **Turn off timer**: new parent-menu option that removes any running timer
  entirely for unlimited play (until re-armed or time is added again)
- **Auto power-off**: if the "Time's up!" screen is left alone for 5 minutes,
  the device shuts down cleanly instead of draining the battery overnight
- **PIN improvements**: **A** confirms (up/down changes a digit, left/right
  moves); a wrong PIN lets you retry on the same screen; and the PIN now
  survives app updates via a snapshot in `Saves/kidmode/` — if no PIN exists
  at all, the unlock gesture asks you to set a new one instead of locking
  you out

## Install

1. Download `KidsMode.zip` below
2. Copy the `KidsMode` folder into `App/` on your SD card (so it becomes
   `/App/KidsMode/`)
3. Reboot, then arm from **Apps → Kids Mode**

**Updating from v1.0.0:** replacing the `KidsMode` folder resets the PIN
once (the update-proof snapshot didn't exist yet). If the device was armed,
hold **SELECT+START** and you'll be asked to set a new PIN. From v1.1.0
onward, the PIN survives updates.

Requires **Onion OS 4.3+**. Tested on a Miyoo Mini Plus running Onion
4.4-beta; base Mini and Mini V4 are supported in code but less tested —
reports welcome.

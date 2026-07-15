# Kids Mode for Onion OS

A fullscreen, favorites-only launcher that locks a **Miyoo Mini / Mini+**
running [Onion OS](https://github.com/OnionUI/Onion) so a young child can
use it unsupervised — big box art, one-button play, a PIN-protected exit,
and an optional play timer.

- **Boots straight into a kid-proof carousel** showing only your ★ favorited
  games: box art, big label, left/right to browse, **A** to play.
- **Exiting a game returns to the carousel — never to Onion's menus.**
- **Play timer** (per session, optional): pick 5–50 minutes when arming.
  Countdown shows inside games via RetroArch's on-screen messages (pinned
  during the last 5 minutes); at zero the game is asked to quit gracefully,
  so Onion's auto-save keeps the exact spot.
- **Parent menu** (hold **SELECT+START 3 s** → PIN): exit Kids Mode or add
  play time (shows played / total / remaining).
- **Start over**: **X** on a game asks "Start over?" and launches from the
  beginning without touching in-game saves.
- **MENU button in-game saves and exits** back to the carousel.
- While armed, RetroArch's settings are hidden (kiosk mode) so the in-game
  menu can't change cores/shaders/mappings. Everything is restored on
  unlock.
- Survives reboots; a powered-off-mid-game session resumes on next boot.

## Requirements

- Miyoo Mini or Mini+ running **Onion OS 4.3 or newer**.
- Games your kid should see must be **favorited** (★, press X on a game in
  Onion) and Onion's *auto save & resume* left on (it is by default).

Tested on a Miyoo Mini Plus with Onion 4.4-beta. The Mini V4's 752×560
screen and the base Mini are supported in code but less tested — reports
welcome.

## Install

1. Download `KidsMode.zip` from [Releases](../../releases).
2. Copy the `KidsMode` folder into the `App` folder on your SD card
   (so it becomes `/App/KidsMode/`). Don't replace the `App` folder itself.
3. Reboot. Nothing changes until you arm it.

## Usage

1. **Arm:** Apps tab → **Kids Mode**. First time, set + confirm a 4-digit
   PIN (up/down changes a digit, A moves to the next, START confirms).
2. Pick a session timer (OFF / 5–50 min) — the device switches straight
   into the kid launcher.
3. Hand it over:

   | Button | Action |
   | ------ | ------ |
   | ◀ ▶ | browse favorites |
   | A | play (resumes where the game last stopped) |
   | X | start over (with confirmation; in-game saves untouched) |
   | MENU (in-game) | save and exit back to the carousel |
   | everything else | does nothing — no dead ends |

4. **Parent access:** hold **SELECT+START ~3 s**, enter the PIN →
   *Exit Kids Mode / Add play time / Back*.

Kids Mode stays armed across reboots until you exit it via the PIN.

## PIN reset / recovery

Everything is recoverable from a computer — nothing on the card is moved
or renamed:

- **Forgot the PIN:** edit `App/KidsMode/kidmode.json` to
  `{ "pin_hash": "", "pin_salt": "", "pin_plain": "1234" }` — the new PIN
  is accepted and re-hashed on next use.
- **Force-disarm:** delete the hidden file `/.kidmode` at the SD root →
  next boot is normal Onion.
- **RetroArch settings stuck hidden:** copy
  `Saves/kidmode/retroarch.cfg.backup` over
  `RetroArch/.retroarch/retroarch.cfg`.
- **Uninstall:** delete `/App/KidsMode/`, `/.kidmode` (if present) and
  `/.tmp_update/startup/kidmode_boot.sh`.

Fail-safes: if the launcher binary is missing or crashes repeatedly, Kids
Mode disarms itself and boots normal Onion instead of brick-looping. A log
is written to `.tmp_update/logs/kidmode.log`.

## How it works

Onion runs everything in `.tmp_update/startup/` before launching its main
UI. Kids Mode installs a hook there (automatically, on first arm) that
checks for the `/.kidmode` flag file: when present, it blocks in the kid
launcher loop, so Onion's MainUI simply never starts until a PIN unlock
removes the flag. Games are launched with the exact command format Onion
itself uses (per-game core overrides, play-activity tracking, V4 560p
handling, auto-save/resume all preserved). No Onion binaries or scripts
are modified; the two config files it adjusts while armed
(`retroarch.cfg` kiosk lock, MENU-button keymap) are backed up to
`Saves/kidmode/` and restored on unlock.

A determined child can still force a shutdown with a long power press —
the device just boots back into Kids Mode. Hardening that path would
require a patched `keymon`; it's documented in the source as future work.

## Building from source

`src/kidsMode/kidui.c` builds inside an Onion source tree with the
[miyoomini toolchain](https://hub.docker.com/r/aemiii91/miyoomini-toolchain):

```sh
git clone https://github.com/OnionUI/Onion && cp -r src/kidsMode Onion/src/
docker run --rm -v "$PWD/Onion":/root/workspace aemiii91/miyoomini-toolchain:latest \
  /bin/bash -c "source /root/.bashrc; cd src/kidsMode && make"
```

The GitHub workflow in this repo does the same on every push and attaches
an install zip to tagged releases.

## Credits & license

Built on and for [Onion OS](https://github.com/OnionUI/Onion) and its
common UI infrastructure. The RetroArch kiosk-lock approach was inspired
by [OnionUI/Onion#1910](https://github.com/OnionUI/Onion/pull/1910).
GPL-3.0, same as Onion.

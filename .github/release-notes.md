# Kids Mode v1.4.0

This release introduces tear-free video playback on the Miyoo Mini Plus and
completes the migration to the built-in KidsPlay media engine. It also improves
display stability, carousel presentation and media playback transitions.

## What's new

- Replaced the previous video presentation path with the integrated KidsPlay
  engine and synchronized display output.
- Eliminated horizontal screen tearing during video playback on the Miyoo Mini
  Plus, including during camera pans, flashes and fast scene changes.
- Kept the child-safe playback controls, theme OSD, progress bar, elapsed and
  remaining time, progressive seeking and paused-frame carousel capture.
- Audio-only files continue to display their artwork and title while playing.
- Added media duration to carousel artwork. Durations are cached in the
  background so long files do not slow down folder navigation.
- Improved media startup and shutdown transitions, including cleaner audio and
  stable brightness when entering or leaving playback.
- Improved framebuffer cleanup around the carousel and parent menu to prevent
  stale, inverted or ghosted screens from reappearing.
- Corrected cached artwork orientation after returning from media playback.
- Refined carousel artwork and title spacing while keeping the navigation
  arrows clear.
- Parent-menu timer and profile operations now return to the appropriate Kids
  Mode screen instead of forcing an unnecessary carousel restart.
- After **Time's up!** is unlocked with the parent PIN, the timer is disabled
  and Kids Mode returns to the previous carousel position or parent menu.

## Included features

- Separate Main and Guest environments for favorites, media, game saves,
  playback positions, carousel selections and artwork caches.
- Direct Main/Guest profile switching from the PIN-protected parent menu.
- Shared parent PIN and play timer across both environments.
- Optional Games-only or Videos-only access and independent media-category
  visibility controls.
- Nested media folders, inherited artwork, automatic thumbnail caching and
  paused-frame screenshots.
- Inactivity dimmer, battery display, final-five-minute warning and automatic
  shutdown after the Time's up screen.

## Updating

1. Download `Kids-Mode.zip` from this release.
2. Exit Kids Mode through the parent menu before updating.
3. Copy the included `App` and `Media` folders to the root of the SD card and
   merge them with the existing folders.
4. Keep `Saves/KidsMode`, `Saves/KidsProfile`, `Saves/CurrentProfile`,
   `Saves/MainProfile` and `Saves/GuestProfile`. They contain settings,
   playback history and save data.
5. Reboot or refresh the Apps list, then launch **Kids Mode**.

Requires Onion OS 4.3 or newer. Read the included README for the full controls,
media-folder structure, recommended video encoding and PIN recovery instructions.

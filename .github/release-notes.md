# Kids Mode v1.5.0

This release improves everyday navigation, playback information, audio
stability and system reliability. It also adds a simpler session-start screen
for choosing the Kids Mode profile and play timer before opening the carousel.

## What's new

- Added a combined startup screen for selecting the Main or Guest profile and
  an optional play timer before entering Kids Mode.
- Holding LEFT or RIGHT now auto-scrolls smoothly through game and media
  carousels and timer values at one consistent, faster speed.
- Holding Y during media playback shows the remaining play time first, then
  alternates every second with the battery level. During the final five
  minutes, the battery appears first because the timer is already visible.
- Added the media filename to the playback OSD using the active Onion theme
  font. Long titles are centred above the progress bar and wrap across up to
  four lines while keeping the seek indicators clear.
- The full playback OSD is shown for two seconds when a file starts or resumes.
  Seek indicators now also disappear correctly after two seconds while paused.
- Improved brightness and volume handling so rapid adjustments no longer
  restore an outdated value when switching between controls or profiles.
- Added clearer volume feedback: orange at high volume, red at very high
  volume and purple while muted. Adjusting the level while muted no longer
  turns the sound back on.
- Increased the KidsPlay audio safety buffer and refined audio startup and
  shutdown to reduce crackling and micro-dropouts during demanding playback.
- Corrected unwanted spacing after apostrophes in carousel titles.
- Disabled the in-game MENU long-press action and vibration. A normal short
  press still saves and returns directly to the carousel.
- Prevented concurrent Kids Mode instances from competing for the player or
  framebuffer, improving recovery from interrupted launches and profile
  changes.
- Removed obsolete duplicate runtime files and temporary diagnostic logging.
- Added an included guide for changing or resetting the parent PIN.

## Included features

- Separate Main and Guest environments for favorites, media, game saves,
  playback positions, carousel selections and artwork caches.
- Direct Main/Guest profile switching from the PIN-protected parent menu.
- Shared parent PIN and play timer across both environments.
- Optional Games-only or Videos-only access and independent media-category
  visibility controls.
- Nested media folders, inherited artwork, automatic thumbnail caching and
  paused-frame screenshots.
- Tear-free KidsPlay video output, inactivity dimmer, battery display,
  final-five-minute warning and automatic shutdown after the Time's up screen.

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

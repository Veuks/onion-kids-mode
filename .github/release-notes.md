# Kids Mode v1.3.0

This release makes it possible to switch safely between the Main and Guest
Kids Mode environments without returning to Onion first. It also simplifies
the parent menus and timer controls.

## What's new

- Added **Switch to Main profile** and **Switch to Guest profile** to the
  PIN-protected parent menu.
- Switching profiles keeps Kids Mode open and automatically loads the selected
  profile's favorites, media library, game saves, playback positions, carousel
  selections, artwork cache and preferences.
- Main and Guest game saves remain fully separate under
  `Saves/KidsProfile/Main` and `Saves/KidsProfile/Guest`.
- The parent PIN and active play timer remain shared and are not reset during
  a profile switch.
- The switch option appears only when the other Onion profile is available.
- Onion remains on the selected profile after leaving Kids Mode.

## Parent menu improvements

- Removed the redundant **Back** rows from the main parent menu and the
  **Media folders** submenu. Press **B** to return from either screen.
- Removed the separate **Turn off timer** row.
- The **Add play time** selector now contains `OFF`, followed by `+5 min` to
  `+120 min`. Selecting `OFF` removes the current time limit.
- Fixed the Main/Guest switch labels displaying as blank text or an invalid
  rectangle on the Miyoo Mini.
- Fixed the timer status text overlapping the last parent-menu row.
- Pressing **A** after seeking now clears the seek value and displays only the
  progress bar with elapsed and remaining time.
- MP3 and other audio titles shown below the artwork now use the active Onion
  theme font, matching the battery, time and seek indicators.

## Updating

1. Download `Kids-Mode.zip` from this release.
2. Exit Kids Mode through the parent menu before updating.
3. Copy the included `App` and `Media` folders to the root of the SD card and
   merge them with the existing folders.
4. Keep `Saves/KidsMode`, `Saves/KidsProfile`, `Saves/CurrentProfile`,
   `Saves/MainProfile` and `Saves/GuestProfile`. They contain settings,
   playback history and save data.
5. Reboot or refresh the Apps list, then launch **Kids Mode**.

The first Main/Guest switch may take a few seconds while Onion safely stores
the current profile and loads the other one. Do not power off the console
during this operation.

Requires Onion OS 4.3 or newer. Read the included README for the full controls,
media-folder structure and PIN recovery instructions.

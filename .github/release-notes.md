# Kids Mode

Kids Mode brings favorite games, children's videos and audio together in one
protected Onion-style launcher for Miyoo Mini and Mini Plus.

## Highlights

- Games on the lower section and videos on the upper section, with a smooth
  vertical transition.
- Optional Games-only or Videos-only lock in the PIN-protected parent menu.
- Recursive folders for categories such as Movies, Series, Music and Stories. A opens
  the selected folder and B returns exactly one level.
- Local artwork folders such as `Movies/Imgs` and
  `Series/Ulysses 31/Imgs`. Videos without an exact matching image reuse their
  nearest available folder cover, searching up to the root category, while
  exact movie or episode artwork takes priority. Covers are inherited whether
  stored in the folder's own `Imgs` or in its parent's `Imgs`.
- Every folder level remembers and restores its own last selected item.
- Automatic restoration of the real shutdown state: running content resumes,
  while a carousel shutdown returns to the same selection. Games and Videos
  also keep separate selections while switching between sections.
- Shared play timer, extra-time controls and automatic power-off after the
  Time's up screen, now headed by the Kids Mode banner.
- Simplified video controls with progressive seeking and on-screen seek values.
- Audio playback with artwork, a progress line, elapsed and remaining time.
- Audio display power saving: minimum brightness after 10 seconds, then
  backlight off after another 5 seconds while playback continues. A wakes the
  display without triggering another action.
- Video progress appears while paused or seeking and hides again on playback.
- Paused-frame capture with MENU + X creates or replaces the selected video's
  carousel image without deleting an existing poster.
- ScreenScraper Mix V1 reflection and uniform square video artwork.
- Cinema-poster artwork is recommended for movies; long automatic episode
  and carousel titles are centred and wrap over up to six lines before
  adaptive font sizing begins.

## Install

1. Download `Kids-Mode.zip`.
2. Exit and remove any previous Kids Mode app folder. For a clean start,
   back up and remove the former `Saves/kidmode` folder, but keep
   `Saves/KidsProfile` because it contains the child's game saves.
3. Copy the included `App` and `Media` folders to the SD card root and merge
   them with the existing folders.
4. Add videos and audio under the included `Media/KidsMode` categories.
   Put item-specific covers in the local `Imgs` folder beside the media.
5. Reboot or refresh Apps, then launch **Kids Mode**.

Requires Onion OS 4.3 or newer. Read the included README for controls, nested
folders, local artwork and PIN recovery.

# Kids Mode Deluxe for Onion OS

Kids Mode Deluxe combines games and videos in one simple, protected launcher
for the Miyoo Mini and Miyoo Mini Plus. It is designed for young children:
large artwork, a small set of useful controls, automatic resume, an optional
session timer, and a PIN-protected parent menu.

Games stay on the lower section and videos stay on the upper section. The two
sections use the active Onion theme and slide smoothly between each other.

## Highlights

- One protected app for both favorite games and children's videos.
- Games are taken directly from Onion's Favorites list.
- Videos, movies and one-level series folders are supported.
- Automatic resume follows the real shutdown state:
  - shut down while playing and that game or video resumes;
  - shut down from a carousel and the same section and selection return;
  - switching between Games and Videos restores the last selection on each.
- Video progress is saved regularly and completed videos restart from the
  beginning the next time they are opened.
- Each series folder reopens on its own last selected episode.
- Optional shared session timer from 5 to 120 minutes, or no timer.
- PIN-protected parent menu with extra time, timer removal, exit, and a
  Games-only or Videos-only lock.
- Onion-style artwork, controls, colors and menus.
- ScreenScraper Mix V1-inspired reflection on video artwork for visual
  consistency with game thumbnails.
- Separate child save profile for games; the parent's saves and states are
  restored when leaving the mode.

## Requirements

- Miyoo Mini or Miyoo Mini Plus.
- Onion OS 4.3 or newer.
- At least one game marked as a Favorite in Onion.
- Onion's FFplay binary, included with normal Onion installations.

The current version was tested on real Miyoo Mini Plus hardware. Mini and Mini
V4 display sizes are supported in the code, but feedback is welcome.

## Installation

1. Download `KidsMode-Deluxe.zip` from the latest GitHub Release.
2. Extract the ZIP.
3. Copy the `KidsModeDeluxe` folder into the `App` folder on the SD card. The
   final path must be:

   ```text
   /mnt/SDCARD/App/KidsModeDeluxe/
   ```

4. Add the games intended for the child to Onion's Favorites list.
5. Add videos as described in the Media section below.
6. Reinsert the SD card, refresh the Apps list or reboot, then open
   **Apps → Kids Mode Deluxe**.
7. On first launch, choose and confirm a four-digit PIN, then select a session
   timer or choose `OFF`.

Kids Mode Deluxe remains active after a reboot until it is exited through the
parent menu.

PIN, timer state, selections and playback positions are stored under
`Saves/kidmode`.

## Media layout

Videos belong in:

```text
/mnt/SDCARD/Media/VideoKidsMode/
```

Supported formats are MP4, MKV, AVI, MOV, M4V and WebM. Entries are sorted
alphabetically.

Artwork belongs in the shared `Imgs` folder and must use the same base name as
the video. PNG, JPG and JPEG are accepted:

```text
Media/VideoKidsMode/
├── The Lion King.mp4
├── The Visitors.mkv
└── Imgs/
    ├── The Lion King.png
    └── The Visitors.jpg
```

Artwork is fitted inside a square without stretching. Black side bars are
added when necessary. If no image exists, the app creates a black card with
the filename centred in the current theme's accent color. Automatic card
titles use up to six balanced lines before reducing the font size.

For movies, portrait cinema-poster artwork is recommended: the whole poster
is preserved and the app adds black side bars inside the square carousel tile
when needed. For series, the cleanest result is usually obtained without any
artwork. Kids Mode Deluxe then creates the black card automatically, places
the episode name inside it and keeps the series name below it. If series or
episode artwork is provided, the image is kept untouched and the episode name
is displayed below it.

### Series folders

One folder level is supported:

```text
Media/VideoKidsMode/
├── Ulysses 31/
│   ├── Episode 01.mp4
│   ├── Episode 02.mp4
│   └── Episode 03.mp4
└── Imgs/
    ├── Ulysses 31.png
    └── Episode 02.jpg
```

`Imgs/Ulysses 31.png` is used for the folder and as the default image for all
episodes. An image matching an episode name takes priority. The folder name is
already shown in its artwork, so an illustrated folder uses the path-style
caption `.../Ulysses 31` in the main carousel; a folder without artwork uses
simply `...`. Inside a folder without artwork, the series name moves below the
black card and the selected episode name is drawn inside it. Long episode names
wrap over up to six lines; the font becomes smaller only when six lines
still do not fit. When an image is available, the episode name is displayed
below the image instead. When a series folder is opened again, the carousel
returns to the last episode selected in that folder.

## Carousel controls

| Button | Action |
| --- | --- |
| LEFT / RIGHT | Browse the current section |
| UP | Move from Games to Videos |
| DOWN | Move from Videos to Games |
| A | Launch/resume a game, play/resume a video, or open a series folder |
| X | Restart the selected game or video after confirmation |
| B | Return from a series folder |
| Hold SELECT + START for 3 seconds | Open the PIN screen and parent menu |
| MENU | No action in the carousel |

UP and DOWN are unavailable while a section is locked. DOWN is also disabled
inside a series folder; press B to return to the main video carousel first.

## While playing a game

The game itself keeps its normal controls. A single press of MENU saves and
returns directly to the Kids Mode Deluxe carousel. RetroArch configuration and
dangerous hotkeys are hidden while the mode is active and restored on exit.

Restarting with X skips Onion's automatic resume state but does not erase
normal in-game save data.

## Video player controls

| Button | Action |
| --- | --- |
| A | Resume playback |
| B | Pause playback |
| MENU + LEFT | Rewind progressively |
| MENU + RIGHT | Fast-forward progressively |
| MENU + DOWN | Rewind 10 minutes |
| MENU + UP | Fast-forward 10 minutes |
| MENU alone | Save the position and return to the carousel |
| Other buttons | Ignored by the player |

Holding MENU + LEFT or RIGHT repeats the seek and increases each step from
10 seconds to 1 minute. It remains at one-minute steps no matter how long the
combination is held. A small white indicator appears at the bottom-left when
rewinding and at the bottom-right when moving forward.

Volume and brightness shortcuts remain usable. Releasing MENU after using a
combination does not accidentally leave the video.

## Parent menu

Hold SELECT + START for about three seconds, enter the four-digit PIN, then
choose:

- **Exit Kids Mode Deluxe** — restore Onion and return to its normal interface.
- **Add play time** — add 5 to 120 minutes to the current session.
- **Turn off timer** — continue without a time limit.
- **Games only / Videos only** — lock the section currently being viewed. The
  vertical navigation arrow disappears while the lock is enabled.
- **Back** — return to the child interface.

## Timer and resume behavior

The same timer is shared by games, videos and both carousels. When it reaches
zero, the running content closes and the child sees **Time's up!** and
**See you next time**. If that screen is left untouched for five minutes, the
device powers off cleanly.

The current floor, folder and selection are saved. Video playback is
checkpointed approximately every five seconds. A video that reaches its natural
end has its saved position cleared automatically.

There is no separate auto-resume option: resume behavior always follows the
real state at shutdown.

## PIN reset and recovery

Nothing on the SD card is permanently locked.

- **Forgotten PIN:** edit `App/KidsModeDeluxe/kidmode.json`, clear `pin_hash`
  and `pin_salt`, and set `pin_plain` to a new four-digit PIN. Also delete
  `Saves/kidmode/pin_backup.json` so the previous PIN is not restored.
- **Force exit from a computer:** delete the hidden `/.kidmode` file at the
  root of the SD card, then boot normally.
- **Log file:** `.tmp_update/logs/kidmode.log` records startup and recovery
  information.
- **Interface failure:** after repeated launcher failures, the safety routine
  returns to normal Onion instead of creating a boot loop.

To uninstall, exit through the parent menu first, then remove
`App/KidsModeDeluxe`. The optional saved state can also be removed from
`Saves/kidmode` if it is no longer needed.

## Building from source

Every push starts the GitHub Actions workflow. It cross-compiles
`src/kidsModeDeluxe`, places `kidui` and `libvcinput.so` inside
`App/KidsModeDeluxe/bin`, and uploads the green artifact named
`KidsMode-Deluxe-build`.

Tagged builds additionally create `KidsMode-Deluxe.zip` and attach it to the
GitHub Release.

For a local build, copy `src/kidsModeDeluxe` into an Onion source tree and use
the Miyoo Mini toolchain:

```sh
git clone https://github.com/OnionUI/Onion
cp -r src/kidsModeDeluxe Onion/src/
docker run --rm -v "$PWD/Onion":/root/workspace \
  aemiii91/miyoomini-toolchain:latest \
  /bin/bash -c "source /root/.bashrc; cd src/kidsModeDeluxe && make"
```

## Credits

- Original Kids Mode concept and implementation by Reddit user `u/daverad`.
- Deluxe games-and-videos version maintained by
  [Veuks](https://github.com/Veuks).
- Built on [Onion OS](https://github.com/OnionUI/Onion) and its native UI
  components.
- Video artwork presentation inspired by ScreenScraper Mix V1.
- Developed through an AI-assisted, iterative hardware-testing workflow.

## License

GPL-3.0, matching Onion OS. See [LICENSE](LICENSE).

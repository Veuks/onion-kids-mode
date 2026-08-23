# Kids Mode for Onion OS

Kids Mode combines games, videos and audio in one simple, protected launcher
for the Miyoo Mini and Miyoo Mini Plus. It is designed for young children:
large artwork, a small set of useful controls, automatic resume, an optional
session timer, and a PIN-protected parent menu.

The existing `v1.0.0` release remains published under the former **Kids Mode
Deluxe** name. Its install archive is still named `KidsMode-Deluxe.zip`.
The current source and all future builds use the Kids Mode name and
folders. Kids Mode is treated as a separate, clean installation; no
settings or playback state are imported automatically from the former app.

Games stay on the lower section and videos stay on the upper section. The two
sections use the active Onion theme and slide smoothly between each other.

## Highlights

- One protected app for favorite games, children's videos and audio.
- Games are taken directly from Onion's Favorites list.
- After the Favorites list changes, Kids Mode runs Onion's Favorites repair
  once and sorts the resulting carousel automatically from A to Z.
- Videos, audio files and nested folders for movies, series, cartoons, music
  or stories are supported.
- Automatic resume follows the real shutdown state:
  - shut down while playing and that game or video resumes;
  - shut down from a carousel and the same section and selection return;
  - switching between Games and Videos restores the last selection on each.
- Video progress is saved regularly and completed videos restart from the
  beginning the next time they are opened.
- Every folder level reopens on its own last selected item.
- Optional shared session timer from 5 to 120 minutes, or no timer.
- PIN-protected parent menu with extra time, timer removal, exit, and a
  Games-only or Videos-only lock.
- Onion-style artwork, controls, colors and menus.
- ScreenScraper Mix V1-inspired reflection on video artwork for visual
  consistency with game thumbnails.
- Fully separate Main and Guest kid environments. Each has its own game saves,
  media library, playback positions, carousel selections and artwork cache.
- The same parent PIN and session timer are shared by both environments.

## Requirements

- Miyoo Mini or Miyoo Mini Plus.
- Onion OS 4.3 or newer.
- At least one game marked as a Favorite in Onion.
- Onion's FFplay binary, included with normal Onion installations.

The current version was tested on real Miyoo Mini Plus hardware. Mini and Mini
V4 display sizes are supported in the code, but feedback is welcome.

## Installation

1. Download `Kids-Mode.zip` from a Kids Mode release, or use the
   `Kids-Mode-build` artifact while testing the current source. The
   historical `v1.0.0` release remains available as `KidsMode-Deluxe.zip`.
2. Extract the ZIP.
3. Copy the included `App` and `Media` folders to the root of the SD card,
   merging them with the existing folders. Do not delete other content already
   stored in `Media`. The application path must be:

   ```text
   /mnt/SDCARD/App/KidsMode/
   ```

4. Add the games intended for the child to Onion's Favorites list.
5. Add videos and audio to the included category folders as described in the
   Media section below.
6. Reinsert the SD card, refresh the Apps list or reboot, then open
   **Apps → Kids Mode**.
7. On first launch, choose and confirm a four-digit PIN, then select a session
   timer or choose `OFF`.

Kids Mode remains active after a reboot until it is exited through the
parent menu.

The environment is chosen automatically from the Onion profile used to launch
Kids Mode. PIN and timer state are shared under `Saves/KidsMode`; profile-bound
state is stored under `Saves/KidsMode/Main` or `Saves/KidsMode/Guest`.

### Clean installation and manual file move

Exit the previous mode through its parent menu before replacing it. Back up
the SD card, then remove any former `App/KidsMode`, `App/KidsModeDeluxe` or
`App/SuperKidsMode` folder. Also remove the former `Saves/kidmode` folder:
on the SD card, `kidmode` and `KidsMode` are the same name, so leaving it in
place would reuse the former PIN, timer and state instead of starting clean.

Do **not** remove `Saves/KidsProfile`, `Saves/CurrentProfile`,
`Saves/MainProfile` or `Saves/GuestProfile`. `KidsProfile` contains the
children's game saves. New sessions use `Saves/KidsProfile/Main` or
`Saves/KidsProfile/Guest`, depending on the Onion profile of origin. Existing
saves can be moved there manually; this release does not copy or merge them.

Copy the new `App/KidsMode` folder and launch it once. Kids Mode creates a
fresh `Saves/KidsMode` folder, PIN, timer state and profile-specific playback
history. Move the videos, series folders and `Imgs` folder manually from
whichever former media folder exists into either `Main` or `Guest`:

```text
Media/VideoKidsMode        → Media/KidsMode/Main
Media/SuperKidsMode        → Media/KidsMode/Main
```

Move the **contents**, not the former media folder itself. Nothing is migrated
automatically. Copy other media to `Media/KidsMode/Guest` only if it should be
available when Kids Mode is launched from Onion Guest. The old
`Saves/VideoCarousel` folder is not used and may be removed after testing.
The historical `v1.0.0` GitHub release itself is not changed.

## Media folders and local artwork

Each Onion profile has its own media root:

```text
/mnt/SDCARD/Media/KidsMode/Main/
/mnt/SDCARD/Media/KidsMode/Guest/
```

Launching Kids Mode from Onion Main displays only `Main`; launching it from
Onion Guest displays only `Guest`. The supplied category structure and generic
artwork are included in both. Files placed directly in the older common
`Media/KidsMode` root are intentionally ignored.

Supported video formats are MP4, MKV, AVI, MOV, M4V and WebM. Supported audio
formats are MP3, M4A, AAC, FLAC, OGG, OPUS, WAV and WMA. Media can be organised
through multiple folder levels. A folder appears when it contains at least one
supported file, either directly or in one of its subfolders. Entries inside a
folder are sorted alphabetically. The supplied root categories use the fixed
order Movies, Music, Cartoons, Series and Stories.

### Optimized pre-rotated videos

This performance build expects video files to be physically rotated by **180°**
during encoding. It can then send decoded frames directly to the Miyoo display
without applying FFplay's `hflip,vflip` filter to every frame. In HandBrake,
choose a 180° rotation in the Dimensions settings while doing the normal H.264
encode. Rotate only video files: posters, folder artwork and other carousel
images must remain upright.

Kids Mode also disables FFplay's automatic metadata rotation. A file changed
only through a QuickTime orientation tag will therefore still appear in its
real stored orientation and cannot silently re-enable a playback-time filter.

For best compatibility use MP4, H.264, AAC, constant frame rate and no more
than 640 × 480. Rotate from the original source as part of that encode instead
of re-encoding an already compressed Kids Mode copy. Test one file on the
console before converting a complete library.

The supplied `kidmode.json` enables this direct path with:

```json
"pre_rotated_videos": true
```

Set it to `false` to play conventional upright files. Kids Mode will then
restore the former real-time 180° FFplay filter. This switch does not rotate
posters or existing artwork.

Each directory can have its own `Imgs` folder. An image uses the same base name
as the media file beside it. PNG, JPG and JPEG are accepted. The complete
package includes five ready-to-use category folders with generic 256 × 256
artwork. This organisation is recommended:

```text
Media/KidsMode/Main/
├── Imgs/
│   ├── Movies.png
│   ├── Music.png
│   ├── Cartoons.png
│   ├── Series.png
│   └── Stories.png
├── Movies/
│   ├── The Lion King.mp4
│   ├── The Visitors.mkv
│   └── Imgs/
│       ├── The Lion King.png
│       └── The Visitors.jpg
├── Music/
│   ├── Song 01.mp3
│   ├── Music video 01.mp4
│   └── Imgs/
│       └── Song 01.png
├── Cartoons/
│   ├── Cartoon 01.mp4
│   └── Imgs/
│       └── Cartoon 01.jpg
├── Series/
│   └── Ulysses 31/
│       ├── Episode 01.mp4
│       ├── Episode 02.mp4
│       └── Imgs/
│           ├── Ulysses 31.png
│           └── Episode 02.jpg
└── Stories/
    ├── Story 01.mp3
    └── Imgs/
        └── Story 01.jpg
```

For a folder cover, place an image named after that folder inside its own
`Imgs` directory. For example, `Series/Ulysses 31/Imgs/Ulysses 31.png` is used
for the Ulysses 31 folder in its parent carousel and as fallback artwork for
episodes without their own image. An exact episode image such as
`Imgs/Episode 02.jpg` takes priority. Folder captions show only the folder
name; movie and episode names remain below the selected image.

Artwork fallback follows the folder hierarchy. If the current folder has no
cover, Kids Mode searches its parent folders up to the category at the media
root. For example, content under `Series/Ulysses 31` can fall back to
`Series/Imgs/Series.png` when `Ulysses 31/Imgs/Ulysses 31.png` does not exist.
At every level, a folder cover may also be stored in its parent's `Imgs`
directory, such as `Media/KidsMode/Main/Imgs/Series.png`; both layouts are
inherited.

The same rule can give category folders their own artwork:
`Movies/Imgs/Movies.png`, `Series/Imgs/Series.png`, `Music/Imgs/Music.png` or
`Cartoons/Imgs/Cartoons.png`.
Without one, Kids Mode creates an automatic black folder card. The older
shared `Media/KidsMode/Main/Imgs` layout remains accepted as a fallback, but
local `Imgs` folders take priority and prevent identical filenames in different
categories from sharing the wrong image. The Guest tree follows the same rule.

Artwork is fitted inside a square without stretching, with black side bars
when necessary. Portrait cinema posters are recommended for movies. If no
image exists, the app creates a black card and wraps its title over up to six
balanced lines before reducing the font size.

The first time a new or modified poster is displayed, Kids Mode creates a
device-sized thumbnail under the active profile's
`Saves/KidsMode/Main/artwork_cache` or `Saves/KidsMode/Guest/artwork_cache`.
Later navigation
loads that prepared thumbnail instead of decoding and resizing the original
again. Replacing the source image overwrites the same cached thumbnail instead
of accumulating versions. When updating from an older test build, its former
`v1-`, `v2-` or `v3-` files can be deleted once manually from that cache folder.

Press A to descend into a folder and B to return exactly one level. Every
folder remembers its own last selected video or subfolder, including after a
reboot. Folder nesting is supported up to sixteen levels, which is effectively
unlimited for normal use.

## Carousel controls

| Button | Action |
| --- | --- |
| LEFT / RIGHT | Browse the current section |
| UP | Move from Games to Videos |
| DOWN | Move from Videos to Games |
| A | Launch/resume a game, play/resume a video, or open a folder |
| X | Restart the selected game or video after confirmation |
| Hold Y | Temporarily show the battery level |
| B | Return to the previous folder level |
| Hold SELECT + START for 3 seconds | Open the PIN screen and parent menu |
| MENU | No action in the carousel |

UP and DOWN are unavailable while a section is locked. Floor switching remains
available inside media folders; returning to Videos restores the same folder
and selection.

## While playing a game

The game itself keeps its normal controls. A single press of MENU saves and
returns directly to the Kids Mode carousel. RetroArch configuration and
dangerous hotkeys are hidden while the mode is active and restored on exit.

Restarting with X skips Onion's automatic resume state but does not erase
normal in-game save data.

## Media player controls

| Button | Action |
| --- | --- |
| A | Resume playback; while already playing, show progress for 2 seconds |
| B | Pause playback |
| MENU + LEFT | Rewind progressively |
| MENU + RIGHT | Fast-forward progressively |
| MENU + DOWN | Rewind progressively: 5, then 10 minutes |
| MENU + UP | Fast-forward progressively: 5, then 10 minutes |
| MENU + X + Y while paused | Capture the current frame for the carousel |
| MENU alone | Save the position and return to the carousel |
| Other buttons | Ignored by the player |

Holding MENU + LEFT or RIGHT repeats the seek and increases each step from
10 seconds to 1 minute. It remains at one-minute steps no matter how long the
combination is held. A small white indicator appears at the bottom-left when
rewinding and at the bottom-right when moving forward.

Audio playback shows the selected file's artwork, or the nearest inherited
folder artwork, with a thin progress line and elapsed/remaining time. After
10 seconds without input the display changes to minimum brightness; five
seconds later the backlight turns off while playback and the timer continue.
Press A, B, X, Y, a direction, START, SELECT, MENU, POWER or either volume
button to restore the display. That first press is consumed: it only wakes the
screen and cannot pause, resume, seek, change the volume, leave the player,
open the parent menu or put the console into standby.

During video playback, the same progress line appears while paused, while
seeking and for two seconds after seeking or resuming. Pressing A while the
video is already playing also shows it for two seconds.

Volume and brightness shortcuts remain usable. Releasing MENU after using a
combination does not accidentally leave the video.

To create a carousel image from a video, pause with B and press MENU + X + Y.
Kids Mode saves `Imgs/Video name.bmp` beside that video. A later capture
replaces the BMP. Captured BMP files take priority over supplied PNG/JPG
posters; deleting the BMP restores the former poster or inherited folder
cover.

## Parent menu

Hold SELECT + START for about three seconds, enter the four-digit PIN, then
choose:

- **Exit Kids Mode** — restore Onion and return to its normal interface.
- **Add play time** — add 5 to 120 minutes to the current session.
- **Turn off timer** — continue without a time limit.
- **Games only / Videos only** — lock the section currently being viewed. The
  vertical navigation arrow disappears while the lock is enabled.
- **Back** — return to the child interface.

## Timer and resume behavior

The same timer is shared by games, video, audio and both carousels. When it reaches
zero, the running content closes and the child sees **Time's up!** and
**See you next time**. If that screen is left untouched for five minutes, the
device powers off cleanly.

The current floor, folder and selection are saved. Media playback is
checkpointed approximately every five seconds. A file that reaches its natural
end has its saved position cleared automatically.

There is no separate auto-resume option: resume behavior always follows the
real state at shutdown.

## PIN reset and recovery

Nothing on the SD card is permanently locked.

- **Forgotten PIN:** edit `App/KidsMode/kidmode.json`, clear `pin_hash`
  and `pin_salt`, and set `pin_plain` to a new four-digit PIN. Also delete
  `Saves/KidsMode/pin_backup.json` so the previous PIN is not restored.
- **Force exit from a computer:** delete the hidden `/.kidmode` file at the
  root of the SD card, then boot normally.
- **Log file:** `.tmp_update/logs/kidsmode.log` records startup and recovery
  information.
- **Interface failure:** after repeated launcher failures, the safety routine
  returns to normal Onion instead of creating a boot loop.

To uninstall, exit through the parent menu first, then remove
`App/KidsMode`. The optional saved state can also be removed from
`Saves/KidsMode` if it is no longer needed.

## Building from source

Every push starts the GitHub Actions workflow. It cross-compiles
`src/kidsMode`, places `kidui` and `libvcinput.so` inside
`App/KidsMode/bin`, and uploads the green artifact named
`Kids-Mode-build`.

Tagged builds additionally create `Kids-Mode.zip` and attach it to the
GitHub Release.

For a local build, copy `src/kidsMode` into an Onion source tree and use
the Miyoo Mini toolchain:

```sh
git clone https://github.com/OnionUI/Onion
cp -r src/kidsMode Onion/src/
docker run --rm -v "$PWD/Onion":/root/workspace \
  aemiii91/miyoomini-toolchain:latest \
  /bin/bash -c "source /root/.bashrc; cd src/kidsMode && make"
```

## Credits

- Original Kids Mode concept and implementation by Reddit user `u/daverad`.
- Kids Mode games-and-videos version maintained by
  [Veuks](https://github.com/Veuks).
- Built on [Onion OS](https://github.com/OnionUI/Onion) and its native UI
  components.
- Video artwork presentation inspired by ScreenScraper Mix V1.
- Developed through an AI-assisted, iterative hardware-testing workflow.

## License

GPL-3.0, matching Onion OS. See [LICENSE](LICENSE).

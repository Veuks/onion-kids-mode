# Video Kids Mode V1

Video Kids Mode is a standalone Onion app. Its internal folder remains named
`VideoCarousel` for update compatibility. It deliberately does not change
`App/KidsMode` or `src/kidsMode`.

## Media layout

Copy videos directly into `/mnt/SDCARD/Media/Videos/`. Supported filename
extensions are MP4, MKV, AVI, MOV, M4V and WebM. Optional artwork goes in
`/mnt/SDCARD/Media/Videos/Imgs/` with the exact same basename:

```text
Media/Videos/Toy Story.mp4
Media/Videos/Imgs/Toy Story.png
```

JPG artwork is also accepted. Titles are sorted alphabetically.
When no artwork is available, the carousel displays the movie or folder name
on a black card using the current theme's selected-item color. Inside a series,
this card keeps the folder name while the episode title is shown below it.

## Controls

Carousel: LEFT/RIGHT (and UP/DOWN) select an item. A plays/resumes a video or
opens a series folder. X opens the Restart confirmation for a video. B returns
from a series folder, and MENU is ignored.

Player:

- A: play/resume
- B: pause
- MENU + LEFT/RIGHT: progressive rewind/fast-forward (10 seconds,
  then 1 minute, then 5 minutes while held)
- MENU + UP/DOWN: seek +10/-10 minutes
- MENU alone: save progress and return to the carousel
- every other key: ignored

Playback uses a subtle scanline filter to soften the very sharp handheld LCD
presentation without the performance cost of a Gaussian blur.

## Series folders

Video Kids Mode supports one folder level below `Media/Videos`. A folder that
contains videos appears in the main carousel. A opens it and B returns to the
main carousel. Inside the folder the footer is A: PLAY, X: RESTART, B: BACK.

Put `Imgs/Folder name.jpg` (or PNG/JPEG) next to the other artwork. It is used
for the folder and as the fallback artwork for every episode inside it. An
exact episode artwork name, when present, takes priority.

## Persistent state

`/mnt/SDCARD/Saves/videocarousel/state.json` records `active_floor`,
`active_mode` (`carousel` or `running`) and the last video. Positions are
stored per video under `Saves/videocarousel/positions/`; `active_folder`
restores the current series. Playback is checkpointed
approximately every five seconds.

If the console reboots while the carousel is visible, it restores the last
selection. If it reboots while a video is running, it reopens that video at
the last checkpoint. The timer deadline is also preserved across reboot.

## Build and install

Run the repository's `Build` GitHub Actions workflow. Download its
`VideoKidsMode-app` artifact and copy `VideoCarousel` into the SD card's
`App` folder. Tagged builds also attach `VideoKidsMode-V1.zip` to the release.

The app expects Onion's FFplay at `.tmp_update/bin/ffplay`; if that path is
not present it falls back to the first `ffplay` found in `PATH`.

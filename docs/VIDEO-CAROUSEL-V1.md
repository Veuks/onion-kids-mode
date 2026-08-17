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

## Controls

Carousel: LEFT/RIGHT (and UP/DOWN) select a video, A plays/resumes, X opens
the same Restart confirmation used by Kids Mode, and MENU exits the app.

Player:

- A: play/resume
- B: pause
- MENU + LEFT/RIGHT: seek -/+ 1 minute
- MENU + UP/DOWN: seek +10/-10 minutes
- MENU alone: save progress and return to the carousel
- every other key: ignored

## Persistent state

`/mnt/SDCARD/Saves/videocarousel/state.json` records `active_floor`,
`active_mode` (`carousel` or `running`) and the last video. Positions are
stored per video under `Saves/videocarousel/positions/` and checkpointed
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

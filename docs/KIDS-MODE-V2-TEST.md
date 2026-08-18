# Kids Mode V2 — first hardware test

This V2 combines the protected game launcher and Video Kids Mode in one app.
It is deliberately installed as `App/KidsModeV2` so the existing V1 folders
remain available while the new interface is tested.

## Navigation

- GAMES is the lower floor; VIDEOS is the upper floor.
- LEFT / RIGHT browses the current floor.
- UP from GAMES opens VIDEOS with a vertical slide.
- DOWN from VIDEOS returns to GAMES.
- The artwork and title slide; footer controls, timer and arrows stay fixed.
- A launches a game, plays a video, or opens a series folder.
- X restarts the selected video after confirmation.
- B returns from a series folder.
- Hold SELECT+START for the parent PIN and menu.

The parent menu includes **Lock current floor**. Its green ON/OFF toggle uses
Onion's native toggle widget. When enabled, UP/DOWN cannot change floor and
the vertical arrows are hidden. The floor that was visible when the parent
enabled the option is the one that remains available.

## Media

Videos go in `/mnt/SDCARD/Media/VideoKidsMode`. Covers go in its `Imgs`
folder and use the same base name as the video. PNG, JPG and JPEG are accepted.
One level of series folders is supported.

## Resume and timer

The V2 records the real active state. A shutdown from a running game or video
resumes that content; a shutdown from the carousel returns to the last floor
and selection. Game and video time use the same Kids Mode timer and parent PIN.

## Build

The GitHub workflow builds `src/kidsModeV2`, places `kidui` and
`libvcinput.so` in `App/KidsModeV2/bin`, and uploads the complete `App`
artifact named `KidsMode-V2-build`. Copy its compiled `KidsModeV2` folder to the SD
card's `App` folder, refresh the Apps list, then launch **Kids Mode V2**.

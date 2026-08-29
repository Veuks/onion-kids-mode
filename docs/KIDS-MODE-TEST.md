# Kids Mode — hardware test guide

Kids Mode combines the protected game launcher and the simplified video
player in one app installed as `App/KidsMode`.

For a clean installation, back up and remove the former `Saves/kidmode`
folder before the first launch. Keep `Saves/KidsProfile`, which contains the
child's game saves, and move videos and artwork manually to `Media/KidsMode`.

## Navigation

- Games are below and videos are above.
- LEFT / RIGHT browses the current section.
- UP from Games opens Videos.
- DOWN from Videos returns to Games.
- Select a different item on each section, switch repeatedly between them,
  and verify that each section returns to its own last selected item.
- A launches, plays or opens the selected folder.
- X restarts after confirmation.
- B returns exactly one folder level.
- Hold SELECT + START for three seconds to open the PIN screen.

The parent menu's **Games only / Videos only** toggle locks the visible
section. UP/DOWN and the vertical arrow are hidden while it is enabled.

## Media

Videos and audio go in `/mnt/SDCARD/Media/KidsMode` and can be nested, for example
`Series/Ulysses 31/Episode 01.mp4`. Each directory has its own `Imgs` folder:
`Movies/Imgs/Movie.jpg` or `Series/Ulysses 31/Imgs/Episode 01.png`. An image
named after a directory inside its own `Imgs` folder is used as that folder's
cover and inherited by videos without their own image. An exact movie or
episode image must take priority. Folder captions show only the folder name;
video names remain below the image. If the current folder has no cover, the
nearest parent cover up to the root category must be inherited. Test both
`Folder/Imgs/Folder.png` and `Parent/Imgs/Folder.png`; both placements must be
accepted. PNG, JPG and JPEG are accepted.

Pause a video with B, then press MENU + X. Return to the carousel and verify
that `Imgs/Video name.bmp` shows the captured frame upright. Repeat at another
frame and verify that the same BMP is replaced. Delete the BMP and verify that
the previous PNG/JPG poster or inherited folder cover becomes visible again.
MENU + X while playback is running must not create a capture.

Test MP3 and at least one other audio format. Verify that playback displays
the exact file cover when present and otherwise inherits the nearest folder
cover. The progress line must show elapsed time on the left and remaining time
on the right. After 30 seconds without input, brightness must drop to minimum;
after 15 more seconds the backlight must turn off without stopping playback or
the timer. Test A, B and MENU separately: the screen must return and the first
press must not resume, pause, seek or leave the player. During video playback,
verify that the progress line appears on pause and seek, then disappears on
resume.

When the timer reaches zero, press buttons and open the PIN/parent screens.
The device must still power off five minutes after **Time's up!** first
appeared. Selecting `OFF` or adding play time before the deadline must cancel
that automatic power-off.

Test at least three levels: open `Series`, open `Ulysses 31`, select an
episode, then press B twice. Reopen both folders and verify that each level
restores its own last selection. The downward Games arrow must remain hidden
until the media root is reached. Without artwork, titles use automatic black
cards; portrait cinema posters remain recommended for movies.

## Resume and timer

A shutdown while a game or video is running resumes that content. A shutdown
from a carousel restores the last section, folder and selection. Games and
videos share the same timer and parent PIN. When time expires, verify that the
top band reads **Kids Mode** above the centred `Time's up!` message.
`Time's up!` must remain white with the current theme, while
`See you next time.` is green.

Launch a RetroArch game with five minutes remaining. Verify that RetroArch's
native compact countdown (`5 min`, `4 min`...) stays visible and changes once
per minute.

Launch a Game Boy or Game Boy Color title using Gambatte. Holding R2 must not
enable fast-forward. Launch a system that uses R2 as a normal gameplay button
and verify that its control remains available.

## Build

The workflow builds `src/kidsMode`, places `kidui` and `libvcinput.so`
in `App/KidsMode/bin`, and uploads `Kids-Mode-build`. Install the
compiled `KidsMode` folder from that artifact.

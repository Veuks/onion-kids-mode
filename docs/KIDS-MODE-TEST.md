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

Videos go in `/mnt/SDCARD/Media/KidsMode` and can be nested, for example
`Series/Ulysses 31/Episode 01.mp4`. Each directory has its own `Imgs` folder:
`Films/Imgs/Movie.jpg` or `Series/Ulysses 31/Imgs/Episode 01.png`. An image
named after a directory inside its own `Imgs` folder is used as that folder's
cover and inherited by videos without their own image. An exact movie or
episode image must take priority. Folder captions show only the folder name;
video names remain below the image. PNG, JPG and JPEG are accepted.

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

## Build

The workflow builds `src/kidsMode`, places `kidui` and `libvcinput.so`
in `App/KidsMode/bin`, and uploads `Kids-Mode-build`. Install the
compiled `KidsMode` folder from that artifact.

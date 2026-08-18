# Kids Mode Deluxe — hardware test guide

Kids Mode Deluxe combines the protected game launcher and Video Kids Mode in
one app installed as `App/KidsModeDeluxe`.

## Navigation

- Games are below and videos are above.
- LEFT / RIGHT browses the current section.
- UP from Games opens Videos.
- DOWN from Videos returns to Games.
- A launches, plays or opens a series folder.
- X restarts after confirmation.
- B returns from a series folder.
- Hold SELECT + START for three seconds to open the PIN screen.

The parent menu's **Games only / Videos only** toggle locks the visible
section. UP/DOWN and the vertical arrow are hidden while it is enabled.

## Media

Videos go in `/mnt/SDCARD/Media/VideoKidsMode`. Covers go in its `Imgs`
folder and use the same base name. PNG, JPG and JPEG are accepted. One series
folder level is supported. Without artwork, the series name is shown below
the black card and the episode name is drawn inside it on up to three lines,
with adaptive font sizing for exceptionally long names. When artwork exists,
the main carousel uses `.../Folder name` and the episode name remains below
the image. Portrait cinema posters are recommended for movies; series are
usually clearest without artwork.

## Resume and timer

A shutdown while a game or video is running resumes that content. A shutdown
from a carousel restores the last section, folder and selection. Games and
videos share the same timer and parent PIN.

## Build

The workflow builds `src/kidsModeDeluxe`, places `kidui` and `libvcinput.so`
in `App/KidsModeDeluxe/bin`, and uploads `KidsMode-Deluxe-build`. Install the
compiled `KidsModeDeluxe` folder from that artifact.

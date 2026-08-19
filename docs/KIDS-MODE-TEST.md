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
- A launches, plays or opens a series folder.
- X restarts after confirmation.
- B returns from a series folder.
- Hold SELECT + START for three seconds to open the PIN screen.

The parent menu's **Games only / Videos only** toggle locks the visible
section. UP/DOWN and the vertical arrow are hidden while it is enabled.

## Media

Videos go in `/mnt/SDCARD/Media/KidsMode`. Covers go in its `Imgs`
folder and use the same base name. PNG, JPG and JPEG are accepted. One series
folder level is supported. Without artwork, the series name is shown below
the black card and the episode name is centred inside it on up to six lines
before adaptive font sizing begins. Automatic black cards in the main
carousel use the same layout. When artwork exists,
the main carousel uses `.../Folder name` and the episode name remains below
the image. Portrait cinema posters are recommended for movies; series are
usually clearest without artwork. Leave a series after selecting an episode,
open it again, and verify that the same episode is selected. Repeat with a
second series to confirm that each folder keeps its own selection.

## Resume and timer

A shutdown while a game or video is running resumes that content. A shutdown
from a carousel restores the last section, folder and selection. Games and
videos share the same timer and parent PIN. When time expires, verify that the
top band reads **Kids Mode** above the centred `Time's up!` message.

## Build

The workflow builds `src/kidsMode`, places `kidui` and `libvcinput.so`
in `App/KidsMode/bin`, and uploads `Kids-Mode-build`. Install the
compiled `KidsMode` folder from that artifact.

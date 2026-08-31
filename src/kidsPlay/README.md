# KidsPlay source

KidsPlay is the single SDL2 media engine used by Kids Mode. It is based on
FFplay from FFmpeg 4.4.5 (LGPL-2.1-or-later) and the Miyoo Mini SDL2 backend.
It contains the child-safe controls, playback state, audio artwork screen,
theme OSD, screenshots, dimmer and vsync presentation directly in the player;
no SDL1 preload input shim is used.

`ffplay.c` replaces `fftools/ffplay.c` in an FFmpeg 4.4.5 tree and includes
`kidsplay_ui.inc` from the same directory. `build_ffplay.sh` documents and
reproduces the exact reduced-codec ARM build when `FFMPEG_SRC`, `SDL2_ROOT`
and `ZIG_BIN` are provided.

The release carries the resulting ARM binary and its runtime libraries in
`App/KidsMode`. The GitHub workflow strips the checked-in binary before
creating its artifact.

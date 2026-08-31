#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${FFMPEG_SRC:?Set FFMPEG_SRC to an FFmpeg 4.4.5 source tree}"
: "${SDL2_ROOT:?Set SDL2_ROOT to the Miyoo SDL2 port root}"
: "${ZIG_BIN:?Set ZIG_BIN to a Zig 0.14 executable}"

sdl_include="$SDL2_ROOT/sdl2/include"
sdl_config_include="${SDL2_CONFIG_INCLUDE:-$SDL2_ROOT/../include}"
sdl_lib="$SDL2_ROOT/prebuilt/mini"
sdl_ext_lib="$SDL2_ROOT/examples"
cross_dir="$root/.cross-tools"
mkdir -p "$cross_dir"

cp "$root/ffplay.c" "$FFMPEG_SRC/fftools/ffplay.c"
cp "$root/kidsplay_ui.inc" "$FFMPEG_SRC/fftools/kidsplay_ui.inc"

for tool in gcc ar ranlib nm; do
    wrapper="$cross_dir/arm-linux-gnueabihf-$tool"
    if [ "$tool" = gcc ]; then
        printf '#!/bin/sh\nexec "%s" cc -target arm-linux-gnueabihf "$@"\n' \
            "$ZIG_BIN" > "$wrapper"
    elif [ "$tool" = nm ]; then
        printf '#!/bin/sh\nexec /usr/bin/nm "$@"\n' > "$wrapper"
    else
        printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$ZIG_BIN" "$tool" > "$wrapper"
    fi
    chmod +x "$wrapper"
done

printf '%s\n' '#!/bin/sh' \
    'case "$1" in' \
    '    --version) echo 2.0.22 ;;' \
    '    --cflags) echo "-I${KIDSPLAY_SDL2_INCLUDE} -I${KIDSPLAY_SDL2_CONFIG_INCLUDE}" ;;' \
    '    --libs) echo "-L${KIDSPLAY_SDL2_LIB} -lSDL2 -Wl,--allow-shlib-undefined" ;;' \
    '    *) exit 1 ;;' \
    'esac' > "$cross_dir/arm-linux-gnueabihf-sdl2-config"
chmod +x "$cross_dir/arm-linux-gnueabihf-sdl2-config"

export KIDSPLAY_SDL2_INCLUDE="$sdl_include"
export KIDSPLAY_SDL2_CONFIG_INCLUDE="$sdl_config_include"
export KIDSPLAY_SDL2_LIB="$sdl_lib"

cd "$FFMPEG_SRC"
./configure \
    --enable-cross-compile --cross-prefix="$cross_dir/arm-linux-gnueabihf-" \
    --target-os=linux --arch=arm --cpu=cortex_a7 \
    --enable-neon --disable-runtime-cpudetect \
    --disable-everything --disable-ffmpeg --disable-ffprobe --enable-ffplay \
    --enable-sdl2 --enable-pthreads --enable-avcodec --enable-avformat \
    --enable-avfilter --enable-swscale --enable-swresample --disable-avdevice \
    --disable-network --enable-protocol=file \
    --enable-demuxer=mov,matroska,avi,flv,mpegts,mpegps,mpegvideo,asf,mp3,ogg,wav,aac,flac \
    --enable-decoder=h264,hevc,mpeg4,mpeg2video,mpeg1video,flv,vp8,vp9,theora,mjpeg,wmv1,wmv2,wmv3,vc1,msmpeg4v1,msmpeg4v2,msmpeg4v3,aac,aac_fixed,aac_latm,mp3,mp3float,ac3,eac3,vorbis,opus,flac,alac,wmav1,wmav2,wmapro,pcm_s16le,pcm_s16be,pcm_s24le,pcm_f32le \
    --enable-parser=h264,hevc,mpeg4video,mpegvideo,aac,aac_latm,ac3,mpegaudio,opus,vorbis \
    --enable-filter=crop,transpose,hflip,vflip,rotate,format,scale,pad,aresample \
    --disable-doc --disable-debug --disable-autodetect --disable-stripping \
    --disable-shared --enable-static \
    --extra-cflags="-O3 -g0 -fomit-frame-pointer -I$sdl_include" \
    --extra-ldflags="-L$sdl_lib -L$sdl_ext_lib -Wl,--allow-shlib-undefined" \
    --extra-libs="-lSDL2 $sdl_ext_lib/libSDL2_image-2.0.so.0 $sdl_ext_lib/libSDL2_ttf-2.0.so.0 -lm -ldl -lpthread"

make -j"${JOBS:-4}" ffplay
cp ffplay "$root/kidsplay"

"$ZIG_BIN" cc -target arm-linux-gnueabihf -O2 \
    "$root/fb_reset.c" -o "$root/fb_reset"
"$ZIG_BIN" cc -target arm-linux-gnueabihf -O2 -shared -fPIC \
    "$root/libminivsync.c" -ldl -lpthread -o "$root/libminivsync.so"

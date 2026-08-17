#!/bin/sh
appdir=/mnt/SDCARD/App/VideoCarousel
savedir=/mnt/SDCARD/Saves/videocarousel
state="$savedir/state.json"
positions="$savedir/positions"
remaining=/tmp/videocarousel_remaining
result=/tmp/videocarousel_ui_result
player_pid=/tmp/videocarousel_player.pid
menu_exit_marker=/tmp/videocarousel_menu_exit
ffplay=/mnt/SDCARD/.tmp_update/bin/ffplay
sysdir=/mnt/SDCARD/.tmp_update
miyoodir=/mnt/SDCARD/miyoo
pinconfig=/mnt/SDCARD/App/KidsMode/kidmode.json
pinbackup=/mnt/SDCARD/Saves/kidmode/pin_backup.json

export LD_LIBRARY_PATH="/lib:/config/lib:$miyoodir/lib:$sysdir/lib:$sysdir/lib/parasyte"
export PATH="$sysdir/bin:$PATH"

mkdir -p "$savedir" "$positions" /mnt/SDCARD/Media/Videos/Imgs
[ -x "$ffplay" ] || ffplay="$(command -v ffplay)"

json_get() {
    jq -r "$1 // empty" "$state" 2>/dev/null
}

hash_string() {
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        printf '%s' "$1" | openssl dgst -sha256 2>/dev/null | awk '{print $NF}'
    else
        return 1
    fi
}

verify_parent_pin() {
    entered="$1"
    pinsource="$pinconfig"
    [ -f "$pinsource" ] || pinsource="$pinbackup"
    [ -f "$pinsource" ] || return 1
    plain="$(jq -r '.pin_plain // ""' "$pinsource" 2>/dev/null)"
    [ -n "$plain" ] && [ "$entered" = "$plain" ] && return 0
    stored_hash="$(jq -r '.pin_hash // ""' "$pinsource" 2>/dev/null)"
    salt="$(jq -r '.pin_salt // ""' "$pinsource" 2>/dev/null)"
    [ -n "$stored_hash" ] || return 1
    entered_hash="$(hash_string "${salt}${entered}" 2>/dev/null)"
    [ -n "$entered_hash" ] && [ "$entered_hash" = "$stored_hash" ]
}

stop_ticker() {
    [ -n "$ticker" ] && kill "$ticker" 2>/dev/null
    ticker=""
}

set_timer_seconds() {
    seconds="$1"
    stop_ticker
    if [ "$seconds" -lt 0 ]; then
        printf '0\n' > "$savedir/timer_minutes"
        rm -f "$savedir/timer_end" "$remaining"
        return
    fi
    end=$(( $(date +%s) + seconds ))
    printf '1\n' > "$savedir/timer_minutes"
    printf '%s\n' "$end" > "$savedir/timer_end"
    start_timer run
}

show_parent_menu() {
    current_remaining="$(cat "$remaining" 2>/dev/null)"
    case "$current_remaining" in ''|*[!0-9]*) current_remaining=-1 ;; esac
    rm -f "$result"
    "$appdir/bin/videoui" --parent-menu --remaining "$current_remaining" \
        >/tmp/videocarousel_ui.log 2>&1
    [ "$(sed -n 1p "$result")" = MENU ] || return
    choice="$(sed -n 2p "$result")"
    case "$choice" in
        UNLOCK)
            save_state carousel "$last"
            rm -f /mnt/SDCARD/.videocarousel
            ;;
        ADDTIME)
            add_minutes="$(sed -n 3p "$result")"
            case "$add_minutes" in ''|*[!0-9]*) add_minutes=5 ;; esac
            [ "$current_remaining" -lt 0 ] && current_remaining=0
            set_timer_seconds $((current_remaining + add_minutes * 60))
            ;;
        NOTIMER)
            set_timer_seconds -1
            ;;
    esac
}

save_state() {
    mode="$1" video="$2"
    tmp="$state.tmp"
    jq -n --arg floor videos --arg mode "$mode" --arg video "$video" \
        '{version:1,active_floor:$floor,active_mode:$mode,last_video:$video}' > "$tmp" && mv -f "$tmp" "$state"
    sync
}

video_key() {
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    elif command -v md5sum >/dev/null 2>&1; then
        printf '%s' "$1" | md5sum | awk '{print $1}'
    else
        printf '%s' "$1" | cksum | awk '{print $1}'
    fi
}

restore_ffplay_state() {
    for backup in $(find /mnt/SDCARD/App/FFplay "$sysdir" \
        -name 'pos.cfg.videocarousel-backup' 2>/dev/null); do
        original="${backup%.videocarousel-backup}"
        rm -f "$original"
        mv -f "$backup" "$original"
    done
}

hide_ffplay_state() {
    # The Miyoo FFplay build has used more than one pos.cfg location across
    # releases. Isolate every FFplay-owned copy so its global resume cannot
    # override VideoCarousel's per-video position (especially Restart=0).
    restore_ffplay_state
    for original in $(find /mnt/SDCARD/App/FFplay "$sysdir" \
        -name pos.cfg 2>/dev/null); do
        mv -f "$original" "$original.videocarousel-backup"
    done
}

start_timer() {
    minutes="$(cat "$savedir/timer_minutes" 2>/dev/null)"
    case "$minutes" in ''|*[!0-9]*) minutes=0 ;; esac
    if [ "$minutes" -eq 0 ]; then
        rm -f "$remaining"
        rm -f "$savedir/timer_end"
        return
    fi
    if [ "$1" = run ] && [ -f "$savedir/timer_end" ]; then
        end="$(cat "$savedir/timer_end" 2>/dev/null)"
        case "$end" in ''|*[!0-9]*) end=0 ;; esac
    else
        end=$(( $(date +%s) + minutes * 60 ))
        printf '%s\n' "$end" > "$savedir/timer_end"
    fi
    (
        while [ -f /mnt/SDCARD/.videocarousel ]; do
            left=$((end - $(date +%s)))
            [ "$left" -lt 0 ] && left=0
            printf '%s\n' "$left" > "$remaining"
            if [ "$left" -eq 0 ]; then
                [ -f "$player_pid" ] && kill "$(cat "$player_pid")" 2>/dev/null
                break
            fi
            sleep 1
        done
    ) &
    ticker=$!
}

play_video() {
    video="$1" fresh="$2"
    [ -f "$video" ] || return
    key="$(video_key "$video")"
    posfile="$positions/$key.pos"
    start=0
    if [ "$fresh" != yes ] && [ -f "$posfile" ]; then
        start="$(cat "$posfile" 2>/dev/null)"
        case "$start" in ''|*[!0-9]*) start=0 ;; esac
    fi
    [ "$fresh" = yes ] && printf '0\n' > "$posfile"
    save_state running "$video"
    hide_ffplay_state
    rm -f "$menu_exit_marker"
    echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
    touch /tmp/stay_awake
    cd "$sysdir" || return
    VC_START_SECONDS="$start" VC_POSITION_FILE="$posfile" \
      LD_PRELOAD="$appdir/bin/libvcinput.so${LD_PRELOAD:+:$LD_PRELOAD}" \
      "$ffplay" -autoexit -vf "hflip,vflip" -ss "$start" -i "$video" &
    pid=$!
    printf '%s\n' "$pid" > "$player_pid"
    wait "$pid"
    player_status=$?
    rm -f /tmp/stay_awake
    # Delete the temporary resume state produced by this playback, then put
    # back the standard FFplay app's own state exactly as it was.
    find /mnt/SDCARD/App/FFplay "$sysdir" -name pos.cfg -exec rm -f {} \; 2>/dev/null
    restore_ffplay_state
    rm -f "$player_pid"
    timer_left="$(cat "$remaining" 2>/dev/null)"
    case "$timer_left" in ''|*[!0-9]*) timer_left=-1 ;; esac
    if [ -f "$menu_exit_marker" ] || [ "$player_status" -eq 0 ] || \
       [ "$timer_left" -eq 0 ]; then
        rm -f "$menu_exit_marker"
        save_state carousel "$video"
        return 0
    fi

    # FFplay was terminated externally while time remained: this is the
    # console shutdown path. Keep active_mode=running for boot resume and do
    # not redraw the carousel while the shutdown screen is taking over.
    sync
    sleep 5
    return 2
}

start_timer "$1"
last="$(json_get '.last_video')"
pin_notice=""
if [ "$1" = run ] && [ "$(json_get '.active_mode')" = running ] && [ -n "$last" ]; then
    play_video "$last" no
    [ $? -eq 2 ] && exit 0
fi

while [ -f /mnt/SDCARD/.videocarousel ]; do
    rm -f "$result"
    if [ -n "$pin_notice" ] && [ -n "$last" ]; then
        "$appdir/bin/videoui" --select "$last" --start-pin \
            --notice "$pin_notice" >/tmp/videocarousel_ui.log 2>&1
    elif [ -n "$pin_notice" ]; then
        "$appdir/bin/videoui" --start-pin --notice "$pin_notice" \
            >/tmp/videocarousel_ui.log 2>&1
    elif [ -n "$last" ]; then
        "$appdir/bin/videoui" --select "$last" >/tmp/videocarousel_ui.log 2>&1
    else
        "$appdir/bin/videoui" >/tmp/videocarousel_ui.log 2>&1
    fi
    action="$(sed -n 1p "$result")"
    video="$(sed -n 2p "$result")"
    case "$action" in
        PLAY)
            last="$video"
            play_video "$video" no
            [ $? -eq 2 ] && break
            ;;
        RESTART)
            last="$video"
            play_video "$video" yes
            [ $? -eq 2 ] && break
            ;;
        PIN)
            if verify_parent_pin "$video"; then
                pin_notice=""
                show_parent_menu
            else
                pin_notice="Wrong PIN - try again"
            fi
            ;;
        POWEROFF) poweroff ;;
        *) break ;;
    esac
done

[ -n "$ticker" ] && kill "$ticker" 2>/dev/null
rm -f "$remaining" "$player_pid"

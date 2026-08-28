#!/bin/sh
# ---------------------------------------------------------------------------
# Kids Mode for Onion OS — arming, play loop, and unlock logic.
#
# The device is locked to a fullscreen favorites-only launcher (kidui).
# Exiting a game always returns to the launcher, never to MainUI.
#
# Usage:
#   kid_mode_loop.sh arm    arm Kids Mode (first run asks to set a PIN),
#                           then enter the loop; called by the Apps-tab app
#   kid_mode_loop.sh run    enter the loop if armed; called by the startup
#                           hook (.tmp_update/startup/kidmode_boot.sh)
#
# Mode flag: /mnt/SDCARD/.kidmode  (present = armed; delete it from a
# computer to force-disable Kids Mode)
#
# Kids Mode runs its own minimally patched keymon while armed. Onion's binary
# is never overwritten and is restarted immediately when Kids Mode exits.
# The patch keeps Onion's normal POWER behaviour, expands reliable process
# suspension and blanks the backlight before any framebuffer can flash.
# ---------------------------------------------------------------------------

sysdir=/mnt/SDCARD/.tmp_update
miyoodir=/mnt/SDCARD/miyoo
appdir=/mnt/SDCARD/App/KidsMode

kidui_bin="$appdir/bin/kidui"
kids_keymon_bin="$appdir/bin/keymon"
onion_keymon_bin="$sysdir/bin/keymon"
configfile="$appdir/kidmode.json"
flagfile=/mnt/SDCARD/.kidmode
favfile=/mnt/SDCARD/Roms/favourite.json
# Backups and state live OUTSIDE the app folder so that replacing
# App/KidsMode during an update can never delete them.
backupdir=/mnt/SDCARD/Saves/KidsMode
source_profile_file="$backupdir/source_profile.txt"

# Main and Guest each get an entirely separate kid environment. When Kids
# Mode is already armed (including after a reboot), the persisted origin is
# authoritative because Onion's profile folders are temporarily parked.
detect_source_profile() {
    if [ -f "$flagfile" ] && [ -f "$source_profile_file" ]; then
        persisted_profile="$(sed -n 1p "$source_profile_file" 2> /dev/null)"
        case "$persisted_profile" in
            Main | Guest)
                printf '%s\n' "$persisted_profile"
                return 0
                ;;
        esac
    fi
    # Onion parks MainProfile while Guest is active, and GuestProfile while
    # Main is active. A device that never enabled Guest Mode is Main.
    if [ -d /mnt/SDCARD/Saves/MainProfile ]; then
        printf 'Guest\n'
    else
        printf 'Main\n'
    fi
}

source_profile="$(detect_source_profile)"
profile_state_dir="$backupdir/$source_profile"
profile_preferences_file="$profile_state_dir/preferences.json"
favorites_signature_file="$profile_state_dir/favorites_repaired.cksum"

racfg=/mnt/SDCARD/RetroArch/.retroarch/retroarch.cfg
rabackup="$backupdir/retroarch.cfg.backup"
keymapcfg=/mnt/SDCARD/.tmp_update/config/keymap.json
keymapbackup="$backupdir/keymap.json.backup"
keymapnone="$backupdir/keymap-was-absent"
blfscript=/mnt/SDCARD/.tmp_update/script/blue_light.sh
blfbackup="$backupdir/blue_light.sh.backup"
current_profile=/mnt/SDCARD/Saves/CurrentProfile
kids_profile="/mnt/SDCARD/Saves/KidsProfile/$source_profile"
isolated_subdirs="saves states romScreens"
profile_isolation_marker="$backupdir/profile_isolation_active"
game_environment_marker="$backupdir/game_environment_ready"
game_prepare_pid_file=/tmp/kidsmode_game_prepare.pid
last_game_file="$profile_state_dir/last_game.txt"
logfile=/mnt/SDCARD/.tmp_update/logs/kidsmode.log

timer_state="$backupdir/timer_state.txt" # 3 lines: day / used seconds / bonus seconds
# The PIN also lives in kidmode.json inside the app folder, which an app
# update replaces. Keep a copy outside so updating while armed can't cause
# a lockout (see restore_pin_backup).
pin_backup="$backupdir/pin_backup.json"
remaining_file=/tmp/kidsmode_remaining
timesup_since_file=/tmp/kidsmode_timesup_since
timer_minutes_file=/tmp/kidsmode_timer_minutes
ticker_pid_file=/tmp/kidmode_ticker.pid

# kidui reports results via this file, NOT stdout — the device's SDL/driver
# stack prints noise on stdout, which broke first-line parsing on hardware.
uiresult=/tmp/kidsmode_ui_result
lockfloor_result=/tmp/kidsmode_lockfloor_result
categories_result=/tmp/kidsmode_categories_result
uilog=/tmp/kidmode_ui_log

# Unified video state. Game state continues to use Onion's native
# cmd_to_run.sh and last_game.txt; this file remembers the active floor,
# folder, selected video and whether playback was running at shutdown.
videosdir="/mnt/SDCARD/Media/KidsMode/$source_profile"
statefile="$profile_state_dir/state.json"
positions="$profile_state_dir/video_positions"
ffplay=/mnt/SDCARD/.tmp_update/bin/ffplay
player_pid=/tmp/kidsmode_player.pid
menu_exit_marker=/tmp/kidsmode_video_menu_exit
libvcinput="$appdir/bin/libvcinput.so"
brightness_pwm=/sys/devices/soc0/soc/1f003400.pwm/pwm/pwmchip0/pwm0/duty_cycle
game_selection_file="$profile_state_dir/game_selection.txt"
video_selection_file="$profile_state_dir/video_selection.txt"
last_artwork_file="$profile_state_dir/last_artwork.txt"
# Keep the established on-card directory name so this update does not create
# a duplicate beside existing per-series selections. It now stores the last
# selected item for every media folder, not only series.
folder_selections_dir="$profile_state_dir/series_selections"
folder_selections_index="$folder_selections_dir/selections.tsv"
folder_history_file=/tmp/kidsmode_folder_history
artwork_cache_dir="$profile_state_dir/artwork_cache"

export LD_LIBRARY_PATH="/lib:/config/lib:$miyoodir/lib:$sysdir/lib:$sysdir/lib/parasyte"
export PATH="$sysdir/bin:$PATH"
export KIDSMODE_MEDIA_DIR="$videosdir"
export KIDSMODE_FOLDER_SELECTIONS_DIR="$folder_selections_dir"
export KIDSMODE_ARTWORK_CACHE_DIR="$artwork_cache_dir"

mkdir -p "$backupdir" "$profile_state_dir" "$positions" \
    "$folder_selections_dir" "$artwork_cache_dir" "$videosdir/Imgs"
[ -x "$ffplay" ] || ffplay="$(command -v ffplay)"
timer_state_date="$(date +%Y-%m-%d)"

cfg_pin_hash=""
cfg_pin_salt=""
cfg_pin_plain=""
cfg_timer_minutes=0
cfg_lock_current_floor=false
cfg_show_stories=true
cfg_show_movies=true
cfg_show_series=true
cfg_show_music=true
cfg_show_cartoons=true
cfg_fav_shortcut=false

log() {
    mkdir -p "$(dirname "$logfile")"
    echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$logfile"
}

# --------------------------- PIN handling ----------------------------------

hash_string() {
    if command -v sha256sum > /dev/null 2>&1; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    elif command -v openssl > /dev/null 2>&1; then
        printf '%s' "$1" | openssl dgst -sha256 2> /dev/null | awk '{print $NF}'
    else
        return 1
    fi
}

make_salt() {
    if [ -r /dev/urandom ]; then
        dd if=/dev/urandom bs=8 count=1 2> /dev/null | od -An -tx1 | tr -d ' \n'
    else
        printf '%s' "$(date +%s)$$"
    fi
}

load_config_cache() {
    [ -f "$configfile" ] || return 1
    config_dump="$(jq -r '
        (.pin_hash // ""),
        (.pin_salt // ""),
        (.pin_plain // ""),
        ((.timer_minutes // 0) | tostring),
        ((.lock_current_floor // false) | tostring),
        ((if has("show_stories") then .show_stories else true end) | tostring),
        ((if has("show_movies") then .show_movies else true end) | tostring),
        ((if has("show_series") then .show_series else true end) | tostring),
        ((if has("show_music") then .show_music else true end) | tostring),
        ((if has("show_cartoons") then .show_cartoons else true end) | tostring),
        ((.fav_shortcut // false) | tostring)
    ' "$configfile" 2> /dev/null)" || return 1
    {
        IFS= read -r cfg_pin_hash
        IFS= read -r cfg_pin_salt
        IFS= read -r cfg_pin_plain
        IFS= read -r cfg_timer_minutes
        IFS= read -r cfg_lock_current_floor
        IFS= read -r cfg_show_stories
        IFS= read -r cfg_show_movies
        IFS= read -r cfg_show_series
        IFS= read -r cfg_show_music
        IFS= read -r cfg_show_cartoons
        IFS= read -r cfg_fav_shortcut
    } <<EOF
$config_dump
EOF
    case "$cfg_timer_minutes" in
        '' | *[!0-9]*) cfg_timer_minutes=0 ;;
    esac
    if [ -f "$profile_preferences_file" ] &&
        jq -e . "$profile_preferences_file" > /dev/null 2>&1; then
        profile_dump="$(jq -r '
            ((.lock_current_floor // false) | tostring),
            ((if has("show_stories") then .show_stories else true end) | tostring),
            ((if has("show_movies") then .show_movies else true end) | tostring),
            ((if has("show_series") then .show_series else true end) | tostring),
            ((if has("show_music") then .show_music else true end) | tostring),
            ((if has("show_cartoons") then .show_cartoons else true end) | tostring)
        ' "$profile_preferences_file" 2> /dev/null)"
        {
            IFS= read -r cfg_lock_current_floor
            IFS= read -r cfg_show_stories
            IFS= read -r cfg_show_movies
            IFS= read -r cfg_show_series
            IFS= read -r cfg_show_music
            IFS= read -r cfg_show_cartoons
        } <<EOF
$profile_dump
EOF
    fi
    printf '%s\n' "$cfg_timer_minutes" > "$timer_minutes_file"
}

category_enabled() {
    case "$1" in
        stories) [ "$cfg_show_stories" != false ] ;;
        movies) [ "$cfg_show_movies" != false ] ;;
        series) [ "$cfg_show_series" != false ] ;;
        music) [ "$cfg_show_music" != false ] ;;
        cartoons) [ "$cfg_show_cartoons" != false ] ;;
        *) return 1 ;;
    esac
}

refresh_category_values() {
    show_stories_value=0
    show_movies_value=0
    show_series_value=0
    show_music_value=0
    show_cartoons_value=0
    category_enabled stories && show_stories_value=1
    category_enabled movies && show_movies_value=1
    category_enabled series && show_series_value=1
    category_enabled music && show_music_value=1
    category_enabled cartoons && show_cartoons_value=1
}

normalize_active_media_folder() {
    case "$active_folder" in
        "$videosdir"/*)
            relative_folder="${active_folder#"$videosdir"/}"
            root_folder="${relative_folder%%/*}"
            case "$root_folder" in
                [Ss][Tt][Oo][Rr][Ii][Ee][Ss])
                    category_enabled stories || active_folder="" ;;
                [Mm][Oo][Vv][Ii][Ee][Ss])
                    category_enabled movies || active_folder="" ;;
                [Ss][Ee][Rr][Ii][Ee][Ss])
                    category_enabled series || active_folder="" ;;
                [Mm][Uu][Ss][Ii][Cc])
                    category_enabled music || active_folder="" ;;
                [Cc][Aa][Rr][Tt][Oo][Oo][Nn][Ss])
                    category_enabled cartoons || active_folder="" ;;
            esac
            ;;
    esac
}

is_4_digits() {
    case "$1" in
        [0-9][0-9][0-9][0-9]) return 0 ;;
        *) return 1 ;;
    esac
}

ensure_config() {
    if [ ! -f "$configfile" ] || ! jq -e . "$configfile" > /dev/null 2>&1; then
        if [ -f "$configfile" ]; then
            mkdir -p "$backupdir"
            cp "$configfile" "$backupdir/kidmode.json.broken" 2> /dev/null
            log "kidmode.json had invalid JSON; reset to defaults. Broken copy saved to $backupdir/kidmode.json.broken — check it for a missing/extra comma."
        fi
        printf '{\n    "pin_hash": "",\n    "pin_salt": "",\n    "pin_plain": ""\n}\n' > "$configfile"
    fi
}

config_merge() {
    # $1 = jq filter mutating the config; keeps all other keys intact
    ensure_config
    tmpcfg=/tmp/kidmode_config.$$
    # Atomic rename is enough here. A global SD-card sync can take more than
    # 20 seconds and used to freeze the final timer screen before the
    # carousel appeared. Critical PIN data is separately backed up below.
    if jq "$@" "$configfile" > "$tmpcfg" && mv -f "$tmpcfg" "$configfile"; then
        load_config_cache
    fi
}

profile_config_merge() {
    # Parent-menu display choices belong to the originating Onion profile.
    tmpprefs=/tmp/kidsmode_preferences.$$
    if [ -f "$profile_preferences_file" ] &&
        jq -e . "$profile_preferences_file" > /dev/null 2>&1; then
        prefs_source="$profile_preferences_file"
    else
        prefs_source=/tmp/kidsmode_empty_preferences.$$
        jq -n --argjson lock "$cfg_lock_current_floor" \
            --argjson stories "$cfg_show_stories" \
            --argjson movies "$cfg_show_movies" \
            --argjson series "$cfg_show_series" \
            --argjson music "$cfg_show_music" \
            --argjson cartoons "$cfg_show_cartoons" \
            '{lock_current_floor: $lock, show_stories: $stories,
              show_movies: $movies, show_series: $series,
              show_music: $music, show_cartoons: $cartoons}' > "$prefs_source"
    fi
    if jq "$@" "$prefs_source" > "$tmpprefs" &&
        mv -f "$tmpprefs" "$profile_preferences_file"; then
        load_config_cache
    else
        rm -f "$tmpprefs"
    fi
    [ "$prefs_source" = "$profile_preferences_file" ] || rm -f "$prefs_source"
}

store_pin() {
    new_pin="$1"
    salt="$(make_salt)"
    hash="$(hash_string "${salt}${new_pin}" 2> /dev/null || true)"
    if [ -n "$hash" ]; then
        config_merge --arg h "$hash" --arg s "$salt" \
            '.pin_hash = $h | .pin_salt = $s | .pin_plain = ""'
    else
        # No hashing tool available — plaintext fallback (threat model: child)
        config_merge --arg p "$new_pin" \
            '.pin_hash = "" | .pin_salt = "" | .pin_plain = $p'
    fi
    backup_pin
    log "PIN updated."
}

# Snapshot the PIN fields outside the app folder, so replacing App/KidsMode
# (an update) while armed can't lose the PIN.
backup_pin() {
    [ -f "$configfile" ] || return 1
    mkdir -p "$backupdir"
    if jq '{pin_hash: (.pin_hash // ""), pin_salt: (.pin_salt // ""), pin_plain: (.pin_plain // "")}' \
        "$configfile" > "$pin_backup.tmp" 2> /dev/null; then
        mv -f "$pin_backup.tmp" "$pin_backup"
    else
        rm -f "$pin_backup.tmp"
        return 1
    fi
}

# The config has no PIN (fresh kidmode.json after an app update): bring it
# back from the snapshot in Saves/KidsMode. Returns 0 if a PIN is on file
# afterwards.
restore_pin_backup() {
    [ -f "$pin_backup" ] || return 1
    bk_hash="$(jq -r '.pin_hash // ""' "$pin_backup" 2> /dev/null)"
    bk_salt="$(jq -r '.pin_salt // ""' "$pin_backup" 2> /dev/null)"
    bk_plain="$(jq -r '.pin_plain // ""' "$pin_backup" 2> /dev/null)"
    if [ -n "$bk_hash" ] || is_4_digits "$bk_plain"; then
        config_merge --arg h "$bk_hash" --arg s "$bk_salt" --arg p "$bk_plain" \
            '.pin_hash = $h | .pin_salt = $s | .pin_plain = $p'
        log "PIN restored from $pin_backup (app folder replaced?)."
        hash_plain_pin
        has_pin
        return $?
    fi
    return 1
}

has_pin() {
    [ -n "$cfg_pin_hash" ] && return 0
    is_4_digits "$cfg_pin_plain"
}

# If the parent wrote a plaintext PIN into kidmode.json, hash it in place.
hash_plain_pin() {
    plain="$cfg_pin_plain"
    if is_4_digits "$plain"; then
        store_pin "$plain"
    fi
}

verify_pin() {
    entered="$1"
    is_4_digits "$entered" || return 1

    stored_plain="$cfg_pin_plain"
    if is_4_digits "$stored_plain" && [ "$entered" = "$stored_plain" ]; then
        return 0
    fi

    stored_hash="$cfg_pin_hash"
    stored_salt="$cfg_pin_salt"
    if [ -n "$stored_hash" ]; then
        entered_hash="$(hash_string "${stored_salt}${entered}" 2> /dev/null || true)"
        [ -n "$entered_hash" ] && [ "$entered_hash" = "$stored_hash" ] && return 0
    fi

    return 1
}

run_pin_entry() {
    # $1 = title, $2 = optional notice shown under the PIN boxes;
    # echoes the PIN on success
    rm -f "$uiresult"
    if [ -n "$2" ]; then
        "$kidui_bin" --set-pin -t "$1" --notice "$2" > "$uilog" 2>&1
    else
        "$kidui_bin" --set-pin -t "$1" > "$uilog" 2>&1
    fi
    [ $? -eq 3 ] || return 1
    [ "$(sed -n 1p "$uiresult")" = "PIN" ] || return 1
    entered="$(sed -n 2p "$uiresult")"
    rm -f "$uiresult"
    is_4_digits "$entered" || return 1
    printf '%s\n' "$entered"
}

ensure_pin() {
    hash_plain_pin
    has_pin || restore_pin_backup
    if has_pin; then
        [ -f "$pin_backup" ] || backup_pin
        return 0
    fi

    # First-time setup; a mismatch retries in place (B cancels)
    setup_notice=""
    while :; do
        pin1="$(run_pin_entry "Set parent PIN" "$setup_notice")" || return 1
        pin2="$(run_pin_entry "Confirm PIN")" || return 1
        if [ "$pin1" = "$pin2" ]; then
            store_pin "$pin1"
            return 0
        fi
        setup_notice="PINs did not match - try again"
    done
}

# ----------------------- RetroArch kiosk lock ------------------------------
# While armed, hide RetroArch's settings so the in-game menu can't be used to
# change cores, shaders, mappings, etc. Restored from backup on unlock.
# (Approach borrowed from OnionUI PR #1910.)

apply_ra_lock() {
    [ -f "$racfg" ] || return 0
    mkdir -p "$backupdir"
    if [ ! -f "$rabackup" ]; then
        cp "$racfg" "$rabackup"
    fi

    # Parse each RetroArch line's key once, then look it up in an awk array.
    # Do not build a variable regular expression for every key on every line:
    # BusyBox awk recompiles those expressions and made this small rewrite
    # surprisingly expensive. Every duplicate occurrence is replaced too,
    # because RetroArch honours the last value found in the file.
    #
    #   kiosk_mode_enable true — locks down the in-game quick menu
    #   video_font_enable true — timer countdown arrives via RA's OSD
    #     (SHOW_MSG), so on-screen notifications must stay on
    #   quick_menu_show_* false — hide options/cheats/shaders/record/stream
    #     from the (already locked-down) quick menu
    #   settings_show_* false — hide every settings category
    #   input_*_btn nul — every documented RetroArch hotkey action,
    #     disabled. MENU is input_enable_hotkey_btn, held with another
    #     button: this covers MENU+SELECT (open RA's menu), MENU+L2/R2
    #     (save/load state), MENU+L/R (rewind/fast-forward), MENU+LEFT/
    #     RIGHT (save-slot change), MENU+START (fullscreen), and every
    #     other hotkey RetroArch documents — even ones not expected by
    #     default, so nothing is left reachable via MENU+<button>. The
    #     individual actions are cleared rather than input_enable_hotkey_btn
    #     itself, because unbinding the enable button would make each of
    #     these fire on a single un-combo'd press instead. MENU+VOLUME for
    #     brightness is handled outside RetroArch (by the system's button
    #     daemon) and is unaffected by any of this.
    ra_keys="kiosk_mode_enable video_font_enable quick_menu_show_options
        quick_menu_show_cheats quick_menu_show_shaders
        quick_menu_show_start_recording quick_menu_show_start_streaming
        settings_show_configuration settings_show_core
        settings_show_directory settings_show_drivers
        settings_show_file_browser settings_show_input
        settings_show_latency settings_show_network settings_show_recording
        settings_show_user settings_show_user_interface settings_show_video
        settings_show_audio input_menu_toggle_btn input_save_state_btn
        input_load_state_btn input_rewind_btn input_toggle_fast_forward_btn
        input_hold_fast_forward_btn input_state_slot_increase_btn
        input_state_slot_decrease_btn input_toggle_fullscreen_btn
        input_shader_toggle_btn input_shader_next_btn input_shader_prev_btn
        input_reset_btn input_screenshot_btn input_pause_toggle_btn
        input_frame_advance_btn input_cheat_toggle_btn
        input_movie_record_toggle_btn input_recording_toggle_btn
        input_streaming_toggle_btn input_netplay_game_watch_btn
        input_ai_service_btn input_audio_mute_btn
        input_cheat_index_minus_btn input_cheat_index_plus_btn
        input_close_content_btn input_desktop_menu_toggle_btn
        input_disk_eject_toggle_btn input_disk_next_btn input_disk_prev_btn
        input_exit_emulator_btn input_fps_toggle_btn
        input_game_focus_toggle_btn input_grab_mouse_toggle_btn
        input_hold_slowmotion_btn input_osk_toggle_btn
        input_overlay_next_btn input_preempt_toggle_btn
        input_runahead_toggle_btn input_send_debug_info_btn
        input_toggle_slowmotion_btn input_toggle_statistics_btn
        input_toggle_vrr_runloop_btn input_volume_up_btn
        input_volume_down_btn input_netplay_fade_chat_toggle_btn
        input_netplay_host_toggle_btn input_netplay_ping_toggle_btn
        input_netplay_player_chat_btn"

    tmpra=/tmp/kidmode_ra.$$
    awkprog=/tmp/kidmode_ra_awk.$$
    {
        echo 'BEGIN {'
        i=0
        for k in $ra_keys; do
            i=$((i + 1))
            case "$k" in
                kiosk_mode_enable | video_font_enable) v=true ;;
                quick_menu_show_* | settings_show_*) v=false ;;
                *) v=nul ;;
            esac
            printf '  order[%d]="%s"; val["%s"]="%s";\n' \
                "$i" "$k" "$k" "$v"
        done
        echo "  n=$i"
        echo '}'
        cat << 'AWKEOF'
{
    equals = index($0, "=")
    if (equals > 0) {
        setting = substr($0, 1, equals - 1)
        sub(/^[ \t]*/, "", setting)
        sub(/[ \t]*$/, "", setting)
        if (setting in val) {
            print setting " = \"" val[setting] "\""
            seen[setting] = 1
            next
        }
    }
    print $0
}
END {
    for (i = 1; i <= n; i++) {
        setting = order[i]
        if (!(setting in seen))
            print setting " = \"" val[setting] "\""
    }
}
AWKEOF
    } > "$awkprog"

    awk -f "$awkprog" "$racfg" > "$tmpra" && mv -f "$tmpra" "$racfg"
    rm -f "$awkprog"
    log "RetroArch kiosk lock applied (direct-key lookup, duplicates locked)."
}

restore_ra_lock() {
    if [ -f "$rabackup" ]; then
        cp "$rabackup" "$racfg"
        rm -f "$rabackup"
        log "RetroArch config restored."
    fi
}

# ------------------------ Blue-light-filter lock ----------------------------
# MENU+B is a system-level shortcut (handled by keymon, outside RetroArch)
# that toggles the blue-light filter by calling this script with "enable" or
# "disable". While armed, we prepend a guard that makes those two calls a
# no-op, so the manual toggle does nothing. The scheduled auto on/off (if the
# person has that feature configured) is untouched, since it calls the
# enable/disable shell functions directly rather than going through this
# case dispatch. Original script restored byte-for-byte on unlock.

apply_blf_lock() {
    [ -f "$blfscript" ] || return 0
    mkdir -p "$backupdir"
    if ! grep -q "KIDMODE_BLF_GUARD" "$blfscript" 2> /dev/null; then
        [ -f "$blfbackup" ] || cp "$blfscript" "$blfbackup"
        tmpblf=/tmp/kidmode_blf.$$
        {
            printf '%s\n' "# KIDMODE_BLF_GUARD: while Kids Mode is armed, ignore the manual"
            printf '%s\n' "# MENU+B toggle (this script called with enable/disable) so a kid"
            printf '%s\n' "# can't turn the blue-light filter on/off mid-game."
            printf '%s\n' 'if [ -f /mnt/SDCARD/.kidmode ] && { [ "$1" = "enable" ] || [ "$1" = "disable" ]; }; then'
            printf '%s\n' '    exit 0'
            printf '%s\n' 'fi'
            cat "$blfscript"
        } > "$tmpblf"
        mv -f "$tmpblf" "$blfscript"
        chmod +x "$blfscript" 2> /dev/null
        log "MENU+B blue-light toggle disabled while armed."
    fi
}

restore_blf_lock() {
    if [ -f "$blfbackup" ]; then
        cp "$blfbackup" "$blfscript"
        rm -f "$blfbackup"
        chmod +x "$blfscript" 2> /dev/null
        log "blue_light.sh restored."
    fi
}

# ------------------------------ save profile --------------------------------
# Saves/CurrentProfile holds several DIFFERENT kinds of data mixed together:
# actual save files/save-states and GameSwitcher's thumbnail cache (personal,
# tied to who's playing) alongside config/ — RetroArch's per-core settings
# like aspect ratio, scanlines/shaders, CPU clock — and theme/, which are
# device-wide preferences, not personal data, and should stay exactly the
# same no matter who's playing.
#
# Onion's own Guest Mode swaps the WHOLE folder (MainProfile <->
# GuestProfile) since a guest is meant to get a fully separate setup. Kids
# Mode only wants the personal parts isolated — so we swap just the
# saves/states/romScreens subfolders individually, leaving config/, theme/,
# and lists/ untouched and shared throughout.
#
# The kid's own save progress should persist across sessions — so instead of
# a throwaway park each time, Kids Mode keeps its own permanent
# Saves/KidsProfile (holding just these three subfolders) that's swapped in
# at arm time and swapped back out (keeping whatever was added) at disarm,
# while whatever was there before (from Main or Guest — we don't need to
# know which) is parked untouched in between. A plain directory rename
# can't partially fail or leave mismatched data the way editing files in
# place could.
apply_profile_isolation() {
    [ -f "$profile_isolation_marker" ] && return 0
    mkdir -p "$kids_profile" "$current_profile" "$backupdir"
    for d in $isolated_subdirs; do
        rm -rf "${backupdir:?}/profile-parked-$d"
        if [ -d "$current_profile/$d" ]; then
            mv "$current_profile/$d" "$backupdir/profile-parked-$d"
        fi
        if [ -d "$kids_profile/$d" ]; then
            mv "$kids_profile/$d" "$current_profile/$d"
        else
            mkdir -p "$current_profile/$d"
        fi
    done
    touch "$profile_isolation_marker"
    log "Switched to the kid's own saves/states/thumbnails for this session."
}

restore_profile_isolation() {
    [ -f "$profile_isolation_marker" ] || return 0
    mkdir -p "$kids_profile"
    for d in $isolated_subdirs; do
        rm -rf "${kids_profile:?}/$d"
        if [ -d "$current_profile/$d" ]; then
            mv "$current_profile/$d" "$kids_profile/$d" # keep kid's progress for next time
        fi
        if [ -d "$backupdir/profile-parked-$d" ]; then
            mv "$backupdir/profile-parked-$d" "$current_profile/$d"
        fi
    done
    rm -f "$profile_isolation_marker"
    log "Restored the previous saves/states/thumbnails."
}

# ------------------------- MENU button override ----------------------------
# While armed, a single press of the MENU button in-game saves and exits
# straight back to the kid launcher (keymap ingame_single_press = 2,
# "exit to menu") instead of opening the GameSwitcher overlay, which could
# expose the parent's recent games. keymon reads keymap.json at startup, so
# it is restarted after the change. Original keymap restored on unlock.

restart_keymon() {
    killall keymon 2> /dev/null
    if [ -f "$flagfile" ] && [ -x "$kids_keymon_bin" ]; then
        "$kids_keymon_bin" &
        log "Started Kids Mode keymon."
    elif [ -x "$onion_keymon_bin" ]; then
        "$onion_keymon_bin" &
        log "Restored Onion keymon."
    else
        keymon &
        log "Started system keymon."
    fi
}

apply_keymap_override() {
    mkdir -p "$backupdir"
    if [ -f "$keymapcfg" ]; then
        [ -f "$keymapbackup" ] || cp "$keymapcfg" "$keymapbackup"
        tmpkm=/tmp/kidmode_keymap.$$
        if jq '.ingame_single_press = 2' "$keymapcfg" > "$tmpkm" 2> /dev/null; then
            mv -f "$tmpkm" "$keymapcfg"
        else
            rm -f "$tmpkm"
        fi
    else
        touch "$keymapnone"
        printf '{\n    "ingame_single_press": 2\n}\n' > "$keymapcfg"
    fi
    restart_keymon
    log "MENU button set to exit-to-launcher while armed."
}

restore_keymap_override() {
    keymap_restored=0
    if [ -f "$keymapnone" ]; then
        rm -f "$keymapcfg" "$keymapnone"
        keymap_restored=1
    elif [ -f "$keymapbackup" ]; then
        cp "$keymapbackup" "$keymapcfg"
        rm -f "$keymapbackup"
        keymap_restored=1
    fi
    if [ "$keymap_restored" = "1" ]; then
        sync
        log "keymap.json restored."
    fi
    # Also restore Onion's original binary when no keymap backup existed.
    restart_keymon
}

# ------------------------------ play timer ---------------------------------
# Daily play budget in 5-minute steps (timer_minutes in kidmode.json;
# 0 = no timer). A background ticker counts *consumed* seconds — not wall
# clock — so sleeping the device pauses the timer and rebooting doesn't
# reset it (used/bonus persist in timer_state.txt, keyed to the day).
# The countdown shows inside games via RetroArch's OSD (see notify_game);
# at zero RetroArch gets a network QUIT, which triggers Onion's normal
# auto-save — the game resumes exactly there next launch.

get_timer_minutes() {
    tm=""
    [ -f "$timer_minutes_file" ] && IFS= read -r tm < "$timer_minutes_file"
    [ -n "$tm" ] || tm="$cfg_timer_minutes"
    case "$tm" in
        '' | *[!0-9]*) echo 0 ;;
        *) echo "$tm" ;;
    esac
}

state_used() {
    v=""
    if [ -f "$timer_state" ]; then
        {
            IFS= read -r _timer_day
            IFS= read -r v
        } < "$timer_state"
    fi
    case "$v" in '' | *[!0-9]*) echo 0 ;; *) echo "$v" ;; esac
}
state_bonus() {
    v=""
    if [ -f "$timer_state" ]; then
        {
            IFS= read -r _timer_day
            IFS= read -r _timer_used
            IFS= read -r v
        } < "$timer_state"
    fi
    case "$v" in '' | *[!0-9]*) echo 0 ;; *) echo "$v" ;; esac
}

state_write() { # $1 used, $2 bonus
    mkdir -p "$backupdir"
    printf '%s\n%s\n%s\n' "$timer_state_date" "$1" "$2" > "$timer_state.tmp"
    mv -f "$timer_state.tmp" "$timer_state"
}

# Recompute and publish remaining seconds right now (clamped to >= 0;
# file absent = timer off). Called by the ticker and after menu changes.
# NB: the budget is per SESSION (set at arm / extended via Add play time);
# there is no daily reset — a new arm starts a fresh budget.
update_remaining_now() {
    budget=$(($(get_timer_minutes) * 60 + $(state_bonus)))
    if [ "$budget" -le 0 ]; then
        rm -f "$remaining_file" "$timesup_since_file"
        return 0
    fi
    rem=$((budget - $(state_used)))
    [ "$rem" -lt 0 ] && rem=0
    echo "$rem" > "$remaining_file"
    if [ "$rem" -eq 0 ]; then
        if [ ! -f "$timesup_since_file" ]; then
            date +%s > "$timesup_since_file.tmp"
            mv -f "$timesup_since_file.tmp" "$timesup_since_file"
        fi
    else
        rm -f "$timesup_since_file"
    fi
    return 0
}

timer_remaining() {
    update_remaining_now
    if [ -f "$remaining_file" ]; then
        remaining_value=0
        IFS= read -r remaining_value < "$remaining_file"
        echo "$remaining_value"
    else
        echo -1 # timer off
    fi
}

add_bonus() {
    state_write "$(state_used)" "$(($(state_bonus) + $1))"
    update_remaining_now
    log "Bonus play time added: $1 s"
}

set_timer_minutes() {
    config_merge --argjson m "$1" '.timer_minutes = $m'
    update_remaining_now
    log "Timer set to $1 min/day."
}

# RetroArch redraws the framebuffer every frame, so imgpop overlays are not
# reliably visible inside games. Use RetroArch's own OSD instead (SHOW_MSG
# network command — same socket used for the graceful QUIT). Silently
# ignored by anything that isn't RetroArch.
notify_game() {
    sendUDP "SHOW_MSG $1" > /dev/null 2>&1 &
}

# RA's OSD messages last ~3 s; re-pushing the same text every ~2 s makes it
# render as one continuous message.
pin_message() {
    (
        for _i in 1 2 3 4 5; do
            sendUDP "SHOW_MSG $1" > /dev/null 2>&1
            sleep 2
        done
    ) &
}

game_is_running() {
    pgrep -f "cmd_to_run.sh" > /dev/null 2>&1 ||
        { [ -f "$player_pid" ] && kill -0 "$(cat "$player_pid")" 2> /dev/null; }
}

# Ask the running game to stop gracefully. RetroArch first (network QUIT →
# normal exit path → Onion auto-save state); escalate only if needed.
# Non-RetroArch games (ports, standalone) get a plain TERM — best effort.
save_quit_game() {
    if [ -f "$player_pid" ]; then
        kill "$(cat "$player_pid")" 2> /dev/null
        log "Play time over; video stopped."
        return
    fi
    notify_game "Time's up! Saving your game..."
    sleep 2
    if pgrep retroarch > /dev/null 2>&1; then
        sendUDP QUIT
        sleep 3
        if pgrep retroarch > /dev/null 2>&1; then
            sendUDP QUIT
            sleep 3
        fi
        if pgrep retroarch > /dev/null 2>&1; then
            killall -TERM retroarch 2> /dev/null
            sleep 2
        fi
    elif game_is_running; then
        pkill -TERM -f "cmd_to_run.sh" 2> /dev/null
        sleep 2
    fi
    log "Play time over; game stopped."
}

ticker_loop() {
    while [ -f "$flagfile" ]; do
        sleep 10
        [ -f "$flagfile" ] || break
        [ -f /tmp/shutting_down ] && break

        budget=$(($(get_timer_minutes) * 60 + $(state_bonus)))
        if [ "$budget" -le 0 ]; then
            rm -f "$remaining_file" "$timesup_since_file"
            continue
        fi

        used=$(($(state_used) + 10))
        state_write "$used" "$(state_bonus)"
        rem=$((budget - used))
        [ "$rem" -lt 0 ] && rem=0
        echo "$rem" > "$remaining_file"
        if [ "$rem" -eq 0 ]; then
            if [ ! -f "$timesup_since_file" ]; then
                date +%s > "$timesup_since_file.tmp"
                mv -f "$timesup_since_file.tmp" "$timesup_since_file"
            fi
        else
            rm -f "$timesup_since_file"
        fi

        if game_is_running; then
            rem_min=$(((rem + 59) / 60))

            # Fresh game session: announce the budget once via RA's OSD
            if [ "$game_seen" != "1" ]; then
                game_seen=1
                [ "$rem" -gt 0 ] && notify_game "Play time: $rem_min minutes"
            fi

            # In-game countdown via RetroArch OSD only. (imgpop overlays are
            # erased by RA's per-frame redraw AND draw in panel-native
            # coordinates — rotated 180° from the viewed image — so they
            # only produce a brief flipped flash. Not used during games.)
            if [ "$rem" -gt 0 ]; then
                if [ "$rem_min" -le 5 ]; then
                    # Last 5 minutes: countdown stays pinned on screen
                    if [ "$rem_min" -eq 1 ]; then
                        pin_message "1 minute left!"
                    else
                        pin_message "$rem_min minutes left"
                    fi
                elif [ "$rem_min" != "$last_notified_min" ] &&
                    [ $((rem_min % 5)) -eq 0 ]; then
                    notify_game "$rem_min minutes left"
                fi
                last_notified_min="$rem_min"
            fi

            if [ "$rem" -le 0 ]; then
                save_quit_game
            fi
        else
            game_seen=0
            last_notified_min=""
        fi
    done
    rm -f "$remaining_file"
}

start_ticker() {
    stop_ticker
    ticker_loop &
    echo $! > "$ticker_pid_file"
}

stop_ticker() {
    if [ -f "$ticker_pid_file" ]; then
        kill "$(cat "$ticker_pid_file")" 2> /dev/null
        rm -f "$ticker_pid_file"
    fi
    killall imgpop 2> /dev/null # remove any lingering chip overlay
    rm -f "$remaining_file"
}

# --------------------------- shutdown handling -----------------------------
# runtime.sh's main loop normally reacts to /tmp/.offOrder; while Kids Mode
# blocks that loop we must handle it ourselves or the device won't power off
# cleanly after keymon kills a game.

check_off_order() {
    [ -f /tmp/.offOrder ] || return 0
    touch /tmp/shutting_down
    # Hide the outgoing carousel/player before any process cleanup. Keep the
    # PWM black until bootScreen has painted both framebuffer pages, then
    # reveal only Onion's completed shutdown image.
    [ -w "$brightness_pwm" ] && printf '0\n' > "$brightness_pwm"
    for _off_script in "$sysdir"/checkoff/*.sh; do
        [ -f "$_off_script" ] && sh "$_off_script"
    done
    bootScreen "$1" 2> /dev/null
    shutdown_brightness=$(/customer/app/jsonval brightness 2> /dev/null)
    case "$shutdown_brightness" in
        '' | *[!0-9]*) shutdown_brightness=5 ;;
    esac
    shutdown_brightness_raw=$(awk -v level="$shutdown_brightness" \
        'BEGIN { printf "%d", 3 * exp(0.350656 * level) + 0.5 }')
    [ -w "$brightness_pwm" ] &&
        printf '%s\n' "$shutdown_brightness_raw" > "$brightness_pwm"
    sleep 0.3
    shutdown
    sleep 60 # never reached; wait for poweroff
}

# ----------------------------- game launch ---------------------------------

start_audioserver_if_needed() {
    if ! pgrep audioserver > /dev/null 2>&1; then
        defvol=$(/customer/app/jsonval vol | awk '{ printf "%.0f\n", 48 * (log(1 + $1) / log(10)) - 60 }')
        "$miyoodir/app/audioserver" "$defvol" &
        sleep 0.5
    fi
}

set_resolution() {
    _res_x="${1%x*}"
    _res_y="${1#*x}"
    bootScreen clear
    fbset -g "$_res_x" "$_res_y" "$_res_x" $((_res_y * 2)) 32
    killall -SIGUSR1 batmon 2> /dev/null
    killall -SIGUSR1 keymon 2> /dev/null
}

enable_ra_network_cmds() {
    # Same patch runtime.sh applies before every game (Onion features rely
    # on RetroArch network commands, e.g. save-on-shutdown).
    if [ -x "$sysdir/script/patch_ra_cfg.sh" ]; then
        cat > /tmp/onion_ra_patch.cfg <<- EOM
network_cmd_enable = "true"
EOM
        "$sysdir/script/patch_ra_cfg.sh" /tmp/onion_ra_patch.cfg
        rm -f /tmp/onion_ra_patch.cfg
    fi
}

# "Start over": launch without loading the auto-save snapshot. Same
# mechanism Onion's runtime.sh uses for its reset-game flag. In-game saves
# (battery saves etc.) are untouched — only the resume snapshot is skipped.
reset_cfg=/tmp/kidmode_reset.cfg

strip_reset_appendconfig() { # $1 = emulator launch script
    [ -n "$1" ] && [ -w "$1" ] || return 0
    if grep -q "$reset_cfg" "$1" 2> /dev/null; then
        sed -i "s| --appendconfig \"$reset_cfg\"||g" "$1"
    fi
}

# Build $sysdir/cmd_to_run.sh for a favorite exactly like MainUI would,
# including the per-rom core override (.game_config/<rom>.cfg).
# $3 = "fresh" to start over instead of resuming.
build_game_cmd() {
    game_launch="$1"
    game_rompath="$2"
    game_fresh="${3:-}"

    if [ -f "$game_rompath" ]; then
        game_rompath="$(realpath "$game_rompath")"
    fi

    # Never leave a stale injection behind from an interrupted fresh launch
    strip_reset_appendconfig "$game_launch"

    if [ "$game_fresh" = "fresh" ]; then
        printf 'savestate_auto_load = "false"\nconfig_save_on_exit = "false"\n' > "$reset_cfg"
    fi

    echo "LD_PRELOAD=$miyoodir/lib/libpadsp.so \"$game_launch\" \"$game_rompath\"" > "$sysdir/cmd_to_run.sh"

    game_ext="$(basename "$game_rompath" | awk -F. '{print tolower($NF)}')"
    game_cfg="$(dirname "$game_rompath")/.game_config/$(basename "$game_rompath" ".$game_ext").cfg"

    game_direct=0
    if [ -f "$game_cfg" ] && [ -f "$game_launch" ] &&
        grep -q '.retroarch/cores' "$game_launch"; then
        game_core=$(grep "core\b" "$game_cfg" | awk '{split($0,a,"="); print a[2]}' | awk -F'"' '{print $2}' | tr -d '\n')
        if [ -n "$game_core" ] && [ -f "/mnt/SDCARD/RetroArch/.retroarch/cores/$game_core.so" ]; then
            if [ "$game_fresh" = "fresh" ]; then
                echo "LD_PRELOAD=$miyoodir/lib/libpadsp.so ./retroarch -v --appendconfig \"$reset_cfg\" -L \".retroarch/cores/$game_core.so\" \"$game_rompath\"" > "$sysdir/cmd_to_run.sh"
            else
                echo "LD_PRELOAD=$miyoodir/lib/libpadsp.so ./retroarch -v -L \".retroarch/cores/$game_core.so\" \"$game_rompath\"" > "$sysdir/cmd_to_run.sh"
            fi
            game_direct=1
        fi
    fi

    # Fresh launch through the emulator's launch script: inject the
    # appendconfig into the script like runtime.sh does (removed after)
    if [ "$game_fresh" = "fresh" ] && [ "$game_direct" -eq 0 ] &&
        [ -f "$game_launch" ] && grep -q './retroarch -v' "$game_launch"; then
        sed -i "s|./retroarch -v|& --appendconfig \"$reset_cfg\"|g" "$game_launch"
    fi

    # Escape dollar signs in rom filenames, like runtime.sh does
    if echo "$game_rompath" | grep -q '\$'; then
        sed -i 's/\$/\\$/g' "$sysdir/cmd_to_run.sh"
    fi

    chmod a+x "$sysdir/cmd_to_run.sh"
}

# Run whatever is in $sysdir/cmd_to_run.sh and clean up afterwards.
# Mirrors runtime.sh launch_game: audio, LOADING splash, 560p handling on the
# Miyoo Mini V4, playActivity tracking, and the post-game SAVING splash —
# so Onion auto-save/resume keeps working unchanged.
prepare_game_environment() {
    # Prepare RetroArch and the kid save profile once per armed session, so
    # the first game launches without an extra setup pause.
    [ -f "$game_environment_marker" ] && return 0
    apply_ra_lock
    apply_profile_isolation
    apply_keymap_override
    touch "$game_environment_marker"
}

prepare_game_environment_async() {
    if [ -f "$game_environment_marker" ]; then
        rm -f "$game_prepare_pid_file"
        return 0
    fi
    if [ -f "$game_prepare_pid_file" ] &&
        kill -0 "$(cat "$game_prepare_pid_file" 2> /dev/null)" 2> /dev/null; then
        return 0
    fi
    (
        started_at="$(date +%s)"
        prepare_game_environment
        finished_at="$(date +%s)"
        log "Startup: game preparation completed in $((finished_at - started_at))s."
    ) &
    echo $! > "$game_prepare_pid_file"
}

wait_for_game_environment() {
    if [ -f "$game_prepare_pid_file" ]; then
        prepare_pid="$(cat "$game_prepare_pid_file" 2> /dev/null)"
        case "$prepare_pid" in
            '' | *[!0-9]*) ;;
            *) wait "$prepare_pid" 2> /dev/null ;;
        esac
        rm -f "$game_prepare_pid_file"
    fi
    [ -f "$game_environment_marker" ] || prepare_game_environment
}

run_game_cmd() {
    [ -f "$sysdir/cmd_to_run.sh" ] || return 1

    # Preparation starts as soon as Kids Mode opens, in parallel with the
    # carousel. Only an immediate game launch waits for the remaining work.
    wait_for_game_environment

    run_cmd="$(cat "$sysdir/cmd_to_run.sh")"
    run_rompath="$(echo "$run_cmd" | awk '{ st = index($0,"\" \""); if (st) print substr($0,st+3,length($0)-st-3)}')"
    run_launch="$(echo "$run_cmd" | awk -F'"' '{print $2}')"

    tz_value="$(cat "$sysdir/config/.tz" 2> /dev/null)"

    start_audioserver_if_needed
    enable_ra_network_cmds

    # Miyoo Mini V4 (752x560): switch resolution if this system supports it
    changed_res=0
    fullres_path="$(dirname "$run_launch")/full_resolution"
    if [ -f /tmp/new_res_available ] && [ -f "$fullres_path" ]; then
        set_resolution "$(cat /tmp/screen_resolution 2> /dev/null || echo 752x560)"
        changed_res=1
    elif [ ! -f /tmp/new_res_available ]; then
        infoPanel --message "LOADING" --persistent --romscreen &
        touch /tmp/dismiss_info_panel
        sync
    fi

    [ -n "$run_rompath" ] && playActivity start "$run_rompath"

    log "launching: $run_cmd"
    cd /mnt/SDCARD/RetroArch || cd "$appdir"
    TZ="$tz_value" sh "$sysdir/cmd_to_run.sh"
    run_retval=$?
    log "game exited with $run_retval"

    if [ "$changed_res" -eq 1 ]; then
        set_resolution "640x480"
    fi

    if [ ! -f /tmp/.offOrder ] && [ -f /tmp/.displaySavingMessage ]; then
        rm -f /tmp/.displaySavingMessage
        infoPanel --message "SAVING" --persistent --romscreen &
        touch /tmp/dismiss_info_panel
        sync
    fi

    [ -n "$run_rompath" ] && playActivity stop "$run_rompath"

    # Remove any fresh-launch injection from the emulator's launch script
    strip_reset_appendconfig "$run_launch"
    rm -f "$reset_cfg"

    rm -f "$sysdir/cmd_to_run.sh"
    cd "$appdir" 2> /dev/null

    check_off_order "End_Save"
    return 0
}

is_game_cmd() {
    grep -q "retroarch/cores\|/../../Roms/\|/mnt/SDCARD/Roms/" "$1" 2> /dev/null
}

# --------------------------- boot hook install -----------------------------
# The startup hook ships inside the app folder and is (re)installed on every
# arm, so installing Kids Mode is just copying App/KidsMode
# onto the card — no manual edits inside the hidden .tmp_update folder.

hook_src="$appdir/kidmode_boot.sh"
hook_dst="$sysdir/startup/kidmode_boot.sh"

install_hook() {
    [ -f "$hook_src" ] || return 1
    mkdir -p "$sysdir/startup"
    if ! cmp -s "$hook_src" "$hook_dst" 2> /dev/null; then
        cp "$hook_src" "$hook_dst"
        log "Boot hook installed to $hook_dst"
    fi
    return 0
}

# ------------------------ MainUI favorites shortcut ------------------------
# Adds a "Kids Mode" entry to Onion's Favorites tab (usually the
# boot tab),
# so arming is one tap without visiting Apps. kidui filters this entry out
# of the kid carousel. Disable with "fav_shortcut": false in kidmode.json.

fav_entry='{"label":"Kids Mode","launch":"/mnt/SDCARD/App/KidsMode/launch.sh","type":5,"imgpath":"/mnt/SDCARD/Icons/Default/app/guest_on.png","rompath":"/mnt/SDCARD/App/KidsMode/launch.sh"}'

# An earlier version appended the shortcut without checking that the file
# ended in a newline, which could glue two JSON entries onto one line and
# corrupt the favorites list (breaking MainUI search results too). Split
# any glued lines back apart.
repair_favourites() {
    [ -f "$favfile" ] || return 0
    if grep -q '}{' "$favfile"; then
        awk '{gsub(/\}\{/, "}\n{"); print}' "$favfile" > "$favfile.tmp" &&
            mv -f "$favfile.tmp" "$favfile"
        log "Repaired glued lines in favourite.json."
    fi
}

favorites_signature() {
    [ -f "$favfile" ] || return 1
    cksum "$favfile" 2> /dev/null | awk '{print $1 ":" $2}'
}

repair_onion_favorites_if_needed() {
    [ -f "$favfile" ] || return 0
    repair_favourites
    current_signature="$(favorites_signature)"
    repaired_signature="$(sed -n 1p "$favorites_signature_file" 2> /dev/null)"
    if [ -z "$current_signature" ] || [ "$current_signature" != "$repaired_signature" ]; then
        started_at="$(date +%s)"
        if command -v tools > /dev/null 2>&1; then
            tools favfix > /dev/null 2>&1
        fi
        sorted_favorites=/tmp/kidsmode_favorites_sorted.$$
        if jq -sc 'sort_by((.label // "") | ascii_downcase)[]' \
            "$favfile" > "$sorted_favorites" 2> /dev/null; then
            mv -f "$sorted_favorites" "$favfile"
        else
            rm -f "$sorted_favorites"
        fi
        log "Onion favorites repaired and sorted A-Z after a favorites change."
        mkdir -p "$backupdir"
        favorites_signature > "$favorites_signature_file.tmp" &&
            mv -f "$favorites_signature_file.tmp" "$favorites_signature_file"
        finished_at="$(date +%s)"
        log "Startup: favorites repair completed in $((finished_at - started_at))s."
    fi
}

ensure_fav_shortcut() {
    repair_favourites

    # Default OFF: the entry confused MainUI's search results on some
    # setups. Opt in with "fav_shortcut": true in kidmode.json.
    if [ "$cfg_fav_shortcut" != "true" ]; then
        if grep -qF "/App/KidsMode/launch.sh" "$favfile" 2> /dev/null; then
            awk 'index($0, "/App/KidsMode/launch.sh") == 0 { print }' \
                "$favfile" > "$favfile.tmp" && mv -f "$favfile.tmp" "$favfile"
            log "Removed Kids Mode shortcut from favorites."
        fi
        return 0
    fi

    if ! grep -qF "/App/KidsMode/launch.sh" "$favfile" 2> /dev/null; then
        # Never append onto a final line that lacks its newline
        if [ -s "$favfile" ] && [ -n "$(tail -c 1 "$favfile")" ]; then
            echo >> "$favfile"
        fi
        printf '%s\n' "$fav_entry" >> "$favfile"
        log "Added Kids Mode shortcut to favorites."
    fi
}

# --------------------------- session timer picker --------------------------
# Shown right after arming: LEFT/RIGHT picks OFF / 5 / 10 / ... / 120 minutes
# (default OFF; must match TIMER_MAX in src/kidsMode/videoui.c). Selecting a
# value starts a fresh budget for this session.

pick_session_timer() {
    rm -f "$uiresult"
    "$kidui_bin" --pick-timer > "$uilog" 2>&1
    picker_rc=$?

    picked=0
    if [ "$picker_rc" -eq 5 ] && [ "$(sed -n 1p "$uiresult")" = "TIMER" ]; then
        picked="$(sed -n 2p "$uiresult")"
        case "$picked" in
            '' | *[!0-9]*) picked=0 ;;
        esac
        [ "$picked" -gt 120 ] && picked=120
    fi
    rm -f "$uiresult"

    set_timer_minutes "$picked"
    state_write 0 0 # fresh budget for this session
update_remaining_now
}

# -------------------------- Onion profile switch ---------------------------
# Run only after the parent PIN has been accepted. Kids Mode first puts the
# current child's saves back in KidsProfile/<origin>, restores the Onion
# profile that was parked at arm time, then performs the same whole-profile
# directory swap as Onion's official Guest Mode app. The script is relaunched
# afterwards so every profile-specific path and cached preference is rebuilt
# from the new origin.
switch_kids_profile() {
    target_profile="$1"
    [ "$target_profile" != "$source_profile" ] || return 0

    case "$source_profile:$target_profile" in
        Main:Guest)
            incoming_profile=/mnt/SDCARD/Saves/GuestProfile
            parked_profile=/mnt/SDCARD/Saves/MainProfile
            guest_config=configON.json
            ;;
        Guest:Main)
            incoming_profile=/mnt/SDCARD/Saves/MainProfile
            parked_profile=/mnt/SDCARD/Saves/GuestProfile
            guest_config=configOFF.json
            ;;
        *)
            log "Rejected invalid profile switch: $source_profile -> $target_profile"
            return 1
            ;;
    esac

    if [ ! -d "$incoming_profile" ] || [ -e "$parked_profile" ]; then
        log "Profile switch unavailable: $source_profile -> $target_profile"
        infoPanel -t "Kids Mode" -m "The requested Onion profile is unavailable." --auto
        return 1
    fi

    log "Switching Kids Mode profile: $source_profile -> $target_profile"
    wait_for_game_environment
    stop_ticker

    # Save the current Kids environment, then restore the current Onion
    # profile completely before asking Onion's profile folders to trade places.
    restore_profile_isolation
    rm -f "$game_environment_marker" "$game_prepare_pid_file"

    mkdir -p "$current_profile/lists"
    for onion_list in /mnt/SDCARD/Roms/*.json; do
        [ -f "$onion_list" ] || continue
        cp -f "$onion_list" "$current_profile/lists/" || {
            log "Profile switch aborted: could not save Onion lists."
            apply_profile_isolation
            start_ticker
            return 1
        }
    done

    system_json=/mnt/SDCARD/system.json
    current_theme_file="$current_profile/theme/currentTheme"
    if [ -f "$system_json" ] && command -v jq > /dev/null 2>&1; then
        mkdir -p "$current_profile/theme"
        jq -r '.theme // empty' "$system_json" > "$current_theme_file.tmp" 2> /dev/null &&
            mv -f "$current_theme_file.tmp" "$current_theme_file"
        rm -f "$current_theme_file.tmp"
    fi
    sync

    # The second rename can still fail on a damaged/full card. Roll the first
    # one back immediately so CurrentProfile is never left missing.
    if ! mv "$current_profile" "$parked_profile"; then
        log "Profile switch aborted: could not park CurrentProfile."
        apply_profile_isolation
        start_ticker
        return 1
    fi
    if ! mv "$incoming_profile" "$current_profile"; then
        mv "$parked_profile" "$current_profile" 2> /dev/null
        log "Profile switch rolled back: could not activate $target_profile."
        apply_profile_isolation
        start_ticker
        return 1
    fi

    # Make the new origin authoritative immediately after the directory swap.
    # If the marker cannot be committed, put both Onion profiles back exactly
    # where they were rather than continuing with ambiguous ownership.
    if ! printf '%s\n' "$target_profile" > "$source_profile_file.tmp" ||
        ! mv -f "$source_profile_file.tmp" "$source_profile_file"; then
        rm -f "$source_profile_file.tmp"
        mv "$current_profile" "$incoming_profile" 2> /dev/null
        mv "$parked_profile" "$current_profile" 2> /dev/null
        log "Profile switch rolled back: could not save the active profile."
        apply_profile_isolation
        start_ticker
        return 1
    fi

    # Replace the global favorites/recent JSON files with the newly active
    # Onion profile's copies. This intentionally mirrors Guest Mode itself.
    rm -f /mnt/SDCARD/Roms/*.json
    for onion_list in "$current_profile"/lists/*.json; do
        [ -f "$onion_list" ] || continue
        cp -f "$onion_list" /mnt/SDCARD/Roms/
    done

    current_theme_file="$current_profile/theme/currentTheme"
    if [ -s "$current_theme_file" ] && [ -f "$system_json" ] &&
        command -v jq > /dev/null 2>&1; then
        theme_tmp=/tmp/kidsmode_system_theme.$$
        if jq --arg theme "$(sed -n 1p "$current_theme_file")" \
            '.theme = $theme' "$system_json" > "$theme_tmp" 2> /dev/null; then
            mv -f "$theme_tmp" "$system_json"
        else
            rm -f "$theme_tmp"
        fi
    fi

    # Keep Onion's Guest Mode app icon/label accurate for the day Kids Mode is
    # eventually unlocked. Older Onion layouts simply skip this optional step.
    guest_app=/mnt/SDCARD/App/Guest_Mode
    if [ -f "$guest_app/data/$guest_config" ]; then
        cp -f "$guest_app/data/$guest_config" "$guest_app/config.json"
    fi
    command -v themeSwitcher > /dev/null 2>&1 && themeSwitcher --reapply_icons

    rm -f /tmp/kidsmode_floor /tmp/kidsmode_selection \
        /tmp/kidsmode_game_selection /tmp/kidsmode_video_selection \
        /tmp/kidsmode_folder /tmp/kidsmode_folder_history
    sync
    log "Kids Mode profile switched to $target_profile."

    # Re-exec rather than mutating dozens of cached profile paths in place.
    exec /bin/sh "$appdir/kid_mode_loop.sh" run
}

# ------------------------------ parent menu --------------------------------
# Shown after a correct PIN: exit Kids Mode, switch Main/Guest or add play
# time. "Add play time" is an inline value selector on the menu row (LEFT/RIGHT to
# pick 5-120 min, A/START to apply) — kidui reports the chosen minutes on
# line 3 of the result. Returns 0 = unlock requested, 1 = stay in Kids Mode.

parent_menu() {
    while :; do
        rm -f "$uiresult" "$lockfloor_result" "$categories_result"
        lock_val=0
        [ "$cfg_lock_current_floor" = "true" ] && lock_val=1
        refresh_category_values
        [ "$active_floor" = videos ] && menu_floor=VIDEOS || menu_floor=GAMES
        switch_profile_arg=""
        if [ "$source_profile" = Main ] &&
            [ -d /mnt/SDCARD/Saves/GuestProfile ]; then
            switch_profile_arg=Guest
        elif [ "$source_profile" = Guest ] &&
            [ -d /mnt/SDCARD/Saves/MainProfile ]; then
            switch_profile_arg=Main
        fi
        "$kidui_bin" --parent-menu \
            --remaining "$(timer_remaining)" \
            --floor "$menu_floor" \
            --lock-floor "$lock_val" \
            --switch-profile "$switch_profile_arg" \
            --show-stories "$show_stories_value" \
            --show-movies "$show_movies_value" \
            --show-series "$show_series_value" \
            --show-music "$show_music_value" \
            --show-cartoons "$show_cartoons_value" > "$uilog" 2>&1
        menu_rc=$?

        if [ -f "$lockfloor_result" ]; then
            new_lock_val="$(sed -n 1p "$lockfloor_result")"
            rm -f "$lockfloor_result"
            case "$new_lock_val" in
                1)
                    profile_config_merge '.lock_current_floor = true'
                    log "Current floor lock turned ON."
                    ;;
                0)
                    profile_config_merge '.lock_current_floor = false'
                    log "Current floor lock turned OFF."
                    ;;
            esac
        fi

        if [ -f "$categories_result" ]; then
            new_stories="$show_stories_value"
            new_movies="$show_movies_value"
            new_series="$show_series_value"
            new_music="$show_music_value"
            new_cartoons="$show_cartoons_value"
            while IFS='=' read -r category new_value; do
                case "$category:$new_value" in
                    stories:1) new_stories=1 ;;
                    stories:0) new_stories=0 ;;
                    movies:1) new_movies=1 ;;
                    movies:0) new_movies=0 ;;
                    series:1) new_series=1 ;;
                    series:0) new_series=0 ;;
                    music:1) new_music=1 ;;
                    music:0) new_music=0 ;;
                    cartoons:1) new_cartoons=1 ;;
                    cartoons:0) new_cartoons=0 ;;
                esac
            done < "$categories_result"
            rm -f "$categories_result"
            profile_config_merge --argjson stories "$new_stories" \
                --argjson movies "$new_movies" --argjson series "$new_series" \
                --argjson music "$new_music" --argjson cartoons "$new_cartoons" \
                '.show_stories = ($stories == 1) |
                 .show_movies = ($movies == 1) |
                 .show_series = ($series == 1) |
                 .show_music = ($music == 1) |
                 .show_cartoons = ($cartoons == 1)'
            log "Visible media folders updated from the parent menu."
            normalize_active_media_folder
        fi

        if [ "$menu_rc" -ne 5 ] || [ "$(sed -n 1p "$uiresult")" != "MENU" ]; then
            rm -f "$uiresult"
            return 1
        fi

        menu_action="$(sed -n 2p "$uiresult")"
        menu_arg="$(sed -n 3p "$uiresult")"
        rm -f "$uiresult" /tmp/kidsmode_game_selection \
            /tmp/kidsmode_video_selection
        case "$menu_action" in
            UNLOCK)
                return 0
                ;;
            NOTIMER)
                # Turn the play timer off entirely: clear the configured
                # minutes AND any bonus, so nothing keeps a budget alive.
                # The kid can play with no limit until re-armed or time is
                # added again.
                set_timer_minutes 0
                state_write 0 0
                update_remaining_now
                log "Play timer turned off from the parent menu."
                return 1
                ;;
            ADDTIME)
                case "$menu_arg" in
                    '' | *[!0-9]*)
                        # Older kidui without the inline selector: fall back
                        # to the separate picker screen; B cancels
                        rm -f "$uiresult"
                        "$kidui_bin" --pick-timer --no-off -t "Add play time" > "$uilog" 2>&1
                        if [ $? -eq 5 ] && [ "$(sed -n 1p "$uiresult")" = "TIMER" ]; then
                            menu_arg="$(sed -n 2p "$uiresult")"
                        else
                            menu_arg=""
                        fi
                        rm -f "$uiresult"
                        ;;
                esac
                case "$menu_arg" in
                    '' | *[!0-9]* | 0) ;; # canceled: back to the parent menu
                    *)
                        [ "$menu_arg" -gt 120 ] && menu_arg=120
                        add_bonus $((menu_arg * 60))
                        # Straight back to the kid so they can play (the menu
                        # already previewed the new remaining time)
                        return 1
                        ;;
                esac
                ;;
            SWITCHPROFILE)
                case "$menu_arg" in
                    Main | Guest)
                        switch_kids_profile "$menu_arg"
                        ;;
                esac
                ;;
        esac
    done
}

# ------------------------------ unlock -------------------------------------

disarm() {
    rm -f "$flagfile"
    rm -f /tmp/kidsmode_carousel_dimmed /tmp/kidsmode_media_dimmed \
        /tmp/kidsmode_media_playing "$timesup_since_file"
    stop_ticker
    wait_for_game_environment
    restore_ra_lock
    restore_blf_lock
    restore_profile_isolation
    restore_keymap_override
    rm -f "$game_environment_marker"
    ensure_fav_shortcut
    rm -f "$sysdir/cmd_to_run.sh" "$uiresult"
    sync
    log "Kids Mode disarmed."
    infoPanel -t "Kids Mode" -m "Unlocked!\nReturning to Onion." --auto
    # Reset the framebuffer (page/pan) so the relaunched MainUI is actually
    # visible — without this the screen can stay on our last-flipped page.
    bootScreen clear 2> /dev/null
}

# ------------------------------ main loop ----------------------------------

load_state_cache() {
    state_dump="$(jq -r '
        (.active_floor // ""),
        (.active_mode // ""),
        (.last_video // ""),
        (.active_folder // "")
    ' "$statefile" 2> /dev/null)" || state_dump=""
    {
        IFS= read -r active_floor
        IFS= read -r initial_mode
        IFS= read -r last_video
        IFS= read -r active_folder
    } <<EOF
$state_dump
EOF
}

state_save() {
    state_floor="$1" state_mode="$2" state_video="$3" state_folder="$4"
    mkdir -p "$backupdir" "$positions" "$videosdir/Imgs"
    jq -n --arg floor "$state_floor" --arg mode "$state_mode" \
        --arg video "$state_video" --arg folder "$state_folder" \
        '{version:2,active_floor:$floor,active_mode:$mode,last_video:$video,active_folder:$folder}' \
        > "$statefile.tmp" && mv -f "$statefile.tmp" "$statefile"
}

video_key() {
    if command -v sha256sum > /dev/null 2>&1; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    else
        printf '%s' "$1" | cksum | awk '{print $1}'
    fi
}

# Remember one carousel selection per media folder. A separate file for each
# level lets Films, Series, individual shows and deeper folders all reopen on
# their own last selected item, even after browsing elsewhere or rebooting.
folder_selection_file() {
    folder_key="$(video_key "$1")" || return 1
    printf '%s/%s.txt\n' "$folder_selections_dir" "$folder_key"
}

remember_folder_selection() {
    browse_folder="$1"
    selected_item="$2"
    case "$selected_item" in
        "$browse_folder"/*)
            selected_parent="${selected_item%/*}"
            [ "$selected_parent" = "$browse_folder" ] || return 0
            [ -f "$selected_item" ] || [ -d "$selected_item" ] || return 0
            ;;
        *) return 0 ;;
    esac
    mkdir -p "$folder_selections_dir"
    folder_state_file="$(folder_selection_file "$browse_folder")" || return 0
    printf '%s\n' "$selected_item" > "$folder_state_file.tmp" &&
        mv -f "$folder_state_file.tmp" "$folder_state_file"

    # Keep one consolidated index for kidui. Reading this single file is much
    # faster than opening every hashed selection file after each video exits.
    tab_char="$(printf '\t')"
    index_tmp="$folder_selections_index.tmp"
    : > "$index_tmp"
    if [ -f "$folder_selections_index" ]; then
        while IFS="$tab_char" read -r indexed_folder indexed_selection; do
            [ "$indexed_folder" = "$browse_folder" ] ||
                printf '%s\t%s\n' "$indexed_folder" "$indexed_selection" >> "$index_tmp"
        done < "$folder_selections_index"
    fi
    printf '%s\t%s\n' "$browse_folder" "$selected_item" >> "$index_tmp"
    mv -f "$index_tmp" "$folder_selections_index"
}

last_folder_selection() {
    browse_folder="$1"
    folder_state_file="$(folder_selection_file "$browse_folder")" || return 0
    [ -f "$folder_state_file" ] || return 0
    selected_item="$(sed -n 1p "$folder_state_file")"
    case "$selected_item" in
        "$browse_folder"/*)
            selected_parent="${selected_item%/*}"
            if [ "$selected_parent" = "$browse_folder" ] &&
                { [ -f "$selected_item" ] || [ -d "$selected_item" ]; }; then
                printf '%s\n' "$selected_item"
            fi
            ;;
    esac
}

ensure_audio_server() {
    pgrep audioserver > /dev/null 2>&1 && return 0
    volume="$(/customer/app/jsonval vol 2> /dev/null)"
    case "$volume" in '' | *[!0-9]*) volume=20 ;; esac
    defvol="$(awk -v v="$volume" 'BEGIN { printf "%.0f\n", 48 * (log(1 + v) / log(10)) - 60 }')"
    "$miyoodir/app/audioserver" "$defvol" > /dev/null 2>&1 &
    n=0
    while ! pgrep audioserver > /dev/null 2>&1 && [ "$n" -lt 8 ]; do
        sleep 1
        n=$((n + 1))
    done
}

restore_ffplay_state() {
    for backup in /mnt/SDCARD/App/FFplay/pos.cfg.kidsmode-backup \
        "$sysdir/pos.cfg.kidsmode-backup"; do
        [ -f "$backup" ] || continue
        original="${backup%.kidsmode-backup}"
        rm -f "$original"
        mv -f "$backup" "$original"
    done
}

hide_ffplay_state() {
    restore_ffplay_state
    for original in /mnt/SDCARD/App/FFplay/pos.cfg "$sysdir/pos.cfg"; do
        [ -f "$original" ] || continue
        mv -f "$original" "$original.kidsmode-backup"
    done
}

watch_media_duration() {
    duration_log="$1" duration_output="$2" watched_pid="$3"
    tries=0
    while [ "$tries" -lt 100 ] && [ -d "/proc/$watched_pid" ]; do
        media_seconds="$(awk '
            match($0, /Duration: [0-9]+:[0-9]+:[0-9]+/) {
                stamp = substr($0, RSTART + 10, RLENGTH - 10)
                split(stamp, value, ":")
                print (value[1] + 0) * 3600 + (value[2] + 0) * 60 +
                      (value[3] + 0)
                exit
            }
        ' "$duration_log" 2> /dev/null)"
        if [ -n "$media_seconds" ]; then
            printf '%s\n' "$media_seconds" > "$duration_output"
            return 0
        fi
        tries=$((tries + 1))
        sleep 0.1
    done
    return 1
}

play_video() {
    video="$1" fresh="$2" artwork_file="$3"
    [ -f "$video" ] || return 1
    mkdir -p "$positions"
    video_dir="${video%/*}"
    video_name="${video##*/}"
    video_base="${video_name%.*}"
    screenshot_dir="$video_dir/Imgs"
    screenshot_file="$screenshot_dir/$video_base.bmp"
    mkdir -p "$screenshot_dir"
    media_kind=video
    case "$video_name" in
        *.[mM][pP]3 | *.[mM]4[aA] | *.[aA][aA][cC] | *.[fF][lL][aA][cC] | \
        *.[oO][gG][gG] | *.[oO][pP][uU][sS] | *.[wW][aA][vV] | \
        *.[wW][mM][aA]) media_kind=audio ;;
    esac
    [ "$media_kind" = audio ] && screenshot_file=""
    key="$(video_key "$video")"
    posfile="$positions/$key.pos"
    runtime_pos="/tmp/kidsmode_position.$$"
    duration_file="/tmp/kidsmode_duration.$$"
    duration_log="/tmp/kidsmode_ffplay.$$"
    brightness_state="/tmp/kidsmode_brightness.$$"
    rm -f "$duration_file" "$duration_log" "$brightness_state"
    brightness_restore=""
    if [ -r "$brightness_pwm" ]; then
        brightness_restore="$(cat "$brightness_pwm" 2> /dev/null)"
        case "$brightness_restore" in
            '' | *[!0-9]*) brightness_restore="" ;;
        esac
    fi
    [ -n "$brightness_restore" ] &&
        printf '%s\n' "$brightness_restore" > "$brightness_state"
    start=0
    if [ "$fresh" != yes ] && [ -f "$posfile" ]; then
        start="$(cat "$posfile" 2> /dev/null)"
        case "$start" in '' | *[!0-9]*) start=0 ;; esac
    fi
    [ "$fresh" = yes ] && printf '0\n' > "$posfile"
    printf '%s\n' "$start" > "$runtime_pos"
    state_save videos running "$video" "$active_folder"
    hide_ffplay_state
    rm -f "$menu_exit_marker"
    ensure_audio_server
    # POWER uses the same genuine Onion suspend path for media and carousel.
    # The Kids Mode keymon has enough PID slots to stop and resume FFplay
    # reliably, so playback and its controls cannot continue during sleep.
    rm -f /tmp/stay_awake
    touch /tmp/kidsmode_media_playing
    # Match the media OSD to the battery font selected by the active Onion
    # theme. Fall back to Onion's standard Exo 2 face when a theme omits it.
    osd_font="/customer/app/Exo-2-Bold-Italic.ttf"
    osd_font_size=24
    battery_fixed=false
    battery_text_align=left
    battery_offset_x=0
    battery_offset_y=0
    theme_path="$(jq -r '.theme // empty' /mnt/SDCARD/system.json \
        2> /dev/null)"
    if [ ! -d "$theme_path" ] &&
       [ -r /mnt/SDCARD/Saves/CurrentProfile/theme/currentTheme ]; then
        IFS= read -r theme_path < \
            /mnt/SDCARD/Saves/CurrentProfile/theme/currentTheme
    fi
    [ "$theme_path" = "./" ] && theme_path="/mnt/SDCARD/miyoo/app/"
    [ -d "$theme_path" ] || theme_path="/mnt/SDCARD/miyoo/app/"
    theme_config="${theme_path%/}/config.json"
    if [ -r "$theme_config" ]; then
        profile_theme_config=/mnt/SDCARD/Saves/CurrentProfile/theme/config.json
        if [ -r "$profile_theme_config" ]; then
            effective_theme="$(jq -s '.[0] * .[1]' "$theme_config" \
                "$profile_theme_config" 2> /dev/null)"
        else
            effective_theme="$(jq -c '.' "$theme_config" 2> /dev/null)"
        fi
        theme_font="$(printf '%s' "$effective_theme" | jq -r \
            '.batteryPercentage.font // .hint.font // empty' 2> /dev/null)"
        theme_font_size="$(printf '%s' "$effective_theme" | jq -r \
            '.batteryPercentage.size // 24' 2> /dev/null)"
        battery_fixed="$(printf '%s' "$effective_theme" | jq -r \
            '.batteryPercentage.fixed // false' 2> /dev/null)"
        battery_text_align="$(printf '%s' "$effective_theme" | jq -r \
            '.batteryPercentage.textAlign // "left"' 2> /dev/null)"
        battery_offset_x="$(printf '%s' "$effective_theme" | jq -r \
            '.batteryPercentage.offsetX // 0' 2> /dev/null)"
        battery_offset_y="$(printf '%s' "$effective_theme" | jq -r \
            '.batteryPercentage.offsetY // 0' 2> /dev/null)"
        if [ -n "$theme_font" ]; then
            case "$theme_font" in
                /*) candidate_font="$theme_font" ;;
                *) candidate_font="${theme_path%/}/$theme_font" ;;
            esac
            [ -r "$candidate_font" ] && osd_font="$candidate_font"
        fi
        case "$theme_font_size" in
            '' | *[!0-9]*) ;;
            *) osd_font_size="$theme_font_size" ;;
        esac
    fi
    if ! cd "$sysdir"; then
        rm -f /tmp/kidsmode_media_playing
        return 1
    fi
    # A true restart must not ask FFplay to seek, even to zero. Apart from
    # avoiding unnecessary decoder preroll, this keeps 0:00 distinct from a
    # normal resume position.
    seek_args=""
    [ "$start" -gt 0 ] && seek_args="-ss $start"
    if [ "$media_kind" = audio ]; then
        # An MP3 can contain an attached cover exposed by FFplay as a video
        # stream. Never send that stream to the Miyoo hardware overlay: the
        # stable audio screen is drawn by libvcinput.
        VC_START_SECONDS="$start" VC_POSITION_FILE="$runtime_pos" \
            VC_CHECKPOINT_FILE="$posfile" VC_SCREENSHOT_FILE="" \
            VC_MEDIA_KIND=audio VC_ARTWORK_FILE="$artwork_file" \
            VC_MEDIA_TITLE="$video_base" \
            VC_DURATION_FILE="$duration_file" \
            VC_BRIGHTNESS_FILE="$brightness_pwm" \
            VC_BRIGHTNESS_RESTORE="$brightness_restore" \
            VC_BRIGHTNESS_STATE_FILE="$brightness_state" \
            VC_OSD_FONT="$osd_font" VC_OSD_FONT_SIZE="$osd_font_size" \
            VC_BATTERY_FIXED="$battery_fixed" \
            VC_BATTERY_TEXT_ALIGN="$battery_text_align" \
            VC_BATTERY_OFFSET_X="$battery_offset_x" \
            VC_BATTERY_OFFSET_Y="$battery_offset_y" \
            VC_THEME_PATH="$theme_path" \
            LD_PRELOAD="$libvcinput:$miyoodir/lib/libpadsp.so${LD_PRELOAD:+:$LD_PRELOAD}" \
            "$ffplay" -vn -autoexit -i "$video" $seek_args \
                2> "$duration_log" &
    else
        VC_START_SECONDS="$start" VC_POSITION_FILE="$runtime_pos" \
            VC_CHECKPOINT_FILE="$posfile" \
            VC_SCREENSHOT_FILE="$screenshot_file" VC_MEDIA_KIND=video \
            VC_DURATION_FILE="$duration_file" \
            VC_BRIGHTNESS_FILE="$brightness_pwm" \
            VC_BRIGHTNESS_RESTORE="$brightness_restore" \
            VC_BRIGHTNESS_STATE_FILE="$brightness_state" \
            VC_OSD_FONT="$osd_font" VC_OSD_FONT_SIZE="$osd_font_size" \
            VC_BATTERY_FIXED="$battery_fixed" \
            VC_BATTERY_TEXT_ALIGN="$battery_text_align" \
            VC_BATTERY_OFFSET_X="$battery_offset_x" \
            VC_BATTERY_OFFSET_Y="$battery_offset_y" \
            VC_THEME_PATH="$theme_path" \
            LD_PRELOAD="$libvcinput:$miyoodir/lib/libpadsp.so${LD_PRELOAD:+:$LD_PRELOAD}" \
            "$ffplay" -autoexit -vf "hflip,vflip" -i "$video" $seek_args \
            2> "$duration_log" &
    fi
    pid=$!
    printf '%s\n' "$pid" > "$player_pid"
    watch_media_duration "$duration_log" "$duration_file" "$pid" &
    duration_watcher=$!
    wait "$pid"
    player_status=$?
    kill "$duration_watcher" 2> /dev/null
    wait "$duration_watcher" 2> /dev/null
    cd "$appdir" 2> /dev/null
    # The next kidui instance primes both framebuffer pages with its first
    # complete frame. Do not call bootScreen or flip FFplay's dying surface
    # here: either can expose a foreign or inverted page during transition.
    # Restore the latest brightness selected during playback, not the value
    # captured when FFplay started. This also recovers cleanly if playback
    # ended while Kids Mode's own dim/off state was active.
    if [ ! -f /tmp/.offOrder ] && [ ! -f /tmp/shutting_down ] &&
        [ -r "$brightness_state" ] && [ -w "$brightness_pwm" ]; then
        brightness_restore="$(cat "$brightness_state" 2> /dev/null)"
        case "$brightness_restore" in
            '' | *[!0-9]*) ;;
            *) printf '%s\n' "$brightness_restore" > "$brightness_pwm" ;;
        esac
    fi
    # Keep libvcinput's two-second safety checkpoint after MENU and during
    # shutdown. FFplay may continue decoding briefly after POWER; its later
    # runtime value must not replace the last point the child actually saw.
    # Other exits still retain their exact runtime value.
    if [ ! -f /tmp/.offOrder ] && [ ! -f "$menu_exit_marker" ] &&
        [ -f "$runtime_pos" ]; then
        cp -f "$runtime_pos" "$posfile"
    fi
    rm -f "$runtime_pos" "$duration_file" "$duration_log" \
        "$brightness_state" "$player_pid" \
        /tmp/stay_awake /tmp/kidsmode_media_playing
    rm -f /mnt/SDCARD/App/FFplay/pos.cfg "$sysdir/pos.cfg"
    restore_ffplay_state
    check_off_order "End_Save"
    rem="$(timer_remaining)"
    if [ "$player_status" -eq 0 ] && [ ! -f "$menu_exit_marker" ] && \
        [ "$rem" != 0 ]; then
        printf '0\n' > "$posfile"
    fi
    rm -f "$menu_exit_marker"
    state_save videos carousel "$video" "$active_folder"
}

cmd_run() {
    if [ ! -f "$kidui_bin" ]; then
        log "kidui binary missing; disarming."
        rm -f "$flagfile"
        sync
        return 1
    fi
    chmod a+x "$kidui_bin" 2> /dev/null
    chmod a+x "$kids_keymon_bin" 2> /dev/null
    rm -f /tmp/kidsmode_carousel_dimmed /tmp/kidsmode_media_dimmed \
        /tmp/kidsmode_media_playing
    restart_keymon

    startup_started_at="$(date +%s)"
    prepare_game_environment_async
    repair_onion_favorites_if_needed

    ui_fails=0
    pin_fails=0
    pin_notice=""
    update_remaining_now

    # Existing installs from before the PIN snapshot existed: take one now,
    # so the next app update can't lose the PIN either
    if has_pin && [ ! -f "$pin_backup" ]; then
        backup_pin
    fi

    start_ticker
    startup_ready_at="$(date +%s)"
    log "Startup: blocking work completed in $((startup_ready_at - startup_started_at))s."

    load_state_cache
    [ "$active_floor" = videos ] || active_floor=games
    case "$active_folder" in "$videosdir"/*) ;; *) active_folder="" ;; esac
    case "$last_video" in "$videosdir"/*) ;; *) last_video="" ;; esac
    [ -d "$active_folder" ] || active_folder=""

    # Resume only what was genuinely running when power was cut. Returning
    # to the carousel before shutdown records carousel and does not relaunch.
    if [ "$initial_mode" = running ] && [ -f "$last_video" ] &&
        [ "$(timer_remaining)" != 0 ]; then
        active_floor=videos
        resume_artwork="$(sed -n 1p "$last_artwork_file" 2> /dev/null)"
        play_video "$last_video" no "$resume_artwork"
    fi

    # A game left in cmd_to_run.sh means the device powered off mid-game:
    # relaunch it first so RetroArch auto-resume works like stock Onion.
    if [ -f "$sysdir/cmd_to_run.sh" ] && is_game_cmd "$sysdir/cmd_to_run.sh"; then
        if [ "$(timer_remaining)" = "0" ]; then
            rm -f "$sysdir/cmd_to_run.sh"
        else
            log "resuming interrupted game"
            active_floor=games
            run_game_cmd
            state_save games carousel "$last_video" "$active_folder"
        fi
    elif [ "$initial_mode" = running ] &&
        [ "$active_floor" = games ] && [ -f "$last_game_file" ] &&
        [ "$(timer_remaining)" != 0 ]; then
        # Some shutdown paths let the emulator wrapper return and remove
        # cmd_to_run.sh just before the hardware actually powers off. The
        # running marker plus last_game.txt is the authoritative fallback.
        lg_launch="$(sed -n 1p "$last_game_file")"
        lg_rompath="$(sed -n 2p "$last_game_file")"
        if [ -f "$lg_launch" ] && [ -f "$lg_rompath" ]; then
            log "resuming interrupted game from saved state: $lg_rompath"
            build_game_cmd "$lg_launch" "$lg_rompath"
            run_game_cmd
            state_save games carousel "$last_video" "$active_folder"
        else
            state_save games carousel "$last_video" "$active_folder"
        fi
    fi

    while [ -f "$flagfile" ]; do
        check_off_order "End"

        # Defensive cleanup: nothing may divert the loop into GameSwitcher
        rm -f "$sysdir/.runGameSwitcher" 2> /dev/null
        pgrep keymon > /dev/null 2>&1 || restart_keymon

        # No PIN on file (armed, but the app folder was replaced and no
        # snapshot existed): the unlock gesture sets a NEW pin instead of
        # rejecting everything — never lock the parent out.
        no_pin_recovery=0
        if ! has_pin; then
            no_pin_recovery=1
        fi

        rm -f "$uiresult" "$folder_history_file"
        game_select_path=""
        video_select_path=""
        [ -f "$game_selection_file" ] &&
            game_select_path="$(sed -n 1p "$game_selection_file")"
        [ -n "$game_select_path" ] || [ ! -f "$last_game_file" ] ||
            game_select_path="$(sed -n 2p "$last_game_file")"
        [ -f "$video_selection_file" ] &&
            video_select_path="$(sed -n 1p "$video_selection_file")"
        [ -n "$video_select_path" ] || video_select_path="$last_video"
        [ "$active_floor" = videos ] && ui_floor_arg=VIDEOS || ui_floor_arg=GAMES
        set -- --floor "$ui_floor_arg"
        [ -n "$game_select_path" ] &&
            set -- "$@" --game-select "$game_select_path"
        [ -n "$video_select_path" ] &&
            set -- "$@" --video-select "$video_select_path"
        [ -n "$active_folder" ] && set -- "$@" --folder "$active_folder"
        [ "$cfg_lock_current_floor" = true ] && set -- "$@" --floor-locked
        refresh_category_values
        set -- "$@" \
            --show-stories "$show_stories_value" \
            --show-movies "$show_movies_value" \
            --show-series "$show_series_value" \
            --show-music "$show_music_value" \
            --show-cartoons "$show_cartoons_value"
        if [ "$no_pin_recovery" = "1" ] && [ -n "$pin_notice" ]; then
            "$kidui_bin" "$@" -t "Set a new PIN" --start-pin --notice "$pin_notice" > "$uilog" 2>&1
        elif [ "$no_pin_recovery" = "1" ]; then
            "$kidui_bin" "$@" -t "Set a new PIN" > "$uilog" 2>&1
        elif [ -n "$pin_notice" ]; then
            # Wrong PIN last time: reopen straight on the PIN screen so the
            # parent can try again in place
            "$kidui_bin" "$@" --start-pin --notice "$pin_notice" > "$uilog" 2>&1
        else
            "$kidui_bin" "$@" > "$uilog" 2>&1
        fi
        ui_rc=$?
        pin_notice=""

        # Folder navigation now stays inside kidui instead of relaunching it.
        # Persist every selection touched during that session once kidui
        # eventually returns to the loop.
        if [ -f "$folder_history_file" ]; then
            tab_char="$(printf '\t')"
            while IFS="$tab_char" read -r history_folder history_selection; do
                [ -n "$history_folder" ] && [ -n "$history_selection" ] &&
                    remember_folder_selection "$history_folder" "$history_selection"
            done < "$folder_history_file"
            rm -f "$folder_history_file"
        fi

        ui_floor="$(cat /tmp/kidsmode_floor 2> /dev/null)"
        case "$ui_floor" in games | videos) active_floor="$ui_floor" ;; esac
        ui_folder="$(sed -n 1p /tmp/kidsmode_folder 2> /dev/null)"
        case "$ui_folder" in
            "$videosdir"/*) [ -d "$ui_folder" ] && active_folder="$ui_folder" ;;
            '') active_folder="" ;;
        esac
        ui_selection="$(sed -n 1p /tmp/kidsmode_selection 2> /dev/null)"
        ui_game_selection="$(sed -n 1p /tmp/kidsmode_game_selection 2> /dev/null)"
        ui_video_selection="$(sed -n 1p /tmp/kidsmode_video_selection 2> /dev/null)"
        [ -n "$ui_game_selection" ] &&
            printf '%s\n' "$ui_game_selection" > "$game_selection_file"
        [ -n "$ui_video_selection" ] &&
            printf '%s\n' "$ui_video_selection" > "$video_selection_file"
        # Compatibility fallback if an older binary is copied beside this
        # script during an interrupted update.
        if [ -z "$ui_game_selection" ] && [ -z "$ui_video_selection" ] &&
            [ -n "$ui_selection" ]; then
            if [ "$active_floor" = videos ]; then
                ui_video_selection="$ui_selection"
                printf '%s\n' "$ui_selection" > "$video_selection_file"
            else
                ui_game_selection="$ui_selection"
                printf '%s\n' "$ui_selection" > "$game_selection_file"
            fi
        fi
        [ -n "$active_folder" ] &&
            remember_folder_selection "$active_folder" "$ui_video_selection"
        state_save "$active_floor" carousel "$last_video" "$active_folder"

        check_off_order "End"

        case "$ui_rc" in
            0) # game selected
                sel_verb="$(sed -n 1p "$uiresult")"
                case "$sel_verb" in
                    FOLDER)
                        active_floor=videos
                        active_folder="$(sed -n 2p "$uiresult")"
                        folder_selection="$(last_folder_selection "$active_folder")"
                        if [ -n "$folder_selection" ]; then
                            printf '%s\n' "$folder_selection" > "$video_selection_file"
                        else
                            printf '\n' > "$video_selection_file"
                        fi
                        last_video=""
                        state_save videos carousel "" "$active_folder"
                        continue
                        ;;
                    BACK)
                        active_floor=videos
                        leaving_folder="$(sed -n 2p "$uiresult")"
                        case "$leaving_folder" in
                            "$videosdir"/*) ;;
                            *) continue ;;
                        esac
                        parent_folder="${leaving_folder%/*}"
                        case "$parent_folder" in
                            "$videosdir") active_folder="" ;;
                            "$videosdir"/*)
                                if [ -d "$parent_folder" ]; then
                                    active_folder="$parent_folder"
                                else
                                    active_folder=""
                                fi
                                ;;
                            *) active_folder="" ;;
                        esac
                        printf '%s\n' "$leaving_folder" > "$video_selection_file"
                        state_save videos carousel "$last_video" "$active_folder"
                        continue
                        ;;
                    PLAY | RESTART)
                        active_floor=videos
                        last_video="$(sed -n 2p "$uiresult")"
                        last_artwork="$(sed -n 3p "$uiresult")"
                        printf '%s\n' "$last_artwork" > "$last_artwork_file"
                        printf '%s\n' "$last_video" > "$video_selection_file"
                        play_video "$last_video" \
                            "$([ "$sel_verb" = RESTART ] && echo yes || echo no)" \
                            "$last_artwork"
                        ui_fails=0
                        continue
                        ;;
                    LAUNCH | LAUNCH_FRESH) active_floor=games ;;
                    *) continue ;;
                esac
                sel_launch="$(sed -n 2p "$uiresult")"
                sel_rompath="$(sed -n 3p "$uiresult")"
                [ -f "$sel_rompath" ] || continue

                sel_rem="$(timer_remaining)"
                [ "$sel_rem" = "0" ] && continue # out of time; kidui shows it

                if [ "$sel_verb" = "LAUNCH_FRESH" ]; then
                    build_game_cmd "$sel_launch" "$sel_rompath" fresh
                else
                    build_game_cmd "$sel_launch" "$sel_rompath"
                fi
                # Remember the selection. Actual boot resume is driven only
                # by the real running/carousel state, never by an option.
                mkdir -p "$backupdir"
                printf '%s\n%s\n' "$sel_launch" "$sel_rompath" > "$last_game_file"
                state_save games running "$last_video" "$active_folder"
                run_game_cmd
                state_save games carousel "$last_video" "$active_folder"
                ui_fails=0
                ;;
            7) # Five minutes after "Time's up!": power off even if buttons
                # were used, unless the parent explicitly turned the timer
                # OFF or added more time before the deadline.
                log "Times-up deadline reached; powering off."
                touch /tmp/.offOrder
                check_off_order "End"
                ;;
            3) # PIN entered
                [ "$(sed -n 1p "$uiresult")" = "PIN" ] || continue
                entered_pin="$(sed -n 2p "$uiresult")"
                rm -f "$uiresult"
                is_4_digits "$entered_pin" || continue

                if [ "$no_pin_recovery" = "1" ]; then
                    # The PIN just entered becomes the new PIN (after a
                    # confirm step)
                    confirm_pin="$(run_pin_entry "Confirm new PIN")"
                    if [ -n "$confirm_pin" ] && [ "$confirm_pin" = "$entered_pin" ]; then
                        store_pin "$entered_pin"
                        pin_fails=0
                        if parent_menu; then
                            disarm
                            return 0
                        fi
                    elif [ -n "$confirm_pin" ]; then
                        pin_notice="PINs did not match - try again"
                    fi
                elif verify_pin "$entered_pin"; then
                    pin_fails=0
                    if parent_menu; then
                        disarm
                        return 0
                    fi
                else
                    pin_fails=$((pin_fails + 1))
                    log "Wrong PIN attempt ($pin_fails)."
                    sleep 1 # slow down guessing
                    if [ "$pin_fails" -ge 3 ]; then
                        pin_notice="Wrong PIN - to reset it, see the README"
                    else
                        pin_notice="Wrong PIN - try again"
                    fi
                fi
                ;;
            *) # UI crashed or won't start
                ui_fails=$((ui_fails + 1))
                log "kidui exited with unexpected code $ui_rc (fail $ui_fails/3)"
                if [ "$ui_fails" -ge 3 ]; then
                    # Fail open: a broken Kids Mode must never brick the
                    # device. Parent can re-arm after fixing the SD card.
                    infoPanel -t "Kids Mode" -m "Interface failed.\nReturning to normal Onion." --auto
                    disarm
                    return 1
                fi
                sleep 1
                ;;
        esac
    done

    # Flag removed externally (e.g. deleted from a computer) — clean up
    stop_ticker
    wait_for_game_environment
    restore_ra_lock
    restore_blf_lock
    restore_profile_isolation
    rm -f /tmp/kidsmode_carousel_dimmed /tmp/kidsmode_media_dimmed \
        /tmp/kidsmode_media_playing
    restore_keymap_override
    rm -f "$game_environment_marker"
    rm -f "$sysdir/cmd_to_run.sh"
    bootScreen clear 2> /dev/null
    return 0
}

cmd_arm() {
    if [ ! -f "$kidui_bin" ]; then
        infoPanel -t "Kids Mode" -m "kidui binary is missing.\nReinstall Kids Mode." --auto
        return 1
    fi

    fav_count=0
    [ -f "$favfile" ] && fav_count=$(grep -c "rompath" "$favfile" 2> /dev/null)
    if [ "$fav_count" -eq 0 ]; then
        infoPanel -t "Kids Mode" -m "No favorites found.\nAdd some favorites first,\nthen try again." --auto
        return 1
    fi

    if ! install_hook; then
        infoPanel -t "Kids Mode" -m "kidmode_boot.sh is missing.\nReinstall Kids Mode." --auto
        return 1
    fi

    if ! ensure_pin; then
        infoPanel -t "Kids Mode" -m "PIN setup canceled.\nThe mode was not armed." --auto
        return 1
    fi

    pick_session_timer

    apply_blf_lock
    ensure_fav_shortcut
    # Save the Onion profile of origin before the armed flag is created.
    # The boot hook uses this value to restore the same isolated environment.
    printf '%s\n' "$source_profile" > "$source_profile_file"
    touch "$flagfile"
    # Flush writes after launch without holding the timer screen on display.
    (sleep 2; sync) &
    log "Kids Mode armed (timer: $(get_timer_minutes) min)."

    cmd_run
}

ensure_config
load_config_cache || exit 1

case "${1:-run}" in
    arm)
        cmd_arm
        ;;
    run)
        [ -f "$flagfile" ] || exit 0
        hash_plain_pin
        # App updated while armed? kidmode.json ships blank — bring the PIN
        # back from the snapshot in Saves/KidsMode
        has_pin || restore_pin_backup
        cmd_run
        ;;
    *)
        echo "Usage: kid_mode_loop.sh [arm|run]" >&2
        exit 1
        ;;
esac

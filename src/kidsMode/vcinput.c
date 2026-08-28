#define _GNU_SOURCE
#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef int (*poll_fn)(SDL_Event *);
typedef int (*wait_fn)(SDL_Event *);
typedef int (*peep_fn)(SDL_Event *, int, SDL_eventaction, Uint32);
typedef int (*overlay_fn)(SDL_Overlay *, SDL_Rect *);
typedef int (*flip_fn)(SDL_Surface *);
typedef SDL_Surface *(*set_mode_fn)(int, int, int, Uint32);
typedef void (*update_fn)(SDL_Surface *, Sint32, Sint32, Uint32, Uint32);
typedef void (*updates_fn)(SDL_Surface *, int, SDL_Rect *);
typedef void (*pause_audio_fn)(int);
static poll_fn real_poll;
static wait_fn real_wait;
static peep_fn real_peep;
static overlay_fn real_overlay;
static flip_fn real_flip;
static set_mode_fn real_set_mode;
static update_fn real_update;
static updates_fn real_updates;
static pause_audio_fn real_pause_audio;
static SDL_Overlay *last_overlay;
static SDL_Rect last_overlay_rect;
static bool last_overlay_rect_ready;
static bool last_overlay_painted;
static bool inside_event_call;
static bool paused;
static bool menu_down;
static bool menu_used;
static bool screenshot_down;
static bool x_down;
static bool y_down;
static Uint32 menu_pressed_at;
static long menu_brightness_at_press = -1;
static SDLKey seek_input = SDLK_UNKNOWN;
static Uint32 seek_started_at;
static Uint32 seek_last_step;
static int pending_seek_events;
static SDLKey pending_seek_key = SDLK_UNKNOWN;
static long pending_seek_delta;
static bool clock_ready;
static bool playback_started;
static Uint32 playback_started_at;
static Uint32 clock_tick;
static Uint32 last_save;
static long position_seconds;
static const char *position_file;
static const char *checkpoint_file;
static bool key_repeat_enabled;
static char seek_notice[8];
static bool seek_notice_forward;
static Uint32 seek_notice_until;
static bool battery_peek_down;
static int battery_percentage;
static bool battery_charging;
static Uint32 last_battery_check;
static bool battery_style_fixed;
static int battery_text_align;
static int battery_offset_x;
static int battery_offset_y;
static bool player_config_ready;
static bool audio_mode;
static const char *artwork_file;
static const char *media_title;
static const char *duration_file;
static const char *brightness_file;
static const char *brightness_state_file;
static long duration_seconds;
static Uint32 last_duration_check;
static long timer_remaining_seconds = -1;
static Uint32 last_timer_check;
static Uint32 progress_until;
static bool progress_waiting_for_video;
static Uint32 last_activity;
static Uint32 last_external_backlight_check;
static int backlight_stage;
static bool external_backlight_off;
static long saved_brightness_raw;
static SDL_Surface *audio_artwork;
static SDL_Surface *audio_visualizer_surface;
static SDL_Surface *hardware_surface;
static bool audio_artwork_loaded;
static bool inside_present;
static SDLKey wake_key = SDLK_UNKNOWN;
static bool overlay_force_redraw = true;
static bool audio_progress_ready;
static int last_progress_knob = -1;
static int last_progress_bar_x = -1;
static int last_progress_bar_w;
static bool seek_notice_drawn;
static int seek_notice_draw_x;
static int seek_notice_draw_y;
static int seek_notice_draw_w;
static int seek_notice_draw_h;
static Uint32 last_clock_update;
static Uint8 *yuv_backup[3];
static size_t yuv_backup_capacity[3];

typedef struct {
    char text[128];
    int size;
    Uint8 red;
    Uint8 green;
    Uint8 blue;
    SDL_Surface *surface;
} OsdTextCache;

static OsdTextCache battery_text_cache;
static OsdTextCache elapsed_text_cache;
static OsdTextCache remaining_text_cache;
static OsdTextCache seek_text_cache;
static OsdTextCache audio_title_text_cache;
static OsdTextCache timer_text_cache;
static TTF_Font *osd_fonts[4];
static int osd_font_sizes[4];
static bool osd_ttf_owned;
static SDL_Surface *battery_icons[6];
static SDL_Surface *scaled_battery_icon;
static SDL_Surface *scaled_battery_source;
static int scaled_battery_target_width;

#define AUDIO_DIM_DELAY 30000
#define AUDIO_OFF_DELAY 60000
#define AUDIO_DIM_RAW 3
#define TIMER_REMAINING_FILE "/tmp/kidsmode_remaining"
#define TIMER_WARNING_SECONDS 300
#define MEDIA_DIMMED_FLAG "/tmp/kidsmode_media_dimmed"
#define MEDIA_PLAYING_FLAG "/tmp/kidsmode_media_playing"
#define MIYOO_SCANCODE_VOLUMEDOWN 114
#define MIYOO_SCANCODE_VOLUMEUP 115
#define MIYOO_DISPLAY_WIDTH 640
#define MIYOO_DISPLAY_HEIGHT 480

static void update_screen(SDL_Surface *surface, Sint32 x, Sint32 y,
                          Uint32 width, Uint32 height)
{
    if (!real_update)
        real_update = (update_fn)dlsym(RTLD_NEXT, "SDL_UpdateRect");
    if (real_update)
        real_update(surface, x, y, width, height);
}

static void update_clock(void);
static long read_number_file(const char *path);
static void draw_player_overlay(void);
static void draw_audio_progress_only(void);
static void draw_audio_timer_only(void);
static void draw_seek_notice(void);
static void draw_battery_peek(SDL_Surface *surface);
static void draw_timer_warning(SDL_Surface *surface);
static void yuv_timer_warning(SDL_Overlay *overlay);
static SDL_Surface *osd_text(OsdTextCache *cache, const char *text,
                             int size);
static SDL_Surface *osd_text_color(OsdTextCache *cache, const char *text,
                                   int size, SDL_Color color);
static void blit_osd_text(SDL_Surface *target, SDL_Surface *text,
                          int logical_x, int logical_y);
static void yuv_blit_osd_text(SDL_Overlay *overlay, SDL_Surface *text,
                              int logical_x, int logical_y);
static SDL_Surface *battery_icon(void);
static SDL_Surface *battery_icon_for_width(int target_width);
static void restore_video_overlay(SDL_Overlay *overlay);
static int text_width(const char *text, int scale);
static void format_time(long seconds, bool remaining, char *out,
                        size_t out_size);
static bool player_overlay_visible(void);

static int read_battery_percentage(void)
{
    long value = read_number_file("/tmp/percBat");
    if (value >= 0 && value <= 100)
        return (int)value;

    // Some Onion builds temporarily put the charging sentinel (500) in
    // percBat. Try the real AXP percentage before falling back to the last
    // valid value, so charging never turns into a misleading "100%".
    FILE *fp = fopen("/tmp/.axp_result", "r");
    if (fp != NULL) {
        char buf[160] = "";
        if (fgets(buf, sizeof(buf), fp) != NULL) {
            char *field = strstr(buf, "\"battery\"");
            char *separator = field != NULL ? strchr(field, ':') : NULL;
            if (separator != NULL) {
                long axp_value = strtol(separator + 1, NULL, 10);
                if (axp_value >= 0 && axp_value <= 100)
                    value = axp_value;
            }
        }
        fclose(fp);
    }
    if (value >= 0 && value <= 100)
        return (int)value;
    return battery_percentage >= 0 && battery_percentage <= 100
               ? battery_percentage
               : 0;
}

static bool read_battery_charging(void)
{
    if (read_number_file("/tmp/percBat") == 500)
        return true;
    FILE *fp = fopen("/tmp/.axp_result", "r");
    if (fp == NULL)
        return false;
    char buf[160] = "";
    bool charging = false;
    if (fgets(buf, sizeof(buf), fp) != NULL) {
        char *field = strstr(buf, "\"charging\"");
        char *separator = field != NULL ? strchr(field, ':') : NULL;
        charging = separator != NULL && atoi(separator + 1) != 0;
        if (!charging)
            charging = strstr(buf, "\"battery\":500") != NULL;
    }
    fclose(fp);
    return charging;
}

static bool update_battery_status(Uint32 now, bool force)
{
    if (!force && now - last_battery_check < 1000)
        return false;
    last_battery_check = now;
    int percentage = read_battery_percentage();
    bool charging = read_battery_charging();
    bool changed = percentage != battery_percentage ||
                   charging != battery_charging;
    battery_percentage = percentage;
    battery_charging = charging;
    return changed;
}

static bool battery_peek_visible(void)
{
    return battery_peek_down || battery_charging;
}

static int osd_size_for_width(int width, int percent)
{
    const char *configured = getenv("VC_OSD_FONT_SIZE");
    int base = configured != NULL ? atoi(configured) : 24;
    if (base < 12 || base > 48)
        base = 24;
    int size = base * width * percent / (640 * 100);
    return size < 10 ? 10 : size;
}

static void mark_playback_started(void)
{
    if (playback_started)
        return;
    playback_started = true;
    playback_started_at = SDL_GetTicks();
    if (clock_ready) {
        clock_tick = playback_started_at;
        last_save = playback_started_at;
    }
}

static void clear_audio_seek_notice(void)
{
    if (!audio_mode || !seek_notice_drawn || seek_notice_draw_w <= 0 ||
        seek_notice_draw_h <= 0)
        return;
    SDL_Surface *surface = hardware_surface != NULL
                               ? hardware_surface
                               : SDL_GetVideoSurface();
    if (surface == NULL)
        return;
    SDL_Rect old_notice = {seek_notice_draw_x, seek_notice_draw_y,
                           seek_notice_draw_w, seek_notice_draw_h};
    SDL_FillRect(surface, &old_notice,
                 SDL_MapRGB(surface->format, 0, 0, 0));
    seek_notice_drawn = false;
    seek_notice_draw_w = 0;
    seek_notice_draw_h = 0;
}

static void dismiss_seek_notice(void)
{
    // A is an explicit request for the neutral progress OSD. Remove any
    // previous seek feedback immediately instead of extending it alongside
    // the progress bar for another two seconds.
    if (audio_mode)
        clear_audio_seek_notice();
    seek_notice[0] = '\0';
    seek_notice_until = 0;
    seek_notice_drawn = false;
}

static Uint8 clamp_color(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (Uint8)value;
}

static bool save_paused_frame(void)
{
    const char *target = getenv("VC_SCREENSHOT_FILE");
    if (target == NULL || target[0] == '\0' || last_overlay == NULL)
        return false;
    if (last_overlay->format != SDL_YV12_OVERLAY &&
        last_overlay->format != SDL_IYUV_OVERLAY)
        return false;
    size_t plane_sizes[3] = {
        (size_t)last_overlay->pitches[0] * last_overlay->h,
        (size_t)last_overlay->pitches[1] * ((last_overlay->h + 1) / 2),
        (size_t)last_overlay->pitches[2] * ((last_overlay->h + 1) / 2)};
    bool use_clean_backup = last_overlay_painted;
    for (int i = 0; i < 3 && use_clean_backup; i++)
        if (yuv_backup[i] == NULL || yuv_backup_capacity[i] < plane_sizes[i])
            use_clean_backup = false;
    bool overlay_locked = false;
    if (!use_clean_backup) {
        if (SDL_LockYUVOverlay(last_overlay) != 0)
            return false;
        overlay_locked = true;
    }

    int source_w = last_overlay->w;
    int source_h = last_overlay->h;
    int output_w = source_w;
    int output_h = source_h;
    if (output_w > 640) {
        output_h = (int)((long long)output_h * 640 / output_w);
        output_w = 640;
    }
    if (output_h > 480) {
        output_w = (int)((long long)output_w * 480 / output_h);
        output_h = 480;
    }
    if (output_w < 1)
        output_w = 1;
    if (output_h < 1)
        output_h = 1;

    SDL_Surface *shot = SDL_CreateRGBSurface(
        SDL_SWSURFACE, output_w, output_h, 32, 0x00ff0000, 0x0000ff00,
        0x000000ff, 0xff000000);
    if (shot == NULL) {
        if (overlay_locked)
            SDL_UnlockYUVOverlay(last_overlay);
        return false;
    }

    Uint8 *planes[3] = {
        use_clean_backup ? yuv_backup[0] : last_overlay->pixels[0],
        use_clean_backup ? yuv_backup[1] : last_overlay->pixels[1],
        use_clean_backup ? yuv_backup[2] : last_overlay->pixels[2]};
    Uint8 *y_plane = planes[0];
    Uint8 *u_plane = last_overlay->format == SDL_YV12_OVERLAY
                         ? planes[2]
                         : planes[1];
    Uint8 *v_plane = last_overlay->format == SDL_YV12_OVERLAY
                         ? planes[1]
                         : planes[2];
    int y_pitch = last_overlay->pitches[0];
    int u_pitch = last_overlay->format == SDL_YV12_OVERLAY
                      ? last_overlay->pitches[2]
                      : last_overlay->pitches[1];
    int v_pitch = last_overlay->format == SDL_YV12_OVERLAY
                      ? last_overlay->pitches[1]
                      : last_overlay->pitches[2];

    if (SDL_MUSTLOCK(shot))
        SDL_LockSurface(shot);
    for (int y = 0; y < output_h; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)shot->pixels + y * shot->pitch);
        // FFplay is rotated 180 degrees for the Miyoo display. Reverse both
        // axes while capturing so the saved carousel image is upright.
        int source_y = source_h - 1 - (int)((long long)y * source_h / output_h);
        for (int x = 0; x < output_w; x++) {
            int source_x =
                source_w - 1 - (int)((long long)x * source_w / output_w);
            int yy = y_plane[source_y * y_pitch + source_x] - 16;
            int uu = u_plane[(source_y / 2) * u_pitch + source_x / 2] - 128;
            int vv = v_plane[(source_y / 2) * v_pitch + source_x / 2] - 128;
            if (yy < 0)
                yy = 0;
            int red = (298 * yy + 409 * vv + 128) >> 8;
            int green = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
            int blue = (298 * yy + 516 * uu + 128) >> 8;
            row[x] = SDL_MapRGB(shot->format, clamp_color(red),
                                clamp_color(green), clamp_color(blue));
        }
    }
    if (SDL_MUSTLOCK(shot))
        SDL_UnlockSurface(shot);
    if (overlay_locked)
        SDL_UnlockYUVOverlay(last_overlay);

    char temporary[2048];
    snprintf(temporary, sizeof(temporary), "%s.tmp", target);
    remove(temporary);
    bool saved = SDL_SaveBMP(shot, temporary) == 0 &&
                 rename(temporary, target) == 0;
    if (!saved)
        remove(temporary);
    SDL_FreeSurface(shot);
    return saved;
}

__attribute__((constructor)) static void vcinput_loaded(void)
{
    FILE *fp = fopen("/tmp/vcinput_loaded", "w");
    if (fp) {
        fputs("1\n", fp);
        fclose(fp);
    }
}

static long read_number_file(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return -1;
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    char value[64] = "";
    bool read_ok = fgets(value, sizeof(value), fp) != NULL;
    fclose(fp);
    if (!read_ok)
        return -1;
    char *end = NULL;
    long result = strtol(value, &end, 10);
    return end != value && result >= 0 ? result : -1;
}

static bool write_backlight(long value)
{
    if (brightness_file == NULL || brightness_file[0] == '\0' || value < 0)
        return false;
    FILE *fp = fopen(brightness_file, "w");
    if (fp == NULL)
        return false;
    bool written = fprintf(fp, "%ld\n", value) > 0;
    fclose(fp);
    return written;
}

static void set_media_dimmed_flag(bool enabled)
{
    if (!enabled) {
        remove(MEDIA_DIMMED_FLAG);
        return;
    }
    FILE *fp = fopen(MEDIA_DIMMED_FLAG, "w");
    if (fp != NULL)
        fclose(fp);
}

static void set_media_playing_flag(bool enabled)
{
    if (!enabled) {
        remove(MEDIA_PLAYING_FLAG);
        return;
    }
    FILE *fp = fopen(MEDIA_PLAYING_FLAG, "w");
    if (fp != NULL)
        fclose(fp);
}

static void save_brightness_choice(long value)
{
    if (value <= 0)
        return;
    saved_brightness_raw = value;
    if (brightness_state_file == NULL || brightness_state_file[0] == '\0')
        return;
    FILE *fp = fopen(brightness_state_file, "w");
    if (fp != NULL) {
        fprintf(fp, "%ld\n", value);
        fclose(fp);
    }
}

static void restore_backlight(void)
{
    if (backlight_stage != 0 && saved_brightness_raw > 0)
        write_backlight(saved_brightness_raw);
    backlight_stage = 0;
    external_backlight_off = false;
    set_media_dimmed_flag(false);
}

__attribute__((destructor)) static void vcinput_unloaded(void)
{
    // Do not reveal FFplay's final frame while Onion is preparing its
    // shutdown splash. The shell restores brightness only after that splash
    // has been painted completely.
    remove(MEDIA_DIMMED_FLAG);
    remove(MEDIA_PLAYING_FLAG);
    if (access("/tmp/.offOrder", F_OK) != 0 &&
        access("/tmp/shutting_down", F_OK) != 0)
        restore_backlight();
    for (int i = 0; i < 3; i++) {
        free(yuv_backup[i]);
        yuv_backup[i] = NULL;
        yuv_backup_capacity[i] = 0;
    }
    OsdTextCache *caches[] = {&battery_text_cache, &elapsed_text_cache,
                              &remaining_text_cache, &seek_text_cache,
                              &audio_title_text_cache, &timer_text_cache};
    for (size_t i = 0; i < sizeof(caches) / sizeof(caches[0]); i++) {
        SDL_FreeSurface(caches[i]->surface);
        caches[i]->surface = NULL;
    }
    for (int i = 0; i < 4; i++) {
        if (osd_fonts[i] != NULL)
            TTF_CloseFont(osd_fonts[i]);
        osd_fonts[i] = NULL;
    }
    SDL_FreeSurface(scaled_battery_icon);
    scaled_battery_icon = NULL;
    scaled_battery_source = NULL;
    for (int i = 0; i < 6; i++) {
        SDL_FreeSurface(battery_icons[i]);
        battery_icons[i] = NULL;
    }
    if (osd_ttf_owned)
        TTF_Quit();
}

static void load_player_config(Uint32 now)
{
    if (player_config_ready)
        return;
    const char *kind = getenv("VC_MEDIA_KIND");
    audio_mode = kind != NULL && strcmp(kind, "audio") == 0;
    const char *fixed = getenv("VC_BATTERY_FIXED");
    const char *align = getenv("VC_BATTERY_TEXT_ALIGN");
    const char *offset_x = getenv("VC_BATTERY_OFFSET_X");
    const char *offset_y = getenv("VC_BATTERY_OFFSET_Y");
    battery_style_fixed = fixed != NULL && strcmp(fixed, "true") == 0;
    battery_text_align = align != NULL && strcasecmp(align, "right") == 0
                             ? 1
                             : (align != NULL &&
                                        strcasecmp(align, "center") == 0
                                    ? 0
                                    : -1);
    battery_offset_x = offset_x != NULL ? atoi(offset_x) : 0;
    battery_offset_y = offset_y != NULL ? atoi(offset_y) : 0;
    set_media_playing_flag(true);
    update_battery_status(now, true);
    artwork_file = getenv("VC_ARTWORK_FILE");
    media_title = getenv("VC_MEDIA_TITLE");
    duration_file = getenv("VC_DURATION_FILE");
    brightness_file = getenv("VC_BRIGHTNESS_FILE");
    brightness_state_file = getenv("VC_BRIGHTNESS_STATE_FILE");
    const char *brightness = getenv("VC_BRIGHTNESS_RESTORE");
    saved_brightness_raw = brightness ? strtol(brightness, NULL, 10) : -1;
    if (saved_brightness_raw <= 0)
        saved_brightness_raw = read_number_file(brightness_file);
    save_brightness_choice(saved_brightness_raw);
    last_activity = now;
    player_config_ready = true;
}

static void update_duration(Uint32 now)
{
    if (duration_seconds > 0 || now - last_duration_check < 250)
        return;
    last_duration_check = now;
    long value = read_number_file(duration_file);
    if (value > 0)
        duration_seconds = value;
}

static bool timer_warning_visible(void)
{
    return timer_remaining_seconds > 0 &&
           timer_remaining_seconds <= TIMER_WARNING_SECONDS &&
           backlight_stage != 2;
}

static bool update_timer_status(Uint32 now, bool force)
{
    if (!force && now - last_timer_check < 1000)
        return false;
    last_timer_check = now;
    bool was_visible = timer_remaining_seconds > 0 &&
                       timer_remaining_seconds <= TIMER_WARNING_SECONDS;
    long old_minutes = (timer_remaining_seconds + 59) / 60;
    long remaining = read_number_file(TIMER_REMAINING_FILE);
    bool is_visible = remaining > 0 && remaining <= TIMER_WARNING_SECONDS;
    long new_minutes = (remaining + 59) / 60;
    bool changed = was_visible != is_visible ||
                   (is_visible && old_minutes != new_minutes);
    timer_remaining_seconds = remaining;
    return changed;
}

static void update_backlight(Uint32 now)
{
    // Audio playback may sleep while it keeps playing. Video playback may
    // sleep only while paused; an actively playing video always stays lit.
    if ((!audio_mode && !paused) || brightness_file == NULL ||
        saved_brightness_raw <= 0)
        return;
    Uint32 idle = now - last_activity;

    // POWER remains owned by Onion even while the player is dimmed or black.
    // Since /tmp/stay_awake keeps playback running, observe Onion's own
    // backlight transition and park our timer until it restores the display.
    if (now - last_external_backlight_check >= 250) {
        last_external_backlight_check = now;
        long current = read_number_file(brightness_file);

        if (external_backlight_off) {
            if (current > 0) {
                external_backlight_off = false;
                backlight_stage = 0;
                save_brightness_choice(current);
                last_activity = now;
                set_media_dimmed_flag(false);
            }
            else {
                last_activity = now;
            }
            return;
        }

        if (backlight_stage == 0 && current == 0) {
            external_backlight_off = true;
            last_activity = now;
            return;
        }
        if (backlight_stage == 1) {
            if (current == 0) {
                external_backlight_off = true;
                last_activity = now;
                return;
            }
            if (current > AUDIO_DIM_RAW) {
                backlight_stage = 0;
                save_brightness_choice(current);
                last_activity = now;
                set_media_dimmed_flag(false);
            }
        }
        else if (backlight_stage == 2 && current > 0) {
            backlight_stage = 0;
            save_brightness_choice(current);
            last_activity = now;
            set_media_dimmed_flag(false);
        }
    }

    idle = now - last_activity;
    if (backlight_stage == 0 && idle >= AUDIO_DIM_DELAY) {
        long current = read_number_file(brightness_file);
        if (current > 0)
            save_brightness_choice(current);
        if (write_backlight(AUDIO_DIM_RAW)) {
            backlight_stage = 1;
            set_media_dimmed_flag(true);
        }
    }
    if (backlight_stage == 1 && idle >= AUDIO_OFF_DELAY) {
        if (write_backlight(0)) {
            backlight_stage = 2;
            set_media_dimmed_flag(true);
        }
    }
}

static void write_position_value(const char *path, long value)
{
    if (!path || !*path)
        return;
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%ld\n", value < 0 ? 0 : value);
        fclose(fp);
    }
}

static void save_position(void)
{
    write_position_value(position_file, position_seconds);
}

static void save_checkpoint(void)
{
    // Keep a small safety rewind for both MENU exits and power-loss resumes,
    // so playback never skips content around the point where viewing stopped.
    write_position_value(checkpoint_file, position_seconds - 2);
}

static void mark_menu_exit(void)
{
    FILE *fp = fopen("/tmp/kidsmode_video_menu_exit", "w");
    if (fp) {
        fputs("1\n", fp);
        fclose(fp);
    }
}

static void update_clock(void)
{
    Uint32 now = SDL_GetTicks();
    // FFplay can enter several interposed SDL functions for every decoded
    // frame. The clock, backlight and save bookkeeping do not need to run at
    // video-frame frequency. Capping this work also keeps the carousel/player
    // responsive on the Miyoo's small CPU.
    if (clock_ready && now - last_clock_update < 100)
        return;
    if (last_clock_update != 0 && now - last_clock_update > 1000 &&
        (audio_mode || paused)) {
        long current = read_number_file(brightness_file);
        if (current > 0)
            save_brightness_choice(current);
        backlight_stage = 0;
        external_backlight_off = false;
        set_media_dimmed_flag(false);
        last_activity = now;
    }
    last_clock_update = now;
    long previous_second = position_seconds;
    load_player_config(now);
    update_duration(now);
    update_backlight(now);
    bool battery_changed = update_battery_status(now, false);
    bool timer_changed = update_timer_status(now, false);
    if (!clock_ready) {
        const char *start = getenv("VC_START_SECONDS");
        position_seconds = start ? strtol(start, NULL, 10) : 0;
        position_file = getenv("VC_POSITION_FILE");
        checkpoint_file = getenv("VC_CHECKPOINT_FILE");
        clock_tick = last_save = playback_started ? playback_started_at : now;
        clock_ready = true;
    }
    // Do not count FFplay's input probing and decoder setup as watched time.
    // A restart must remain at 0:00 until audio really starts or the first
    // video frame is presented.
    if (!playback_started) {
        clock_tick = now;
        return;
    }
    if (!paused && now - clock_tick >= 1000) {
        position_seconds += (now - clock_tick) / 1000;
        clock_tick += ((now - clock_tick) / 1000) * 1000;
    } else if (paused) {
        clock_tick = now;
    }
    if (now - last_save >= 1000) {
        save_position();
        if ((now / 5000) != (last_save / 5000))
            save_checkpoint();
        last_save = now;
    }
    if (battery_changed || timer_changed) {
        if (audio_mode && !inside_present && timer_changed &&
            !battery_changed) {
            draw_audio_timer_only();
        }
        else if (audio_mode && !inside_present) {
            overlay_force_redraw = true;
            draw_player_overlay();
        }
        else if (paused && last_overlay != NULL && last_overlay_rect_ready &&
                 real_overlay != NULL) {
            if (last_overlay_painted) {
                restore_video_overlay(last_overlay);
                last_overlay_painted = false;
            }
            real_overlay(last_overlay, &last_overlay_rect);
            draw_player_overlay();
        }
    }
    if (audio_mode && backlight_stage != 2 && !inside_present &&
        position_seconds != previous_second) {
        if (overlay_force_redraw)
            draw_player_overlay();
        else
            draw_audio_progress_only();
    }
    // A paused audio file has no advancing second to trigger the normal
    // incremental redraw. Still remove an expired seek message on time.
    if (audio_mode && !inside_present && seek_notice_drawn &&
        now >= seek_notice_until)
        draw_seek_notice();
    // A paused video has no next decoded frame to erase an expired seek
    // message. Re-present the already paused frame once, then paint only the
    // still-valid progress controls over it.
    if (!audio_mode && paused && seek_notice[0] != '\0' &&
        now >= seek_notice_until && last_overlay != NULL &&
        last_overlay_rect_ready && real_overlay != NULL) {
        seek_notice[0] = '\0';
        if (last_overlay_painted) {
            restore_video_overlay(last_overlay);
            last_overlay_painted = false;
        }
        real_overlay(last_overlay, &last_overlay_rect);
        draw_player_overlay();
    }
}

static void key(SDL_Event *event, SDLKey sym, Uint8 state)
{
    memset(event, 0, sizeof(*event));
    event->type = state == SDL_PRESSED ? SDL_KEYDOWN : SDL_KEYUP;
    event->key.type = event->type;
    event->key.state = state;
    event->key.keysym.sym = sym;
}

static void ensure_key_repeat(void)
{
    if (!key_repeat_enabled) {
        // SDL 1.2 does not repeat held keys unless the application enables
        // it. Onion's FFplay leaves it disabled on some builds.
        SDL_EnableKeyRepeat(300, 100);
        key_repeat_enabled = true;
    }
}

static const unsigned char *glyph_rows(char c)
{
    static const unsigned char plus[7] = {0, 4, 4, 31, 4, 4, 0};
    static const unsigned char minus[7] = {0, 0, 0, 31, 0, 0, 0};
    static const unsigned char zero[7] = {14, 17, 19, 21, 25, 17, 14};
    static const unsigned char one[7] = {4, 12, 4, 4, 4, 4, 14};
    static const unsigned char two[7] = {14, 17, 1, 2, 4, 8, 31};
    static const unsigned char three[7] = {30, 1, 1, 14, 1, 1, 30};
    static const unsigned char four[7] = {2, 6, 10, 18, 31, 2, 2};
    static const unsigned char five[7] = {31, 16, 16, 30, 1, 1, 30};
    static const unsigned char six[7] = {14, 16, 16, 30, 17, 17, 14};
    static const unsigned char seven[7] = {31, 1, 2, 4, 8, 8, 8};
    static const unsigned char eight[7] = {14, 17, 17, 14, 17, 17, 14};
    static const unsigned char nine[7] = {14, 17, 17, 15, 1, 1, 14};
    static const unsigned char colon[7] = {0, 4, 4, 0, 4, 4, 0};
    static const unsigned char ess[7] = {0, 0, 15, 16, 14, 1, 30};
    static const unsigned char em[7] = {0, 0, 26, 21, 21, 21, 21};
    static const unsigned char letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14},      {7,2,2,2,2,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},   {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},     {31,1,2,4,8,16,31}};
    static const unsigned char dot[7] = {0,0,0,0,0,4,4};
    static const unsigned char apostrophe[7] = {4,4,2,0,0,0,0};
    static const unsigned char question[7] = {14,17,1,2,4,0,4};
    if (c == 's')
        return ess;
    if (c == 'm')
        return em;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return letters[c - 'A'];
    switch (c) {
    case '+': return plus;
    case '-': return minus;
    case '0': return zero;
    case '1': return one;
    case '2': return two;
    case '3': return three;
    case '4': return four;
    case '5': return five;
    case '6': return six;
    case '7': return seven;
    case '8': return eight;
    case '9': return nine;
    case ':': return colon;
    case '.': return dot;
    case '\'': return apostrophe;
    case '?': return question;
    default: return NULL;
    }
}

static bool ensure_yuv_backup(int plane, size_t size)
{
    if (yuv_backup_capacity[plane] >= size)
        return true;
    Uint8 *grown = realloc(yuv_backup[plane], size);
    if (grown == NULL)
        return false;
    yuv_backup[plane] = grown;
    yuv_backup_capacity[plane] = size;
    return true;
}

static void yuv_rect(SDL_Overlay *overlay, int x, int y, int w, int h,
                     Uint8 yy, Uint8 uu, Uint8 vv)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > overlay->w) w = overlay->w - x;
    if (y + h > overlay->h) h = overlay->h - y;
    if (w <= 0 || h <= 0)
        return;
    Uint8 *u_plane = overlay->format == SDL_YV12_OVERLAY
                         ? overlay->pixels[2]
                         : overlay->pixels[1];
    Uint8 *v_plane = overlay->format == SDL_YV12_OVERLAY
                         ? overlay->pixels[1]
                         : overlay->pixels[2];
    int u_pitch = overlay->format == SDL_YV12_OVERLAY
                      ? overlay->pitches[2]
                      : overlay->pitches[1];
    int v_pitch = overlay->format == SDL_YV12_OVERLAY
                      ? overlay->pitches[1]
                      : overlay->pitches[2];
    for (int row = y; row < y + h; row++)
        memset(overlay->pixels[0] + row * overlay->pitches[0] + x, yy,
               (size_t)w);
    int uv_x = x / 2;
    int uv_right = (x + w + 1) / 2;
    int uv_y = y / 2;
    int uv_bottom = (y + h + 1) / 2;
    for (int row = uv_y; row < uv_bottom; row++) {
        memset(u_plane + row * u_pitch + uv_x, uu,
               (size_t)(uv_right - uv_x));
        memset(v_plane + row * v_pitch + uv_x, vv,
               (size_t)(uv_right - uv_x));
    }
}

static void yuv_logical_rect(SDL_Overlay *overlay, int x, int y, int w,
                             int h, Uint8 yy, Uint8 uu, Uint8 vv)
{
    yuv_rect(overlay, overlay->w - x - w, overlay->h - y - h, w, h,
             yy, uu, vv);
}

// FFplay's YUV surface can be 320, 640, 1280 or 1920 pixels wide, and its
// destination rectangle can be smaller than the 640x480 framebuffer. Convert
// display-pixel measurements back into YUV pixels so the OSD has one stable
// on-screen size instead of changing with every video's encoded resolution.
static int yuv_from_screen_x(SDL_Overlay *overlay, int pixels)
{
    int destination_width = last_overlay_rect_ready && last_overlay_rect.w > 0
                                ? last_overlay_rect.w
                                : MIYOO_DISPLAY_WIDTH;
    int result = pixels * overlay->w / destination_width;
    return result < 1 ? 1 : result;
}

static int yuv_from_screen_y(SDL_Overlay *overlay, int pixels)
{
    int destination_height = last_overlay_rect_ready && last_overlay_rect.h > 0
                                 ? last_overlay_rect.h
                                 : MIYOO_DISPLAY_HEIGHT;
    int result = pixels * overlay->h / destination_height;
    return result < 1 ? 1 : result;
}

static int yuv_osd_font_size(SDL_Overlay *overlay)
{
    int display_size = osd_size_for_width(MIYOO_DISPLAY_WIDTH, 100);
    return yuv_from_screen_x(overlay, display_size);
}

static void yuv_battery_peek(SDL_Overlay *overlay)
{
    if (!battery_peek_visible())
        return;
    int icon_reference_width =
        yuv_from_screen_x(overlay, MIYOO_DISPLAY_WIDTH);
    SDL_Surface *icon = battery_icon_for_width(icon_reference_width);
    char text[8];
    snprintf(text, sizeof(text), "%d%%", battery_percentage);
    SDL_Surface *label = osd_text(&battery_text_cache, text,
                                  yuv_osd_font_size(overlay));
    if (icon == NULL && label == NULL)
        return;
    int spacer = icon != NULL && label != NULL
                     ? yuv_from_screen_x(overlay, 5)
                     : 0;
    int header_center = yuv_from_screen_x(overlay, 596);
    int center_y = yuv_from_screen_y(overlay, 30);
    int icon_x = icon != NULL ? header_center - icon->w / 2 : header_center;
    int label_x = icon_x;
    if (icon != NULL && label != NULL) {
        int offset_x = battery_offset_x == 0
                           ? 0
                           : yuv_from_screen_x(overlay,
                                               abs(battery_offset_x));
        if (battery_offset_x < 0)
            offset_x = -offset_x;
        if (battery_style_fixed) {
            if (battery_text_align > 0)
                label_x = icon_x + icon->w - label->w + offset_x;
            else if (battery_text_align == 0)
                label_x = icon_x + (icon->w - label->w) / 2 + offset_x;
            else
                label_x = icon_x + offset_x;
        }
        else
            label_x = icon_x + icon->w + spacer + offset_x;
    }
    int label_offset_y = battery_offset_y;
    const char *font_path = getenv("VC_OSD_FONT");
    if (font_path != NULL && strstr(font_path, "Exo-2") != NULL)
        label_offset_y -=
            (int)(0.15 * osd_size_for_width(MIYOO_DISPLAY_WIDTH, 100));
    int scaled_offset_y = label_offset_y == 0
                              ? 0
                              : yuv_from_screen_y(overlay,
                                                  abs(label_offset_y));
    if (label_offset_y < 0)
        scaled_offset_y = -scaled_offset_y;
    if (icon != NULL) {
        yuv_blit_osd_text(overlay, icon, icon_x,
                          center_y - icon->h / 2);
    }
    if (label != NULL)
        yuv_blit_osd_text(overlay, label, label_x,
                          center_y - label->h / 2 + scaled_offset_y);
}

static void format_timer_warning(char *text, size_t text_size)
{
    long minutes = (timer_remaining_seconds + 59) / 60;
    snprintf(text, text_size, "%ld min", minutes);
}

static void yuv_timer_warning(SDL_Overlay *overlay)
{
    if (overlay == NULL || !timer_warning_visible())
        return;
    char text[16];
    format_timer_warning(text, sizeof(text));
    SDL_Color red = {255, 64, 64, 255};
    SDL_Surface *label = osd_text_color(
        &timer_text_cache, text, yuv_osd_font_size(overlay), red);
    if (label != NULL)
        yuv_blit_osd_text(overlay, label,
                          yuv_from_screen_x(overlay, 18),
                          yuv_from_screen_y(overlay, 18));
}

static void yuv_progress_knob(SDL_Overlay *overlay, int logical_x,
                              int logical_y, int radius)
{
    int cx = overlay->w - logical_x;
    int cy = overlay->h - logical_y;
    int border = yuv_from_screen_x(overlay, 2);
    int outer_radius = radius + border;
    for (int dy = -outer_radius; dy <= outer_radius; dy++)
        for (int dx = -outer_radius; dx <= outer_radius; dx++) {
            int distance = dx * dx + dy * dy;
            if (distance <= outer_radius * outer_radius)
                yuv_rect(overlay, cx + dx, cy + dy, 1, 1,
                         16, 128, 128);
        }
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
            if (dx * dx + dy * dy <= radius * radius)
                yuv_rect(overlay, cx + dx, cy + dy, 1, 1,
                         122, 193, 160);
}

static void paint_video_yuv_overlay(SDL_Overlay *overlay)
{
    if (!player_overlay_visible())
        return;
    Uint32 now = SDL_GetTicks();
    bool controls_visible = duration_seconds > 0 &&
        (paused || progress_waiting_for_video || now < progress_until ||
         (seek_notice[0] != '\0' && now < seek_notice_until));
    if (!controls_visible) {
        yuv_timer_warning(overlay);
        yuv_battery_peek(overlay);
        return;
    }
    long elapsed = position_seconds;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > duration_seconds) elapsed = duration_seconds;
    char elapsed_text[24], remaining_text[24];
    format_time(elapsed, false, elapsed_text, sizeof(elapsed_text));
    format_time(duration_seconds - elapsed, true, remaining_text,
                sizeof(remaining_text));
    int font_size = yuv_osd_font_size(overlay);
    SDL_Surface *elapsed_label = osd_text(&elapsed_text_cache, elapsed_text,
                                          font_size);
    SDL_Surface *remaining_label = osd_text(&remaining_text_cache,
                                            remaining_text, font_size);
    if (elapsed_label == NULL || remaining_label == NULL)
        return;
    int label_h = elapsed_label->h > remaining_label->h
                      ? elapsed_label->h
                      : remaining_label->h;
    int bar_y = overlay->h - yuv_from_screen_y(overlay, 28);
    int logical_y = bar_y - label_h / 2 -
                    yuv_from_screen_y(overlay, 6);
    int margin = yuv_from_screen_x(overlay, 16);
    int label_gap = yuv_from_screen_x(overlay, 16);
    int elapsed_width = elapsed_label->w;
    int remaining_width = remaining_label->w;
    int bar_x = margin + elapsed_width + label_gap;
    int bar_right = overlay->w - margin - remaining_width - label_gap;
    int bar_w = bar_right - bar_x;
    if (bar_w < overlay->w / 4) {
        bar_x = overlay->w / 4;
        bar_w = overlay->w / 2;
    }
    yuv_blit_osd_text(overlay, elapsed_label, margin, logical_y);
    yuv_blit_osd_text(overlay, remaining_label,
                      overlay->w - margin - remaining_width, logical_y);
    yuv_logical_rect(overlay, bar_x,
                     bar_y - yuv_from_screen_y(overlay, 2), bar_w,
                     yuv_from_screen_y(overlay, 5),
                     16, 128, 128);
    yuv_logical_rect(overlay, bar_x,
                     bar_y - yuv_from_screen_y(overlay, 1), bar_w,
                     yuv_from_screen_y(overlay, 3),
                     122, 193, 160);
    int knob_x = bar_x +
        (int)((long long)bar_w * elapsed / duration_seconds);
    yuv_progress_knob(overlay, knob_x, bar_y,
                      yuv_from_screen_x(overlay, 5));

    if (seek_notice[0] != '\0' && SDL_GetTicks() < seek_notice_until) {
        SDL_Surface *notice = osd_text(
            &seek_text_cache, seek_notice,
            yuv_from_screen_x(
                overlay,
                osd_size_for_width(MIYOO_DISPLAY_WIDTH, 100)));
        int width = notice != NULL ? notice->w : 0;
        int logical_x = seek_notice_forward
                            ? overlay->w - width -
                                  yuv_from_screen_x(overlay, 18)
                            : yuv_from_screen_x(overlay, 18);
        // Match the framebuffer path used while paused. Its physical offset
        // is 35 base-scale pixels from the panel edge; account for the glyph
        // height when expressing that same point in logical coordinates.
        int logical_y = overlay->h - yuv_from_screen_y(overlay, 70) -
                        (notice != NULL ? notice->h : 0);
        yuv_blit_osd_text(overlay, notice, logical_x, logical_y);
    }
    yuv_timer_warning(overlay);
    yuv_battery_peek(overlay);
}

static bool backup_and_paint_video_overlay(SDL_Overlay *overlay)
{
    if (overlay == NULL ||
        (overlay->format != SDL_YV12_OVERLAY &&
         overlay->format != SDL_IYUV_OVERLAY) ||
        SDL_LockYUVOverlay(overlay) != 0)
        return false;
    size_t sizes[3] = {
        (size_t)overlay->pitches[0] * overlay->h,
        (size_t)overlay->pitches[1] * ((overlay->h + 1) / 2),
        (size_t)overlay->pitches[2] * ((overlay->h + 1) / 2)};
    bool ready = true;
    for (int i = 0; i < 3; i++)
        if (!ensure_yuv_backup(i, sizes[i]))
            ready = false;
    if (ready) {
        for (int i = 0; i < 3; i++)
            memcpy(yuv_backup[i], overlay->pixels[i], sizes[i]);
        paint_video_yuv_overlay(overlay);
    }
    SDL_UnlockYUVOverlay(overlay);
    return ready;
}

static void restore_video_overlay(SDL_Overlay *overlay)
{
    if (overlay == NULL || SDL_LockYUVOverlay(overlay) != 0)
        return;
    size_t sizes[3] = {
        (size_t)overlay->pitches[0] * overlay->h,
        (size_t)overlay->pitches[1] * ((overlay->h + 1) / 2),
        (size_t)overlay->pitches[2] * ((overlay->h + 1) / 2)};
    for (int i = 0; i < 3; i++)
        if (yuv_backup[i] != NULL && yuv_backup_capacity[i] >= sizes[i])
            memcpy(overlay->pixels[i], yuv_backup[i], sizes[i]);
    SDL_UnlockYUVOverlay(overlay);
}

static void draw_glyph(SDL_Surface *surface, char c, int x, int y, int scale,
                       Uint32 black, Uint32 white)
{
    const unsigned char *rows = glyph_rows(c);
    if (!rows)
        return;
    for (int pass = 0; pass < 2; pass++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                // FFplay's Miyoo framebuffer is physically rotated 180°.
                // Draw every glyph pre-rotated so it appears upright.
                if (!(rows[6 - row] & (1 << col)))
                    continue;
                SDL_Rect rect = pass == 0
                                    ? (SDL_Rect){x + col * scale - 1,
                                                 y + row * scale - 1,
                                                 scale + 2, scale + 2}
                                    : (SDL_Rect){x + col * scale,
                                                 y + row * scale,
                                                 scale, scale};
                SDL_FillRect(surface, &rect, pass == 0 ? black : white);
            }
}

static int text_width(const char *text, int scale)
{
    int length = text == NULL ? 0 : (int)strlen(text);
    return length > 0 ? length * 6 * scale - scale : 0;
}

static void draw_logical_rect(SDL_Surface *surface, int x, int y, int w,
                              int h, Uint32 color)
{
    SDL_Rect physical = {surface->w - x - w, surface->h - y - h, w, h};
    SDL_FillRect(surface, &physical, color);
}

static void draw_rotated_text(SDL_Surface *surface, const char *text,
                              int logical_x, int logical_y, int scale,
                              Uint32 black, Uint32 color)
{
    int length = (int)strlen(text);
    int width = text_width(text, scale);
    int physical_x = surface->w - logical_x - width;
    int physical_y = surface->h - logical_y - 7 * scale;
    for (int i = 0; i < length; i++)
        draw_glyph(surface, text[length - 1 - i],
                   physical_x + i * 6 * scale, physical_y, scale, black,
                   color);
}

static void draw_battery_peek(SDL_Surface *surface)
{
    if (surface == NULL || !battery_peek_visible())
        return;
    SDL_Surface *icon = battery_icon_for_width(surface->w);
    char text[8];
    snprintf(text, sizeof(text), "%d%%", battery_percentage);
    SDL_Surface *label = osd_text(&battery_text_cache, text,
                                  osd_size_for_width(surface->w, 100));
    if (icon == NULL && label == NULL)
        return;
    int spacer = icon != NULL && label != NULL ? 5 * surface->w / 640 : 0;
    int header_center = surface->w * 596 / 640;
    int center_y = surface->w * 30 / 640;
    int icon_x = icon != NULL ? header_center - icon->w / 2 : header_center;
    int label_x = icon_x;
    if (icon != NULL && label != NULL) {
        int offset_x = battery_offset_x * surface->w / 640;
        if (battery_style_fixed) {
            if (battery_text_align > 0)
                label_x = icon_x + icon->w - label->w + offset_x;
            else if (battery_text_align == 0)
                label_x = icon_x + (icon->w - label->w) / 2 + offset_x;
            else
                label_x = icon_x + offset_x;
        }
        else
            label_x = icon_x + icon->w + spacer + offset_x;
    }
    int label_offset_y = battery_offset_y;
    const char *font_path = getenv("VC_OSD_FONT");
    if (font_path != NULL && strstr(font_path, "Exo-2") != NULL)
        label_offset_y -=
            (int)(0.15 * osd_size_for_width(surface->w, 100));
    label_offset_y = label_offset_y * surface->w / 640;
    if (icon != NULL) {
        blit_osd_text(surface, icon, icon_x, center_y - icon->h / 2);
    }
    if (label != NULL)
        blit_osd_text(surface, label, label_x,
                      center_y - label->h / 2 + label_offset_y);
}

static void draw_timer_warning(SDL_Surface *surface)
{
    if (surface == NULL || !timer_warning_visible())
        return;
    char text[16];
    format_timer_warning(text, sizeof(text));
    SDL_Color red = {255, 64, 64, 255};
    SDL_Surface *label = osd_text_color(
        &timer_text_cache, text, osd_size_for_width(surface->w, 100), red);
    if (label != NULL)
        blit_osd_text(surface, label, 18 * surface->w / 640,
                      18 * surface->w / 640);
}

static void make_audio_title(char *out, size_t out_size, int max_chars)
{
    if (out_size == 0)
        return;
    out[0] = '\0';
    if (media_title == NULL || media_title[0] == '\0' || max_chars <= 0)
        return;
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)media_title;
    while (*p && used + 1 < out_size && (int)used < max_chars) {
        unsigned char c = *p++;
        char mapped = '?';
        if (c < 0x80) {
            mapped = c == '_' ? ' ' : (char)c;
            if (mapped >= 'a' && mapped <= 'z')
                mapped = (char)(mapped - 'a' + 'A');
        }
        else if (c == 0xc3 && *p) {
            // Common French accents, kept readable by their base letter.
            unsigned char accent = *p++;
            if (accent == 0xa0 || accent == 0xa2 || accent == 0xa4 ||
                accent == 0x80 || accent == 0x82 || accent == 0x84)
                mapped = 'A';
            else if (accent == 0xa7 || accent == 0x87)
                mapped = 'C';
            else if ((accent >= 0xa8 && accent <= 0xab) ||
                     (accent >= 0x88 && accent <= 0x8b))
                mapped = 'E';
            else if ((accent >= 0xac && accent <= 0xaf) ||
                     (accent >= 0x8c && accent <= 0x8f))
                mapped = 'I';
            else if ((accent >= 0xb2 && accent <= 0xb6) ||
                     (accent >= 0x92 && accent <= 0x96))
                mapped = 'O';
            else if ((accent >= 0xb9 && accent <= 0xbc) ||
                     (accent >= 0x99 && accent <= 0x9c))
                mapped = 'U';
        }
        else {
            while ((*p & 0xc0) == 0x80)
                p++;
        }
        out[used++] = mapped;
    }
    out[used] = '\0';
    if (*p && used >= 3) {
        out[used - 3] = '.';
        out[used - 2] = '.';
        out[used - 1] = '.';
    }
}

static void format_time(long seconds, bool remaining, char *out,
                        size_t out_size)
{
    if (seconds < 0)
        seconds = 0;
    long hours = seconds / 3600;
    long minutes = (seconds % 3600) / 60;
    long secs = seconds % 60;
    if (hours > 0)
        snprintf(out, out_size, "%s%ld:%02ld:%02ld", remaining ? "-" : "",
                 hours, minutes, secs);
    else
        snprintf(out, out_size, "%s%ld:%02ld", remaining ? "-" : "",
                 minutes, secs);
}

static void rotate_surface_180(SDL_Surface *surface)
{
    if (surface == NULL || surface->format->BytesPerPixel != 4)
        return;
    bool locked = SDL_MUSTLOCK(surface);
    if (locked && SDL_LockSurface(surface) != 0)
        return;
    int count = surface->w * surface->h;
    for (int i = 0; i < count / 2; i++) {
        int x1 = i % surface->w;
        int y1 = i / surface->w;
        int opposite = count - 1 - i;
        int x2 = opposite % surface->w;
        int y2 = opposite / surface->w;
        Uint32 *p1 = (Uint32 *)((Uint8 *)surface->pixels +
                                y1 * surface->pitch) + x1;
        Uint32 *p2 = (Uint32 *)((Uint8 *)surface->pixels +
                                y2 * surface->pitch) + x2;
        Uint32 swap = *p1;
        *p1 = *p2;
        *p2 = swap;
    }
    if (locked)
        SDL_UnlockSurface(surface);
}

static bool battery_icon_path(const char *name, char *out, size_t out_size)
{
    const char *theme = getenv("VC_THEME_PATH");
    const char *roots[3] = {
        "/mnt/SDCARD/Saves/CurrentProfile/theme",
        theme != NULL && theme[0] != '\0' ? theme : "/mnt/SDCARD/miyoo/app",
        "/mnt/SDCARD/miyoo/app"};
    for (int i = 0; i < 3; i++) {
        snprintf(out, out_size, "%s%s/skin/%s.png", roots[i],
                 roots[i][strlen(roots[i]) - 1] == '/' ? "" : "/", name);
        if (access(out, R_OK) == 0)
            return true;
    }
    out[0] = '\0';
    return false;
}

static SDL_Surface *battery_icon(void)
{
    int index;
    const char *name;
    if (battery_charging) {
        index = 5;
        name = "ic-power-charge-100%";
    }
    else if (battery_percentage < 5) {
        index = 0;
        name = "power-0%-icon";
    }
    else if (battery_percentage < 30) {
        index = 1;
        name = "power-20%-icon";
    }
    else if (battery_percentage < 60) {
        index = 2;
        name = "power-50%-icon";
    }
    else if (battery_percentage < 90) {
        index = 3;
        name = "power-80%-icon";
    }
    else {
        index = 4;
        name = "power-full-icon";
    }
    if (battery_icons[index] != NULL)
        return battery_icons[index];
    char path[512];
    if (!battery_icon_path(name, path, sizeof(path)) && index == 4 &&
        !battery_icon_path("power-full-icon_back", path, sizeof(path)))
        return NULL;
    if (path[0] == '\0')
        return NULL;
    SDL_Surface *raw = IMG_Load(path);
    if (raw == NULL)
        return NULL;
    if (raw->format->BytesPerPixel == 4) {
        battery_icons[index] = raw;
    }
    else {
        battery_icons[index] = SDL_CreateRGBSurface(
            SDL_SWSURFACE, raw->w, raw->h, 32, 0x00ff0000, 0x0000ff00,
            0x000000ff, 0xff000000);
        if (battery_icons[index] != NULL) {
            SDL_FillRect(battery_icons[index], NULL,
                         SDL_MapRGBA(battery_icons[index]->format,
                                     0, 0, 0, 0));
            SDL_BlitSurface(raw, NULL, battery_icons[index], NULL);
        }
        SDL_FreeSurface(raw);
    }
    if (battery_icons[index] != NULL) {
        rotate_surface_180(battery_icons[index]);
        SDL_SetAlpha(battery_icons[index], SDL_SRCALPHA, 255);
    }
    return battery_icons[index];
}

static SDL_Surface *battery_icon_for_width(int target_width)
{
    SDL_Surface *source = battery_icon();
    if (source == NULL || target_width <= 0)
        return source;
    if (target_width == 640)
        return source;
    if (scaled_battery_icon != NULL && scaled_battery_source == source &&
        scaled_battery_target_width == target_width)
        return scaled_battery_icon;

    SDL_FreeSurface(scaled_battery_icon);
    scaled_battery_icon = NULL;
    scaled_battery_source = source;
    scaled_battery_target_width = target_width;

    int width = source->w * target_width / 640;
    int height = source->h * target_width / 640;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    scaled_battery_icon = SDL_CreateRGBSurface(
        SDL_SWSURFACE, width, height, 32, 0x00ff0000, 0x0000ff00,
        0x000000ff, 0xff000000);
    if (scaled_battery_icon == NULL)
        return source;

    // SDL_SoftStretch on the Miyoo SDL 1.2 build does not reliably preserve
    // per-pixel alpha and can turn the transparent charging-icon canvas into
    // a large white rectangle. Scale the already-rotated 32-bit pixels
    // directly so their RGBA values remain untouched.
    bool source_locked = SDL_MUSTLOCK(source);
    bool target_locked = SDL_MUSTLOCK(scaled_battery_icon);
    if (source_locked && SDL_LockSurface(source) != 0) {
        SDL_FreeSurface(scaled_battery_icon);
        scaled_battery_icon = NULL;
        return source;
    }
    if (target_locked && SDL_LockSurface(scaled_battery_icon) != 0) {
        if (source_locked)
            SDL_UnlockSurface(source);
        SDL_FreeSurface(scaled_battery_icon);
        scaled_battery_icon = NULL;
        return source;
    }
    for (int y = 0; y < height; y++) {
        int source_y = y * source->h / height;
        Uint32 *source_row = (Uint32 *)((Uint8 *)source->pixels +
                                        source_y * source->pitch);
        Uint32 *target_row =
            (Uint32 *)((Uint8 *)scaled_battery_icon->pixels +
                       y * scaled_battery_icon->pitch);
        for (int x = 0; x < width; x++)
            target_row[x] = source_row[x * source->w / width];
    }
    if (target_locked)
        SDL_UnlockSurface(scaled_battery_icon);
    if (source_locked)
        SDL_UnlockSurface(source);
    SDL_SetAlpha(scaled_battery_icon, SDL_SRCALPHA, 255);
    return scaled_battery_icon;
}

static TTF_Font *osd_font(int size)
{
    if (size < 10)
        size = 10;
    for (int i = 0; i < 4; i++)
        if (osd_fonts[i] != NULL && osd_font_sizes[i] == size)
            return osd_fonts[i];
    if (!TTF_WasInit()) {
        if (TTF_Init() != 0)
            return NULL;
        osd_ttf_owned = true;
    }
    const char *path = getenv("VC_OSD_FONT");
    if (path == NULL || path[0] == '\0')
        path = "/customer/app/Exo-2-Bold-Italic.ttf";
    for (int i = 0; i < 4; i++) {
        if (osd_fonts[i] != NULL)
            continue;
        osd_fonts[i] = TTF_OpenFont(path, size);
        if (osd_fonts[i] == NULL &&
            strcmp(path, "/customer/app/Exo-2-Bold-Italic.ttf") != 0)
            osd_fonts[i] = TTF_OpenFont(
                "/customer/app/Exo-2-Bold-Italic.ttf", size);
        osd_font_sizes[i] = size;
        return osd_fonts[i];
    }
    return osd_fonts[0];
}

static SDL_Surface *osd_text_color(OsdTextCache *cache, const char *text,
                                   int size, SDL_Color color)
{
    if (cache == NULL || text == NULL)
        return NULL;
    if (cache->surface != NULL && cache->size == size &&
        cache->red == color.r && cache->green == color.g &&
        cache->blue == color.b &&
        strcmp(cache->text, text) == 0)
        return cache->surface;

    SDL_FreeSurface(cache->surface);
    cache->surface = NULL;
    cache->text[0] = '\0';
    cache->size = size;
    cache->red = color.r;
    cache->green = color.g;
    cache->blue = color.b;
    TTF_Font *font = osd_font(size);
    if (font == NULL)
        return NULL;

    int outline = size >= 22 ? 2 : 1;
    SDL_Color black = {0, 0, 0, 255};
    TTF_SetFontOutline(font, outline);
    SDL_Surface *shadow = TTF_RenderUTF8_Blended(font, text, black);
    TTF_SetFontOutline(font, 0);
    SDL_Surface *face = TTF_RenderUTF8_Blended(font, text, color);
    if (shadow == NULL || face == NULL) {
        SDL_FreeSurface(shadow);
        SDL_FreeSurface(face);
        return NULL;
    }
    cache->surface = SDL_CreateRGBSurface(
        SDL_SWSURFACE, shadow->w, shadow->h, 32, 0x00ff0000, 0x0000ff00,
        0x000000ff, 0xff000000);
    if (cache->surface != NULL) {
        SDL_FillRect(cache->surface, NULL,
                     SDL_MapRGBA(cache->surface->format, 0, 0, 0, 0));
        bool lock_shadow = SDL_MUSTLOCK(shadow);
        bool lock_face = SDL_MUSTLOCK(face);
        bool lock_target = SDL_MUSTLOCK(cache->surface);
        if ((!lock_shadow || SDL_LockSurface(shadow) == 0) &&
            (!lock_face || SDL_LockSurface(face) == 0) &&
            (!lock_target || SDL_LockSurface(cache->surface) == 0)) {
            for (int y = 0; y < shadow->h; y++)
                for (int x = 0; x < shadow->w; x++) {
                    Uint32 pixel = *(Uint32 *)((Uint8 *)shadow->pixels +
                                               y * shadow->pitch + x * 4);
                    Uint8 r, g, b, a;
                    SDL_GetRGBA(pixel, shadow->format, &r, &g, &b, &a);
                    Uint32 *dst = (Uint32 *)((Uint8 *)cache->surface->pixels +
                                             y * cache->surface->pitch) + x;
                    *dst = SDL_MapRGBA(cache->surface->format, r, g, b, a);
                }
            for (int y = 0; y < face->h; y++)
                for (int x = 0; x < face->w; x++) {
                    Uint32 pixel = *(Uint32 *)((Uint8 *)face->pixels +
                                               y * face->pitch + x * 4);
                    Uint8 r, g, b, a;
                    SDL_GetRGBA(pixel, face->format, &r, &g, &b, &a);
                    if (a == 0)
                        continue;
                    int dx = x + outline;
                    int dy = y + outline;
                    if (dx >= cache->surface->w || dy >= cache->surface->h)
                        continue;
                    Uint32 *dst = (Uint32 *)((Uint8 *)cache->surface->pixels +
                                             dy * cache->surface->pitch) + dx;
                    *dst = SDL_MapRGBA(cache->surface->format, r, g, b, a);
                }
            if (lock_target)
                SDL_UnlockSurface(cache->surface);
            if (lock_face)
                SDL_UnlockSurface(face);
            if (lock_shadow)
                SDL_UnlockSurface(shadow);
        }
        rotate_surface_180(cache->surface);
        SDL_SetAlpha(cache->surface, SDL_SRCALPHA, 255);
        snprintf(cache->text, sizeof(cache->text), "%s", text);
    }
    SDL_FreeSurface(shadow);
    SDL_FreeSurface(face);
    return cache->surface;
}

static SDL_Surface *osd_text(OsdTextCache *cache, const char *text,
                             int size)
{
    SDL_Color white = {255, 255, 255, 255};
    return osd_text_color(cache, text, size, white);
}

static void blit_osd_text(SDL_Surface *target, SDL_Surface *text,
                          int logical_x, int logical_y)
{
    if (target == NULL || text == NULL)
        return;
    SDL_Rect position = {target->w - logical_x - text->w,
                         target->h - logical_y - text->h, 0, 0};
    SDL_BlitSurface(text, NULL, target, &position);
}

static void yuv_blit_osd_text(SDL_Overlay *overlay, SDL_Surface *text,
                              int logical_x, int logical_y)
{
    if (overlay == NULL || text == NULL || text->format->BytesPerPixel != 4)
        return;
    bool locked = SDL_MUSTLOCK(text);
    if (locked && SDL_LockSurface(text) != 0)
        return;
    int origin_x = overlay->w - logical_x - text->w;
    int origin_y = overlay->h - logical_y - text->h;
    for (int y = 0; y < text->h; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)text->pixels + y * text->pitch);
        for (int x = 0; x < text->w; x++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(row[x], text->format, &r, &g, &b, &a);
            if (a < 24)
                continue;
            int px = origin_x + x;
            int py = origin_y + y;
            if (px < 0 || py < 0 || px >= overlay->w || py >= overlay->h)
                continue;
            Uint8 *dst = overlay->pixels[0] + py * overlay->pitches[0] + px;
            int target_y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
            *dst = (Uint8)((target_y * a + *dst * (255 - a)) / 255);
            if (a >= 128) {
                Uint8 *u_plane = overlay->format == SDL_YV12_OVERLAY
                                     ? overlay->pixels[2]
                                     : overlay->pixels[1];
                Uint8 *v_plane = overlay->format == SDL_YV12_OVERLAY
                                     ? overlay->pixels[1]
                                     : overlay->pixels[2];
                int u_pitch = overlay->format == SDL_YV12_OVERLAY
                                  ? overlay->pitches[2]
                                  : overlay->pitches[1];
                int v_pitch = overlay->format == SDL_YV12_OVERLAY
                                  ? overlay->pitches[1]
                                  : overlay->pitches[2];
                Uint8 *u_dst = u_plane + (py / 2) * u_pitch + px / 2;
                Uint8 *v_dst = v_plane + (py / 2) * v_pitch + px / 2;
                int target_u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
                int target_v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
                *u_dst = (Uint8)((clamp_color(target_u) * a +
                                  *u_dst * (255 - a)) / 255);
                *v_dst = (Uint8)((clamp_color(target_v) * a +
                                  *v_dst * (255 - a)) / 255);
            }
        }
    }
    if (locked)
        SDL_UnlockSurface(text);
}

static void load_audio_artwork(SDL_Surface *surface)
{
    if (audio_artwork_loaded)
        return;
    audio_artwork_loaded = true;
    if (artwork_file == NULL || artwork_file[0] == '\0')
        return;
    SDL_Surface *raw = IMG_Load(artwork_file);
    if (raw == NULL || raw->w < 1 || raw->h < 1) {
        if (raw != NULL)
            SDL_FreeSurface(raw);
        return;
    }
    int tile = (int)(surface->h * 0.58);
    double scale_w = (double)tile / raw->w;
    double scale_h = (double)tile / raw->h;
    double scale = scale_w < scale_h ? scale_w : scale_h;
    int scaled_w = (int)(raw->w * scale + 0.5);
    int scaled_h = (int)(raw->h * scale + 0.5);
    if (scaled_w < 1)
        scaled_w = 1;
    if (scaled_h < 1)
        scaled_h = 1;

    // Keep the player's cover independent from libSDL_rotozoom. That
    // library has device-specific orientation side effects and was also
    // making repeated presentation calls much less stable on the Miyoo.
    SDL_Surface *source = SDL_CreateRGBSurface(
        SDL_SWSURFACE, raw->w, raw->h, 32, 0x00ff0000, 0x0000ff00,
        0x000000ff, 0xff000000);
    audio_artwork = SDL_CreateRGBSurface(
        SDL_SWSURFACE, scaled_w, scaled_h, 32, 0x00ff0000, 0x0000ff00,
        0x000000ff, 0xff000000);
    if (source != NULL && audio_artwork != NULL) {
        SDL_SetAlpha(raw, 0, 255);
        SDL_BlitSurface(raw, NULL, source, NULL);
        if (SDL_SoftStretch(source, NULL, audio_artwork, NULL) != 0) {
            SDL_FreeSurface(audio_artwork);
            audio_artwork = NULL;
        }
        else
            rotate_surface_180(audio_artwork);
    }
    else if (audio_artwork != NULL) {
        SDL_FreeSurface(audio_artwork);
        audio_artwork = NULL;
    }
    if (source != NULL)
        SDL_FreeSurface(source);
    SDL_FreeSurface(raw);
}

static void draw_audio_background(SDL_Surface *surface)
{
    if (!audio_mode || backlight_stage == 2)
        return;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    SDL_FillRect(surface, NULL, black);
    load_audio_artwork(surface);
    if (audio_artwork != NULL) {
        int logical_center_y = (int)(surface->h * 0.40);
        int physical_center_y = surface->h - logical_center_y;
        SDL_Rect position = {(surface->w - audio_artwork->w) / 2,
                             physical_center_y - audio_artwork->h / 2, 0, 0};
        SDL_BlitSurface(audio_artwork, NULL, surface, &position);
    }
    int title_scale = surface->w >= 600 ? 2 : 1;
    int max_chars = (surface->w - 32) / (6 * title_scale);
    char title[128];
    make_audio_title(title, sizeof(title), max_chars);
    if (title[0] != '\0') {
        int font_size = osd_size_for_width(surface->w, 100);
        SDL_Surface *label = osd_text(&audio_title_text_cache, title,
                                      font_size);
        while (label != NULL && label->w > surface->w - 32 &&
               font_size > 10) {
            font_size -= 2;
            if (font_size < 10)
                font_size = 10;
            label = osd_text(&audio_title_text_cache, title, font_size);
        }
        if (label != NULL)
            blit_osd_text(surface, label, (surface->w - label->w) / 2,
                          (int)(surface->h * 0.73));
    }
}

static void draw_audio_timer_only(void)
{
    if (!audio_mode || backlight_stage == 2)
        return;
    SDL_Surface *surface = hardware_surface != NULL
                               ? hardware_surface
                               : SDL_GetVideoSurface();
    if (surface == NULL)
        return;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    int scale = surface->w / 640;
    if (scale < 1)
        scale = 1;
    draw_logical_rect(surface, 12 * scale, 12 * scale,
                      150 * scale, 48 * scale, black);
    draw_timer_warning(surface);
}

static void draw_progress_knob(SDL_Surface *surface, int knob_x, int bar_y,
                               Uint32 color)
{
    int physical_x = surface->w - knob_x;
    int physical_y = surface->h - bar_y;
    const int radius = 5;
    const int outer_radius = 7;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    for (int dy = -outer_radius; dy <= outer_radius; dy++)
        for (int dx = -outer_radius; dx <= outer_radius; dx++)
            if (dx * dx + dy * dy <= outer_radius * outer_radius) {
                SDL_Rect pixel = {physical_x + dx, physical_y + dy, 1, 1};
                SDL_FillRect(surface, &pixel, black);
            }
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
            if (dx * dx + dy * dy <= radius * radius) {
                SDL_Rect pixel = {physical_x + dx, physical_y + dy, 1, 1};
                SDL_FillRect(surface, &pixel, color);
            }
}

static void draw_progress_bar(SDL_Surface *surface)
{
    if (duration_seconds <= 0)
        return;
    Uint32 now = SDL_GetTicks();
    if (!audio_mode && !paused && now >= progress_until)
        return;
    if (audio_mode && backlight_stage == 2)
        return;

    long elapsed = position_seconds;
    if (elapsed < 0)
        elapsed = 0;
    if (elapsed > duration_seconds)
        elapsed = duration_seconds;
    long remaining = duration_seconds - elapsed;
    char elapsed_text[24];
    char remaining_text[24];
    format_time(elapsed, false, elapsed_text, sizeof(elapsed_text));
    format_time(remaining, true, remaining_text, sizeof(remaining_text));

    int font_size = osd_size_for_width(surface->w, 100);
    SDL_Surface *elapsed_label = osd_text(&elapsed_text_cache, elapsed_text,
                                          font_size);
    SDL_Surface *remaining_label = osd_text(&remaining_text_cache,
                                            remaining_text, font_size);
    if (elapsed_label == NULL || remaining_label == NULL)
        return;
    int label_h = elapsed_label->h > remaining_label->h
                      ? elapsed_label->h
                      : remaining_label->h;
    int base_scale = surface->w / 320;
    if (base_scale < 1)
        base_scale = 1;
    int bar_y = surface->h - 14 * base_scale;
    int logical_y = bar_y - label_h / 2 - 3 * base_scale;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    Uint32 accent = SDL_MapRGB(surface->format, 174, 72, 255);

    int elapsed_width = elapsed_label->w;
    int remaining_width = remaining_label->w;
    int bar_x = 16 + elapsed_width + 16;
    int bar_right = surface->w - 16 - remaining_width - 16;
    int bar_w = bar_right - bar_x;
    if (bar_w < surface->w / 4) {
        bar_x = (int)(surface->w * 0.24);
        bar_w = surface->w - bar_x * 2;
    }

    blit_osd_text(surface, elapsed_label, 16, logical_y);
    blit_osd_text(surface, remaining_label,
                  surface->w - 16 - remaining_width, logical_y);
    draw_logical_rect(surface, bar_x, bar_y - 2, bar_w, 5, black);
    draw_logical_rect(surface, bar_x, bar_y - 1, bar_w, 3, accent);

    int knob_x = bar_x + (int)((long long)bar_w * elapsed / duration_seconds);
    draw_progress_knob(surface, knob_x, bar_y, accent);
    if (audio_mode) {
        last_progress_knob = knob_x;
        last_progress_bar_x = bar_x;
        last_progress_bar_w = bar_w;
        audio_progress_ready = true;
    }
}

static void update_changed_text(SDL_Surface *surface, const char *new_text,
                                int right_edge,
                                int logical_y, int font_size, Uint32 black,
                                OsdTextCache *cache)
{
    SDL_Surface *old_label = cache->surface;
    int old_width = old_label != NULL ? old_label->w : 0;
    int old_height = old_label != NULL ? old_label->h : 0;
    SDL_Surface *new_label = osd_text(cache, new_text, font_size);
    if (new_label == NULL)
        return;
    int new_width = new_label->w;
    int new_height = new_label->h;
    int old_x = right_edge >= 0 ? right_edge - old_width : 16;
    int new_x = right_edge >= 0 ? right_edge - new_width : 16;
    int left = old_x < new_x ? old_x : new_x;
    int right = old_x + old_width > new_x + new_width
                    ? old_x + old_width
                    : new_x + new_width;
    int height = old_height > new_height ? old_height : new_height;
    draw_logical_rect(surface, left - 2, logical_y - 2,
                      right - left + 4, height + 4, black);
    blit_osd_text(surface, new_label, new_x, logical_y);
}

static void draw_audio_progress_only(void)
{
    if (!audio_mode || backlight_stage == 2)
        return;
    SDL_Surface *surface = hardware_surface != NULL
                               ? hardware_surface
                               : SDL_GetVideoSurface();
    if (surface == NULL)
        return;

    if (duration_seconds <= 0)
        return;

    long elapsed = position_seconds;
    if (elapsed < 0)
        elapsed = 0;
    if (elapsed > duration_seconds)
        elapsed = duration_seconds;
    long remaining = duration_seconds - elapsed;
    char elapsed_text[24];
    char remaining_text[24];
    format_time(elapsed, false, elapsed_text, sizeof(elapsed_text));
    format_time(remaining, true, remaining_text, sizeof(remaining_text));

    int font_size = osd_size_for_width(surface->w, 100);
    int label_h = font_size + 4;
    if (elapsed_text_cache.surface != NULL &&
        elapsed_text_cache.surface->h > label_h)
        label_h = elapsed_text_cache.surface->h;
    if (remaining_text_cache.surface != NULL &&
        remaining_text_cache.surface->h > label_h)
        label_h = remaining_text_cache.surface->h;
    int base_scale = surface->w / 320;
    if (base_scale < 1)
        base_scale = 1;
    int bar_y = surface->h - 14 * base_scale;
    int logical_y = bar_y - label_h / 2 - 3 * base_scale;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    Uint32 accent = SDL_MapRGB(surface->format, 174, 72, 255);

    if (!audio_progress_ready) {
        draw_progress_bar(surface);
        draw_seek_notice();
        return;
    }

    update_changed_text(surface, elapsed_text, -1, logical_y, font_size,
                        black, &elapsed_text_cache);
    update_changed_text(surface, remaining_text, surface->w - 16, logical_y,
                        font_size, black, &remaining_text_cache);

    int elapsed_width = elapsed_text_cache.surface != NULL
                            ? elapsed_text_cache.surface->w
                            : 0;
    int remaining_width = remaining_text_cache.surface != NULL
                              ? remaining_text_cache.surface->w
                              : 0;
    int bar_x = 16 + elapsed_width + 16;
    int bar_right = surface->w - 16 - remaining_width - 16;
    int bar_w = bar_right - bar_x;
    if (bar_w < surface->w / 4) {
        bar_x = (int)(surface->w * 0.24);
        bar_w = surface->w - bar_x * 2;
    }
    int knob_x = bar_x + (int)((long long)bar_w * elapsed / duration_seconds);
    int clear_left = bar_x - 7;
    int clear_right = bar_x + bar_w + 7;
    if (last_progress_bar_x >= 0) {
        if (last_progress_bar_x - 7 < clear_left)
            clear_left = last_progress_bar_x - 7;
        if (last_progress_bar_x + last_progress_bar_w + 7 > clear_right)
            clear_right = last_progress_bar_x + last_progress_bar_w + 7;
    }
    if (last_progress_knob >= 0) {
        if (last_progress_knob - 7 < clear_left)
            clear_left = last_progress_knob - 7;
        if (last_progress_knob + 7 > clear_right)
            clear_right = last_progress_knob + 7;
    }
    draw_logical_rect(surface, clear_left, bar_y - 7,
                      clear_right - clear_left, 15, black);
    draw_logical_rect(surface, bar_x, bar_y - 2, bar_w, 5, black);
    draw_logical_rect(surface, bar_x, bar_y - 1, bar_w, 3, accent);
    draw_progress_knob(surface, knob_x, bar_y, accent);
    draw_seek_notice();
    last_progress_knob = knob_x;
    last_progress_bar_x = bar_x;
    last_progress_bar_w = bar_w;
    // Direct framebuffer writes are deliberately not followed by an SDL
    // refresh: the Miyoo driver can turn even a tiny update into a full-page
    // flip, which was the remaining source of random whole-screen flashes.
}

static void draw_seek_notice(void)
{
    SDL_Surface *surface = hardware_surface != NULL
                               ? hardware_surface
                               : SDL_GetVideoSurface();
    if (!surface)
        return;
    int base_scale = surface->w / 320;
    if (base_scale < 1)
        base_scale = 1;
    if (!seek_notice[0] || SDL_GetTicks() >= seek_notice_until) {
        clear_audio_seek_notice();
        seek_notice[0] = '\0';
        return;
    }
    if (audio_mode && seek_notice_drawn)
        return;
    SDL_Surface *notice = osd_text(
        &seek_text_cache, seek_notice,
        osd_size_for_width(surface->w, 100));
    if (notice == NULL)
        return;
    int width = notice->w;
    int x = seek_notice_forward ? 18 : surface->w - width - 18;
    int y = 35 * base_scale;
    // Coordinates are pre-rotated as well: logical top-left becomes the
    // physical bottom-right, and logical top-right becomes bottom-left.
    // Leave the lowest row free for the progress line and its time labels.
    blit_osd_text(surface, notice, surface->w - x - width,
                  surface->h - y - notice->h);
    seek_notice_drawn = true;
    if (audio_mode) {
        seek_notice_draw_x = x - 1;
        seek_notice_draw_y = y - 1;
        seek_notice_draw_w = width + 2;
        seek_notice_draw_h = notice->h + 2;
    }
}

static void set_seek_notice(bool forward, long step)
{
    Uint32 now = SDL_GetTicks();
    char next_notice[sizeof(seek_notice)];
    snprintf(next_notice, sizeof(next_notice), "%c%ld%s",
             forward ? '+' : '-', step >= 60 ? step / 60 : step,
             step >= 60 ? "m" : "s");
    bool same_audio_notice = audio_mode && seek_notice_drawn &&
                             seek_notice_forward == forward &&
                             strcmp(seek_notice, next_notice) == 0;
    if (audio_mode && seek_notice_drawn && !same_audio_notice)
        clear_audio_seek_notice();
    snprintf(seek_notice, sizeof(seek_notice), "%s", next_notice);
    seek_notice_forward = forward;
    if (!same_audio_notice)
        seek_notice_drawn = false;
    seek_notice_until = now + 2000;
    progress_until = now + 2000;
    // A seek can take long enough to consume the display timeout before a
    // decoded frame reaches the screen. Restart the two-second window on the
    // first post-seek video frame instead of counting during the seek itself.
    progress_waiting_for_video = !audio_mode;
    if (audio_mode) {
        overlay_force_redraw = false;
        draw_audio_progress_only();
    }
    else
        overlay_force_redraw = true;
}

static void paint_player_overlay(SDL_Surface *surface)
{
    draw_audio_background(surface);
    draw_progress_bar(surface);
    draw_seek_notice();
    draw_timer_warning(surface);
    draw_battery_peek(surface);
}

static bool player_overlay_visible(void)
{
    Uint32 now = SDL_GetTicks();
    if (backlight_stage == 2)
        return false;
    if (battery_peek_visible())
        return true;
    if (timer_warning_visible())
        return true;
    if (audio_mode)
        return true;
    if (progress_waiting_for_video)
        return true;
    if (duration_seconds > 0 && (paused || now < progress_until))
        return true;
    return seek_notice[0] != '\0' && now < seek_notice_until;
}

static void draw_player_overlay(void)
{
    SDL_Surface *surface = hardware_surface != NULL
                               ? hardware_surface
                               : SDL_GetVideoSurface();
    if (surface == NULL || !player_overlay_visible() ||
        (!audio_mode && !paused))
        return;
    bool was_inside_present = inside_present;
    inside_present = true;
    paint_player_overlay(surface);
    // Audio needs one real presentation when its complete static screen is
    // first composed. Video does not: SDL_DisplayYUVOverlay has just written
    // the decoded frame into the framebuffer, so the controls can be painted
    // directly into those pixels. Requesting an SDL refresh here caused a
    // second page flip for every video frame, visible as flicker and lag.
    if (audio_mode)
        update_screen(surface, 0, 0, surface->w, surface->h);
    inside_present = was_inside_present;
    overlay_force_redraw = false;
}

static bool progressive_seek(SDL_Event *event, Uint32 now)
{
    bool horizontal = seek_input == SDLK_LEFT || seek_input == SDLK_RIGHT;
    bool vertical = seek_input == SDLK_UP || seek_input == SDLK_DOWN;
    if (!horizontal && !vertical)
        return false;
    if (seek_last_step != 0 && now - seek_last_step < 450)
        return false;

    Uint32 held = now - seek_started_at;
    SDLKey out;
    long step;
    long applied_step;
    if (vertical && held < 1500) {
        // FFplay has a native one-minute key but no five-minute key. Emit
        // five native steps. Update our position only as each event is really
        // delivered so an immediately following key cannot leave the OSD
        // five minutes ahead of FFplay.
        out = seek_input == SDLK_DOWN ? SDLK_DOWN : SDLK_UP;
        pending_seek_key = out;
        pending_seek_events = 4;
        step = 300;
        applied_step = 60;
    }
    else if (vertical) {
        out = seek_input == SDLK_DOWN ? SDLK_LALT : SDLK_LSHIFT;
        step = 600;
        applied_step = step;
    }
    else if (held < 1500) {
        out = seek_input == SDLK_LEFT ? SDLK_LEFT : SDLK_RIGHT;
        step = 10;
        applied_step = step;
    }
    else {
        // After the short-seek phase, stay at one-minute steps for as long as
        // the combination is held. A third five-minute tier was too abrupt.
        out = seek_input == SDLK_LEFT ? SDLK_DOWN : SDLK_UP;
        step = 60;
        applied_step = step;
    }

    bool backwards = seek_input == SDLK_LEFT || seek_input == SDLK_DOWN;
    long delta = backwards ? -applied_step : applied_step;
    position_seconds += delta;
    if (position_seconds < 0)
        position_seconds = 0;
    if (duration_seconds > 0 && position_seconds > duration_seconds)
        position_seconds = duration_seconds;
    pending_seek_delta = pending_seek_events > 0 ? delta : 0;
    save_position();
    save_checkpoint();
    set_seek_notice(!backwards, step);
    seek_last_step = now;
    key(event, out, SDL_PRESSED);
    return true;
}

static bool emit_pending_seek(SDL_Event *event)
{
    if (pending_seek_events <= 0 || pending_seek_key == SDLK_UNKNOWN)
        return false;
    position_seconds += pending_seek_delta;
    if (position_seconds < 0)
        position_seconds = 0;
    if (duration_seconds > 0 && position_seconds > duration_seconds)
        position_seconds = duration_seconds;
    save_position();
    save_checkpoint();
    key(event, pending_seek_key, SDL_PRESSED);
    pending_seek_events--;
    if (pending_seek_events == 0) {
        pending_seek_key = SDLK_UNKNOWN;
        pending_seek_delta = 0;
    }
    return true;
}

static void cancel_seek_sequence(bool clear_notice)
{
    seek_input = SDLK_UNKNOWN;
    pending_seek_events = 0;
    pending_seek_key = SDLK_UNKNOWN;
    pending_seek_delta = 0;
    if (clear_notice) {
        if (audio_mode)
            clear_audio_seek_notice();
        seek_notice[0] = '\0';
        seek_notice_drawn = false;
    }
}

static bool map_event(SDL_Event *event)
{
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)
        return false;
    Uint8 state = event->key.state;
    SDLKey in = event->key.keysym.sym;
    Uint8 scancode = event->key.keysym.scancode;
    Uint32 now = SDL_GetTicks();
    load_player_config(now);

    // Swallow repeats and the release belonging to a wake press. Otherwise
    // holding B or MENU for a fraction too long could pause or leave playback
    // immediately after the backlight comes back on.
    if (wake_key != SDLK_UNKNOWN) {
        if (in == wake_key && state == SDL_RELEASED)
            wake_key = SDLK_UNKNOWN;
        return true;
    }

    // Onion has entered its own POWER screen-off state. Playback is allowed
    // to continue because /tmp/stay_awake is active, but player controls must
    // remain inert until Onion itself completes the POWER wake.
    if ((audio_mode || paused) && external_backlight_off)
        return true;

    if ((audio_mode || paused) && backlight_stage != 0) {
        // The player buttons and D-pad can wake an audio screen or a paused
        // video. Consume the whole gesture so waking cannot also seek,
        // resume, pause, capture or leave.
        if ((in == SDLK_SPACE || in == SDLK_LCTRL || in == SDLK_ESCAPE ||
             in == SDLK_LSHIFT || in == SDLK_LALT || in == SDLK_RCTRL ||
             in == SDLK_RETURN || in == SDLK_LEFT || in == SDLK_RIGHT ||
             in == SDLK_UP || in == SDLK_DOWN ||
             scancode == MIYOO_SCANCODE_VOLUMEDOWN ||
             scancode == MIYOO_SCANCODE_VOLUMEUP) &&
            state == SDL_PRESSED) {
            wake_key = in;
            restore_backlight();
            last_activity = now;
            if (audio_mode)
                draw_audio_progress_only();
            else
                draw_player_overlay();
        }
        return true;
    }

    // Once awake, discard physical volume keys from FFplay so they can never
    // become directional seeks; Onion's keymon performs the real adjustment.
    // While dimmed or off, the wake block above consumes the first gesture.
    if (scancode == MIYOO_SCANCODE_VOLUMEDOWN ||
        scancode == MIYOO_SCANCODE_VOLUMEUP) {
        // Stop held-key repetition, but let an already requested five-minute
        // jump finish. Those queued minute steps belong to the preceding
        // D-pad command, not to Volume.
        seek_input = SDLK_UNKNOWN;
        if (menu_down && state == SDL_PRESSED)
            menu_used = true;
        return true;
    }
    if (state == SDL_PRESSED)
        last_activity = now;

    if (in == SDLK_LSHIFT)
        x_down = state != SDL_RELEASED;
    if (in == SDLK_LALT)
        y_down = state != SDL_RELEASED;

    if (in == SDLK_ESCAPE) {
        if (state == SDL_PRESSED) {
            // Ignore MENU auto-repeat: only the first press starts the
            // gesture, otherwise its timer and combo flag would reset.
            if (!menu_down) {
                menu_down = true;
                menu_used = false;
                menu_pressed_at = SDL_GetTicks();
                menu_brightness_at_press =
                    read_number_file(brightness_file);
            }
            return true;
        }
        menu_down = false;
        // Releasing MENU ends repetition but must not truncate the current
        // five-minute batch (which previously produced only +/-2 minutes).
        seek_input = SDLK_UNKNOWN;
        // Onion's keymon may consume the volume-key event before SDL sees it.
        // Detect the resulting PWM change so MENU+VOL is still recognised as
        // a brightness gesture and MENU release cannot leave or seek media.
        long current_brightness = read_number_file(brightness_file);
        if (current_brightness > 0 && menu_brightness_at_press >= 0 &&
            current_brightness != menu_brightness_at_press) {
            menu_used = true;
            save_brightness_choice(current_brightness);
        }
        menu_brightness_at_press = -1;
        Uint32 held_ms = SDL_GetTicks() - menu_pressed_at;
        if (!menu_used && held_ms < 500)
        {
            update_clock();
            save_position();
            save_checkpoint();
            mark_menu_exit();
            key(event, SDLK_q, SDL_PRESSED);
        }
        else
            return true;
        return false;
    }

    if (menu_down && (in == SDLK_LEFT || in == SDLK_RIGHT ||
                      in == SDLK_UP || in == SDLK_DOWN)) {
        menu_used = true;
        if (state == SDL_RELEASED) {
            if (seek_input == in)
                seek_input = SDLK_UNKNOWN;
            return true;
        }
        Uint32 now = SDL_GetTicks();
        if (seek_input != in) {
            // A new direction replaces any unfinished multi-event jump from
            // the previous direction.
            cancel_seek_sequence(false);
            seek_input = in;
            seek_started_at = now;
            seek_last_step = 0;
        }
        if (progressive_seek(event, now))
            return false;
        return true;
    }

    if (in == SDLK_LALT && !menu_down) { /* Miyoo Y: battery */
        if (state == SDL_PRESSED && !battery_peek_down) {
            battery_peek_down = true;
            update_battery_status(now, true);
            if (audio_mode || paused) {
                overlay_force_redraw = true;
                draw_player_overlay();
            }
        }
        else if (state == SDL_RELEASED && battery_peek_down) {
            battery_peek_down = false;
            if (audio_mode) {
                overlay_force_redraw = true;
                draw_player_overlay();
            }
            else if (paused && last_overlay != NULL &&
                     last_overlay_rect_ready && real_overlay != NULL) {
                if (last_overlay_painted) {
                    restore_video_overlay(last_overlay);
                    last_overlay_painted = false;
                }
                real_overlay(last_overlay, &last_overlay_rect);
                draw_player_overlay();
            }
        }
        return true;
    }

    if (menu_down && (in == SDLK_LSHIFT || in == SDLK_LALT)) {
        menu_used = true;
        if (x_down && y_down && !screenshot_down) {
            screenshot_down = true;
            if (paused)
                save_paused_frame();
        }
        if (!x_down || !y_down)
            screenshot_down = false;
        return true;
    }

    if (menu_down && state == SDL_PRESSED) {
        // Any key used while MENU is held makes this a combination, even
        // when FFplay does not need the key (notably hardware volume keys).
        menu_used = true;
        seek_input = SDLK_UNKNOWN;
    }

    if (in == SDLK_SPACE) { /* Miyoo A: resume/show progress */
        if (state == SDL_PRESSED)
            dismiss_seek_notice();
        if (state == SDL_PRESSED && paused) {
            restore_backlight();
            last_activity = now;
            paused = false;
            set_media_playing_flag(true);
            progress_until = now + 2000;
            seek_notice_until = now + 2000;
            progress_waiting_for_video = !audio_mode;
            overlay_force_redraw = !audio_mode;
            if (audio_mode)
                draw_audio_progress_only();
            key(event, SDLK_SPACE, SDL_PRESSED);
            return false;
        }
        if (state == SDL_PRESSED && !paused) {
            // A during normal playback is deliberately not forwarded to
            // FFplay. It only reveals the progress information for two
            // seconds, without altering playback.
            progress_until = now + 2000;
            progress_waiting_for_video = !audio_mode;
            overlay_force_redraw = !audio_mode;
            if (audio_mode)
                draw_audio_progress_only();
            else
                draw_player_overlay();
        }
        return true;
    }
    if (in == SDLK_LCTRL) { /* Miyoo B: pause only */
        if (state == SDL_PRESSED && !paused) {
            paused = true;
            set_media_playing_flag(false);
            last_activity = now;
            overlay_force_redraw = !audio_mode;
            if (audio_mode)
                draw_audio_progress_only();
            else {
                // The playing OSD is baked into the last YUV frame. Replace
                // it with the pristine copy before painting the static pause
                // OSD, otherwise seek text appears twice at two layers.
                if (last_overlay_painted && last_overlay != NULL &&
                    last_overlay_rect_ready && real_overlay != NULL) {
                    restore_video_overlay(last_overlay);
                    last_overlay_painted = false;
                    real_overlay(last_overlay, &last_overlay_rect);
                }
                draw_player_overlay();
            }
            key(event, SDLK_SPACE, SDL_PRESSED);
            return false;
        }
        return true;
    }
    return true; /* child mode: discard every other key */
}

int SDL_PollEvent(SDL_Event *event)
{
    if (!real_poll)
        real_poll = (poll_fn)dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!real_poll && !real_wait)
        real_wait = (wait_fn)dlsym(RTLD_NEXT, "SDL_WaitEvent");
    ensure_key_repeat();
    update_clock();
    // Finish a logical five-minute command before consuming later physical
    // releases or volume events. This keeps all five native one-minute steps
    // together and prevents rapid gestures from truncating the command.
    if (emit_pending_seek(event))
        return 1;
    while (real_poll) {
        inside_event_call = true;
        int got = real_poll(event);
        inside_event_call = false;
        if (!got)
            break;
        if (!map_event(event))
            return 1;
    }
    if (emit_pending_seek(event) || progressive_seek(event, SDL_GetTicks()))
        return 1;
    return 0;
}

int SDL_WaitEvent(SDL_Event *event)
{
    // Do not block in the real SDL_WaitEvent while a seek key is held: the
    // Miyoo input driver does not reliably emit key-repeat events. Polling
    // here lets us generate the progressively larger seek steps ourselves.
    ensure_key_repeat();
    if (!real_poll)
        real_poll = (poll_fn)dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!real_poll && !real_wait)
        real_wait = (wait_fn)dlsym(RTLD_NEXT, "SDL_WaitEvent");
    while (real_poll) {
        update_clock();
        if (emit_pending_seek(event))
            return 1;
        inside_event_call = true;
        int got = real_poll(event);
        inside_event_call = false;
        if (got && !map_event(event))
            return 1;
        if (emit_pending_seek(event) || progressive_seek(event, SDL_GetTicks()))
            return 1;
        SDL_Delay(10);
    }
    return real_wait ? real_wait(event) : 0;
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action,
                   Uint32 mask)
{
    ensure_key_repeat();
    if (!real_peep)
        real_peep = (peep_fn)dlsym(RTLD_NEXT, "SDL_PeepEvents");
    if (!real_peep)
        return -1;
    update_clock();
    int count = real_peep(events, numevents, action, mask);
    if (inside_event_call)
        return count;
    if (action != SDL_GETEVENT || count < 0)
        return count;
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (!map_event(&events[i])) {
            if (kept != i)
                events[kept] = events[i];
            kept++;
        }
    }
    if (action == SDL_GETEVENT && kept < numevents) {
        while (kept < numevents && emit_pending_seek(&events[kept]))
            kept++;
        if (kept < numevents &&
            progressive_seek(&events[kept], SDL_GetTicks()))
            kept++;
    }
    return kept;
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    if (!real_set_mode)
        real_set_mode = (set_mode_fn)dlsym(RTLD_NEXT, "SDL_SetVideoMode");
    if (!real_set_mode)
        return NULL;

    load_player_config(SDL_GetTicks());
    hardware_surface = real_set_mode(width, height, bpp, flags);
    if (!audio_mode || hardware_surface == NULL)
        return hardware_surface;

    // Old SDL FFplay draws its audio waveform directly into the surface's
    // pixels before calling SDL_UpdateRect. Give those writes a software-only
    // target; our artwork/progress UI is painted separately on the real
    // hardware surface. This isolates audio without touching video playback.
    SDL_PixelFormat *format = hardware_surface->format;
    audio_visualizer_surface = SDL_CreateRGBSurface(
        SDL_SWSURFACE, width, height, format->BitsPerPixel,
        format->Rmask, format->Gmask, format->Bmask, format->Amask);
    if (audio_visualizer_surface != NULL) {
        SDL_FillRect(audio_visualizer_surface, NULL,
                     SDL_MapRGB(audio_visualizer_surface->format, 0, 0, 0));
        overlay_force_redraw = true;
        draw_player_overlay();
    }
    return audio_visualizer_surface != NULL ? audio_visualizer_surface
                                            : hardware_surface;
}

int SDL_DisplayYUVOverlay(SDL_Overlay *overlay, SDL_Rect *dstrect)
{
    load_player_config(SDL_GetTicks());
    if (audio_mode) {
        // Some audio files expose their embedded cover as a YUV video frame.
        // Presenting it through the Miyoo hardware overlay makes it flicker
        // over our audio UI and can leave an inverted layer after FFplay exits.
        // Suppress every hardware-video frame while in audio mode.
        update_clock();
        return 0;
    }
    if (!real_overlay)
        real_overlay = (overlay_fn)dlsym(RTLD_NEXT, "SDL_DisplayYUVOverlay");
    if (!real_overlay)
        return -1;
    mark_playback_started();
    last_overlay = overlay;
    if (dstrect != NULL) {
        last_overlay_rect = *dstrect;
        last_overlay_rect_ready = true;
    }
    update_clock();
    if (progress_waiting_for_video) {
        Uint32 now = SDL_GetTicks();
        progress_until = now + 2000;
        if (seek_notice[0] != '\0')
            seek_notice_until = now + 2000;
        progress_waiting_for_video = false;
    }
    // Compose the controls into the same YUV frame that FFplay presents.
    // This avoids painting the framebuffer after its page flip, which made
    // the OSD alternate between visible and missing frames on the Miyoo.
    bool painted = player_overlay_visible() &&
                   backup_and_paint_video_overlay(overlay);
    int result = real_overlay(overlay, dstrect);
    // Do not restore immediately: the Miyoo overlay is scanned out
    // asynchronously, so changing its pixels here produces striped text.
    // The pristine backup is retained for screenshots and paused redraws.
    last_overlay_painted = painted;
    return result;
}

void SDL_PauseAudio(int pause_on)
{
    if (!real_pause_audio)
        real_pause_audio = (pause_audio_fn)dlsym(RTLD_NEXT, "SDL_PauseAudio");
    load_player_config(SDL_GetTicks());
    // Audio-only playback has no YUV frame to establish its true start.
    // SDL unpauses the device exactly when samples may begin playing.
    if (!pause_on && audio_mode)
        mark_playback_started();
    if (real_pause_audio)
        real_pause_audio(pause_on);
}

void SDL_UpdateRect(SDL_Surface *surface, Sint32 x, Sint32 y, Uint32 width,
                    Uint32 height)
{
    if (!real_update)
        real_update = (update_fn)dlsym(RTLD_NEXT, "SDL_UpdateRect");
    if (!real_update)
        return;
    if (inside_present) {
        real_update(surface, x, y, width, height);
        return;
    }

    load_player_config(SDL_GetTicks());
    if (!audio_mode) {
        real_update(surface, x, y, width, height);
        return;
    }

    // FFplay's audio visualizer can request many full redraws per second.
    // Do not present those intermediate frames: they caused alternating
    // orientations, flicker and unnecessary load. Present our stable audio
    // screen only when its displayed second or state actually changes.
    update_clock();
}

void SDL_UpdateRects(SDL_Surface *surface, int numrects, SDL_Rect *rects)
{
    if (!real_updates)
        real_updates = (updates_fn)dlsym(RTLD_NEXT, "SDL_UpdateRects");
    if (!real_updates)
        return;
    if (inside_present) {
        real_updates(surface, numrects, rects);
        return;
    }

    load_player_config(SDL_GetTicks());
    if (!audio_mode) {
        real_updates(surface, numrects, rects);
        return;
    }

    // FFplay's audio waveform uses the plural SDL update entry point on
    // some Onion builds. Block it exactly like SDL_UpdateRect/SDL_Flip and
    // present one complete artwork/progress frame instead.
    update_clock();
}

int SDL_Flip(SDL_Surface *surface)
{
    if (!real_flip)
        real_flip = (flip_fn)dlsym(RTLD_NEXT, "SDL_Flip");
    if (!real_flip)
        return -1;
    if (inside_present)
        return real_flip(surface);

    load_player_config(SDL_GetTicks());
    if (!audio_mode)
        return real_flip(surface);

    update_clock();
    // The surface being flipped is the private software visualizer surface;
    // never present it. update_clock performs any tiny direct framebuffer
    // changes needed for the duration digits and progress knob.
    return 0;
}

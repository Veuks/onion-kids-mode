#define _GNU_SOURCE
#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PLATFORM_MIYOOMINI
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

typedef int (*poll_fn)(SDL_Event *);
typedef int (*wait_fn)(SDL_Event *);
typedef int (*peep_fn)(SDL_Event *, int, SDL_eventaction, Uint32);
typedef int (*overlay_fn)(SDL_Overlay *, SDL_Rect *);
typedef int (*flip_fn)(SDL_Surface *);
typedef SDL_Surface *(*set_mode_fn)(int, int, int, Uint32);
typedef void (*update_fn)(SDL_Surface *, Sint32, Sint32, Uint32, Uint32);
typedef void (*updates_fn)(SDL_Surface *, int, SDL_Rect *);
static poll_fn real_poll;
static wait_fn real_wait;
static peep_fn real_peep;
static overlay_fn real_overlay;
static flip_fn real_flip;
static set_mode_fn real_set_mode;
static update_fn real_update;
static updates_fn real_updates;
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
static SDLKey seek_input = SDLK_UNKNOWN;
static Uint32 seek_started_at;
static Uint32 seek_last_step;
static int pending_seek_events;
static SDLKey pending_seek_key = SDLK_UNKNOWN;
static bool clock_ready;
static Uint32 clock_tick;
static Uint32 last_save;
static long position_seconds;
static const char *position_file;
static const char *checkpoint_file;
static bool key_repeat_enabled;
static char seek_notice[8];
static bool seek_notice_forward;
static Uint32 seek_notice_until;
static bool player_config_ready;
static bool audio_mode;
static const char *artwork_file;
static const char *media_title;
static const char *duration_file;
static const char *brightness_file;
static long duration_seconds;
static Uint32 last_duration_check;
static Uint32 progress_until;
static bool progress_waiting_for_video;
static Uint32 last_activity;
static int backlight_stage;
static long saved_brightness_raw;
static SDL_Surface *audio_artwork;
static SDL_Surface *audio_visualizer_surface;
static SDL_Surface *hardware_surface;
static bool audio_artwork_loaded;
static bool inside_present;
static SDLKey wake_key = SDLK_UNKNOWN;
static bool overlay_force_redraw = true;
static bool audio_progress_ready;
static char last_elapsed_text[24];
static char last_remaining_text[24];
static int last_progress_knob = -1;
static bool seek_notice_drawn;
static int seek_notice_draw_x;
static int seek_notice_draw_y;
static int seek_notice_draw_w;
static int seek_notice_draw_h;
static Uint32 last_clock_update;
static Uint8 *yuv_backup[3];
static size_t yuv_backup_capacity[3];
#ifdef PLATFORM_MIYOOMINI
static int wake_input_fd = -1;
static bool wake_input_grabbed;
static unsigned short wake_input_code;
#endif

#define AUDIO_DIM_DELAY 10000
#define AUDIO_OFF_DELAY 15000
#define AUDIO_DIM_RAW 3
#define MIYOO_SCANCODE_VOLUMEDOWN 114
#define MIYOO_SCANCODE_VOLUMEUP 115

static void update_screen(SDL_Surface *surface, Sint32 x, Sint32 y,
                          Uint32 width, Uint32 height)
{
    if (!real_update)
        real_update = (update_fn)dlsym(RTLD_NEXT, "SDL_UpdateRect");
    if (real_update)
        real_update(surface, x, y, width, height);
}

static void update_clock(void);
static void draw_player_overlay(void);
static void draw_audio_progress_only(void);
static void draw_seek_notice(void);
static void restore_video_overlay(SDL_Overlay *overlay);
static int text_width(const char *text, int scale);
static void format_time(long seconds, bool remaining, char *out,
                        size_t out_size);
static bool player_overlay_visible(void);

#ifdef PLATFORM_MIYOOMINI
static bool is_wake_hardware_key(unsigned short code)
{
    return code == KEY_SPACE || code == KEY_LEFTCTRL ||
           code == KEY_LEFTSHIFT || code == KEY_LEFTALT ||
           code == KEY_RIGHTCTRL || code == KEY_ENTER ||
           code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP ||
           code == KEY_DOWN || code == KEY_ESC || code == KEY_POWER;
}

static bool grab_wake_input(void)
{
    if (wake_input_grabbed)
        return true;
    wake_input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (wake_input_fd < 0)
        return false;
    if (ioctl(wake_input_fd, EVIOCGRAB, 1) < 0) {
        close(wake_input_fd);
        wake_input_fd = -1;
        return false;
    }
    wake_input_grabbed = true;
    wake_input_code = 0;
    return true;
}

static void release_wake_input(void)
{
    if (wake_input_fd >= 0) {
        if (wake_input_grabbed)
            ioctl(wake_input_fd, EVIOCGRAB, 0);
        close(wake_input_fd);
    }
    wake_input_fd = -1;
    wake_input_grabbed = false;
    wake_input_code = 0;
}
#else
static bool grab_wake_input(void) { return false; }
static void release_wake_input(void) {}
#endif

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

static void restore_backlight(void)
{
    if (backlight_stage != 0 && saved_brightness_raw > 0)
        write_backlight(saved_brightness_raw);
    backlight_stage = 0;
}

__attribute__((destructor)) static void vcinput_unloaded(void)
{
    restore_backlight();
    release_wake_input();
    for (int i = 0; i < 3; i++) {
        free(yuv_backup[i]);
        yuv_backup[i] = NULL;
        yuv_backup_capacity[i] = 0;
    }
}

static void poll_wake_input(Uint32 now)
{
#ifdef PLATFORM_MIYOOMINI
    if (!wake_input_grabbed || wake_input_fd < 0)
        return;
    struct input_event ev;
    while (read(wake_input_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY || !is_wake_hardware_key(ev.code))
            continue;
        if (wake_input_code == 0 && ev.value == 1) {
            // Wake on the press for immediate feedback, but retain EVIOCGRAB
            // through its release so neither keymon nor FFplay can act on
            // any part of the gesture (especially POWER).
            wake_input_code = ev.code;
            restore_backlight();
            last_activity = now;
            if (audio_mode)
                draw_audio_progress_only();
            else
                draw_player_overlay();
        }
        else if (wake_input_code == ev.code && ev.value == 0) {
            release_wake_input();
            break;
        }
    }
#else
    (void)now;
#endif
}

static void load_player_config(Uint32 now)
{
    if (player_config_ready)
        return;
    const char *kind = getenv("VC_MEDIA_KIND");
    audio_mode = kind != NULL && strcmp(kind, "audio") == 0;
    artwork_file = getenv("VC_ARTWORK_FILE");
    media_title = getenv("VC_MEDIA_TITLE");
    duration_file = getenv("VC_DURATION_FILE");
    brightness_file = getenv("VC_BRIGHTNESS_FILE");
    const char *brightness = getenv("VC_BRIGHTNESS_RESTORE");
    saved_brightness_raw = brightness ? strtol(brightness, NULL, 10) : -1;
    if (saved_brightness_raw <= 0)
        saved_brightness_raw = read_number_file(brightness_file);
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

static void update_backlight(Uint32 now)
{
    // Audio playback may sleep while it keeps playing. Video playback may
    // sleep only while paused; an actively playing video always stays lit.
    if ((!audio_mode && !paused) || brightness_file == NULL ||
        saved_brightness_raw <= 0)
        return;
    Uint32 idle = now - last_activity;
    if (backlight_stage == 0 && idle >= AUDIO_DIM_DELAY) {
        long current = read_number_file(brightness_file);
        if (current > 0)
            saved_brightness_raw = current;
        if (write_backlight(AUDIO_DIM_RAW))
            backlight_stage = 1;
    }
    if (backlight_stage == 1 && idle >= AUDIO_OFF_DELAY &&
        grab_wake_input()) {
        if (write_backlight(0))
            backlight_stage = 2;
        else
            release_wake_input();
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
    if (clock_ready && now - last_clock_update < 50)
        return;
    last_clock_update = now;
    long previous_second = position_seconds;
    load_player_config(now);
    update_duration(now);
    poll_wake_input(now);
    update_backlight(now);
    if (!clock_ready) {
        const char *start = getenv("VC_START_SECONDS");
        position_seconds = start ? strtol(start, NULL, 10) : 0;
        position_file = getenv("VC_POSITION_FILE");
        checkpoint_file = getenv("VC_CHECKPOINT_FILE");
        clock_tick = last_save = now;
        clock_ready = true;
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

static void yuv_glyph(SDL_Overlay *overlay, char c, int x, int y, int scale,
                      bool outline)
{
    const unsigned char *rows = glyph_rows(c);
    if (rows == NULL)
        return;
    // Draw the complete outline first, then all foreground pixels. Painting
    // black+white one glyph pixel at a time lets the next pixel's outline
    // overwrite its neighbour, which produced the dotted/hatched playback
    // font visible in screenshots. The paused framebuffer path already uses
    // these same two passes and therefore remains solid.
    int first_pass = outline ? 0 : 1;
    for (int pass = first_pass; pass < 2; pass++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                if (!(rows[6 - row] & (1 << col)))
                    continue;
                if (pass == 0)
                    yuv_rect(overlay, x + col * scale - 1,
                             y + row * scale - 1, scale + 2, scale + 2,
                             16, 128, 128);
                else
                    yuv_rect(overlay, x + col * scale, y + row * scale,
                             scale, scale, 235, 128, 128);
            }
}

static void yuv_rotated_text(SDL_Overlay *overlay, const char *text,
                             int logical_x, int logical_y, int scale)
{
    int length = (int)strlen(text);
    int width = text_width(text, scale);
    int physical_x = overlay->w - logical_x - width;
    int physical_y = overlay->h - logical_y - 7 * scale;
    for (int i = 0; i < length; i++)
        yuv_glyph(overlay, text[length - 1 - i],
                  physical_x + i * 6 * scale, physical_y, scale, true);
}

static void yuv_logical_rect(SDL_Overlay *overlay, int x, int y, int w,
                             int h, Uint8 yy, Uint8 uu, Uint8 vv)
{
    yuv_rect(overlay, overlay->w - x - w, overlay->h - y - h, w, h,
             yy, uu, vv);
}

static void yuv_progress_knob(SDL_Overlay *overlay, int logical_x,
                              int logical_y, int radius)
{
    int cx = overlay->w - logical_x;
    int cy = overlay->h - logical_y;
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
            if (dx * dx + dy * dy <= radius * radius)
                yuv_rect(overlay, cx + dx, cy + dy, 1, 1,
                         122, 193, 160);
}

static void paint_video_yuv_overlay(SDL_Overlay *overlay)
{
    if (duration_seconds <= 0 || !player_overlay_visible())
        return;
    int scale = overlay->w / 320;
    if (scale < 1)
        scale = 1;
    long elapsed = position_seconds;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > duration_seconds) elapsed = duration_seconds;
    char elapsed_text[24], remaining_text[24];
    format_time(elapsed, false, elapsed_text, sizeof(elapsed_text));
    format_time(duration_seconds - elapsed, true, remaining_text,
                sizeof(remaining_text));
    int logical_y = overlay->h - 20 * scale;
    int bar_y = overlay->h - 14 * scale;
    int margin = 8 * scale;
    int elapsed_width = text_width(elapsed_text, scale);
    int remaining_width = text_width(remaining_text, scale);
    int bar_x = margin + elapsed_width + 8 * scale;
    int bar_right = overlay->w - margin - remaining_width - 8 * scale;
    int bar_w = bar_right - bar_x;
    if (bar_w < overlay->w / 4) {
        bar_x = overlay->w / 4;
        bar_w = overlay->w / 2;
    }
    yuv_rotated_text(overlay, elapsed_text, margin, logical_y, scale);
    yuv_rotated_text(overlay, remaining_text,
                     overlay->w - margin - remaining_width, logical_y,
                     scale);
    yuv_logical_rect(overlay, bar_x, bar_y - 1, bar_w, 3,
                     16, 128, 128);
    yuv_logical_rect(overlay, bar_x, bar_y, bar_w, 1,
                     122, 193, 160);
    int knob_x = bar_x +
        (int)((long long)bar_w * elapsed / duration_seconds);
    yuv_progress_knob(overlay, knob_x, bar_y, scale < 3 ? 3 : scale + 1);

    if (seek_notice[0] != '\0' && SDL_GetTicks() < seek_notice_until) {
        int notice_scale = scale + (scale > 1 ? scale / 2 : 1);
        int width = text_width(seek_notice, notice_scale);
        int logical_x = seek_notice_forward
                            ? overlay->w - width - 9 * scale
                            : 9 * scale;
        // Match the framebuffer path used while paused. Its physical offset
        // is 35 base-scale pixels from the panel edge; account for the glyph
        // height when expressing that same point in logical coordinates.
        int logical_y = overlay->h - 35 * scale - 7 * notice_scale;
        yuv_rotated_text(overlay, seek_notice, logical_x,
                         logical_y, notice_scale);
    }
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
        int width = text_width(title, title_scale);
        Uint32 white = SDL_MapRGB(surface->format, 255, 255, 255);
        draw_rotated_text(surface, title, (surface->w - width) / 2,
                          (int)(surface->h * 0.73), title_scale, black, white);
    }
}

static void draw_progress_knob(SDL_Surface *surface, int knob_x, int bar_y,
                               Uint32 color)
{
    int physical_x = surface->w - knob_x;
    int physical_y = surface->h - bar_y;
    const int radius = 5;
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

    int scale = surface->w >= 600 ? 2 : 1;
    int logical_y = surface->h - 39;
    int bar_y = surface->h - 28;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    Uint32 white = SDL_MapRGB(surface->format, 255, 255, 255);
    Uint32 accent = SDL_MapRGB(surface->format, 174, 72, 255);

    int elapsed_width = text_width(elapsed_text, scale);
    int remaining_width = text_width(remaining_text, scale);
    int bar_x = 16 + elapsed_width + 16;
    int bar_right = surface->w - 16 - remaining_width - 16;
    int bar_w = bar_right - bar_x;
    if (bar_w < surface->w / 4) {
        bar_x = (int)(surface->w * 0.24);
        bar_w = surface->w - bar_x * 2;
    }

    draw_rotated_text(surface, elapsed_text, 16, logical_y, scale, black,
                      white);
    draw_rotated_text(surface, remaining_text,
                      surface->w - 16 - remaining_width, logical_y, scale,
                      black, white);
    draw_logical_rect(surface, bar_x, bar_y - 1, bar_w, 3, black);
    draw_logical_rect(surface, bar_x, bar_y, bar_w, 1, accent);

    int knob_x = bar_x + (int)((long long)bar_w * elapsed / duration_seconds);
    draw_progress_knob(surface, knob_x, bar_y, accent);
    if (audio_mode) {
        snprintf(last_elapsed_text, sizeof(last_elapsed_text), "%s",
                 elapsed_text);
        snprintf(last_remaining_text, sizeof(last_remaining_text), "%s",
                 remaining_text);
        last_progress_knob = knob_x;
        audio_progress_ready = true;
    }
}

static void update_changed_text(SDL_Surface *surface, const char *old_text,
                                const char *new_text, int right_edge,
                                int logical_y, int scale, Uint32 black,
                                Uint32 white)
{
    int old_len = (int)strlen(old_text);
    int new_len = (int)strlen(new_text);
    int old_width = text_width(old_text, scale);
    int new_width = text_width(new_text, scale);
    int old_x = right_edge >= 0 ? right_edge - old_width : 16;
    int new_x = right_edge >= 0 ? right_edge - new_width : 16;

    if (old_len != new_len) {
        int left = old_x < new_x ? old_x : new_x;
        int right = old_x + old_width > new_x + new_width
                        ? old_x + old_width
                        : new_x + new_width;
        draw_logical_rect(surface, left - 1, logical_y - 1,
                          right - left + 2, 7 * scale + 2, black);
        draw_rotated_text(surface, new_text, new_x, logical_y, scale, black,
                          white);
        return;
    }

    for (int i = 0; i < new_len; i++) {
        if (old_text[i] == new_text[i])
            continue;
        int x = new_x + i * 6 * scale;
        draw_logical_rect(surface, x - 1, logical_y - 1,
                          6 * scale + 2, 7 * scale + 2, black);
        char digit[2] = {new_text[i], '\0'};
        draw_rotated_text(surface, digit, x, logical_y, scale, black, white);
    }
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

    int scale = surface->w >= 600 ? 2 : 1;
    int logical_y = surface->h - 39;
    int bar_y = surface->h - 28;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    Uint32 white = SDL_MapRGB(surface->format, 255, 255, 255);
    Uint32 accent = SDL_MapRGB(surface->format, 174, 72, 255);

    if (!audio_progress_ready) {
        draw_progress_bar(surface);
        draw_seek_notice();
        return;
    }

    update_changed_text(surface, last_elapsed_text, elapsed_text, -1,
                        logical_y, scale, black, white);
    update_changed_text(surface, last_remaining_text, remaining_text,
                        surface->w - 16, logical_y, scale, black, white);

    int elapsed_width = text_width(elapsed_text, scale);
    int remaining_width = text_width(remaining_text, scale);
    int bar_x = 16 + elapsed_width + 16;
    int bar_right = surface->w - 16 - remaining_width - 16;
    int bar_w = bar_right - bar_x;
    if (bar_w < surface->w / 4) {
        bar_x = (int)(surface->w * 0.24);
        bar_w = surface->w - bar_x * 2;
    }
    int knob_x = bar_x + (int)((long long)bar_w * elapsed / duration_seconds);
    bool layout_changed = strlen(last_elapsed_text) != strlen(elapsed_text) ||
                          strlen(last_remaining_text) != strlen(remaining_text);
    if (layout_changed) {
        int old_elapsed_width = text_width(last_elapsed_text, scale);
        int old_remaining_width = text_width(last_remaining_text, scale);
        int old_bar_x = 16 + old_elapsed_width + 16;
        int old_bar_right = surface->w - 16 - old_remaining_width - 16;
        int old_bar_w = old_bar_right - old_bar_x;
        if (old_bar_w < surface->w / 4) {
            old_bar_x = (int)(surface->w * 0.24);
            old_bar_w = surface->w - old_bar_x * 2;
        }
        int clear_left = old_bar_x < bar_x ? old_bar_x : bar_x;
        int clear_right = old_bar_x + old_bar_w > bar_x + bar_w
                              ? old_bar_x + old_bar_w
                              : bar_x + bar_w;
        draw_logical_rect(surface, clear_left - 7, bar_y - 7,
                          clear_right - clear_left + 14, 15, black);
        draw_logical_rect(surface, bar_x, bar_y - 1, bar_w, 3, black);
        draw_logical_rect(surface, bar_x, bar_y, bar_w, 1, accent);
        draw_progress_knob(surface, knob_x, bar_y, accent);
    }
    else if (last_progress_knob >= 0 && knob_x != last_progress_knob) {
        const int radius = 6;
        draw_logical_rect(surface, last_progress_knob - radius,
                          bar_y - radius, radius * 2 + 1,
                          radius * 2 + 1, black);
        draw_logical_rect(surface, last_progress_knob - radius,
                          bar_y - 1, radius * 2 + 1, 3, black);
        draw_logical_rect(surface, last_progress_knob - radius,
                          bar_y, radius * 2 + 1, 1, accent);
        draw_progress_knob(surface, knob_x, bar_y, accent);
    }
    draw_seek_notice();
    snprintf(last_elapsed_text, sizeof(last_elapsed_text), "%s", elapsed_text);
    snprintf(last_remaining_text, sizeof(last_remaining_text), "%s",
             remaining_text);
    last_progress_knob = knob_x;
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
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    int base_scale = surface->w / 320;
    if (base_scale < 1)
        base_scale = 1;
    int scale = base_scale + (base_scale > 1 ? base_scale / 2 : 1);
    int length = (int)strlen(seek_notice);
    int width = length > 0 ? length * 6 * scale - scale : 0;
    int x = seek_notice_forward ? 18 : surface->w - width - 18;
    int y = 35 * base_scale;
    if (!seek_notice[0] || SDL_GetTicks() >= seek_notice_until) {
        clear_audio_seek_notice();
        seek_notice[0] = '\0';
        return;
    }
    if (audio_mode && seek_notice_drawn)
        return;
    // Coordinates are pre-rotated as well: logical top-left becomes the
    // physical bottom-right, and logical top-right becomes bottom-left.
    // Leave the lowest row free for the progress line and its time labels.
    Uint32 white = SDL_MapRGB(surface->format, 255, 255, 255);
    for (int i = 0; i < length; i++)
        draw_glyph(surface, seek_notice[length - 1 - i],
                   x + i * 6 * scale, y, scale,
                   black, white);
    seek_notice_drawn = true;
    if (audio_mode) {
        seek_notice_draw_x = x - 1;
        seek_notice_draw_y = y - 1;
        seek_notice_draw_w = width + 2;
        seek_notice_draw_h = 7 * scale + 2;
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
}

static bool player_overlay_visible(void)
{
    Uint32 now = SDL_GetTicks();
    if (backlight_stage == 2)
        return false;
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
    if (vertical && held < 1500) {
        // FFplay has a native one-minute key but no five-minute key. Emit
        // five native steps; position_seconds is adjusted once for the whole
        // logical jump so the displayed time remains exact.
        out = seek_input == SDLK_DOWN ? SDLK_DOWN : SDLK_UP;
        pending_seek_key = out;
        pending_seek_events = 4;
        step = 300;
    }
    else if (vertical) {
        out = seek_input == SDLK_DOWN ? SDLK_LALT : SDLK_LSHIFT;
        step = 600;
    }
    else if (held < 1500) {
        out = seek_input == SDLK_LEFT ? SDLK_LEFT : SDLK_RIGHT;
        step = 10;
    }
    else {
        // After the short-seek phase, stay at one-minute steps for as long as
        // the combination is held. A third five-minute tier was too abrupt.
        out = seek_input == SDLK_LEFT ? SDLK_DOWN : SDLK_UP;
        step = 60;
    }

    bool backwards = seek_input == SDLK_LEFT || seek_input == SDLK_DOWN;
    position_seconds += backwards ? -step : step;
    if (position_seconds < 0)
        position_seconds = 0;
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
    key(event, pending_seek_key, SDL_PRESSED);
    pending_seek_events--;
    if (pending_seek_events == 0)
        pending_seek_key = SDLK_UNKNOWN;
    return true;
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

    // The Miyoo SDL driver can expose the two physical volume buttons with a
    // directional SDL symbol. Identify them by their Linux hardware scan code
    // before handling D-pad seeks, so volume/brightness gestures can never
    // jump to another point in the media. keymon still handles the system OSD.
    if (scancode == MIYOO_SCANCODE_VOLUMEDOWN ||
        scancode == MIYOO_SCANCODE_VOLUMEUP) {
        if (menu_down && state == SDL_PRESSED)
            menu_used = true;
        return true;
    }

    // Swallow repeats and the release belonging to a wake press. Otherwise
    // holding B or MENU for a fraction too long could pause or leave playback
    // immediately after the backlight comes back on.
    if (wake_key != SDLK_UNKNOWN) {
        if (in == wake_key && state == SDL_RELEASED)
            wake_key = SDLK_UNKNOWN;
        return true;
    }

    if ((audio_mode || paused) && backlight_stage != 0) {
        // The player buttons and D-pad can wake an audio screen or a paused
        // video. Consume the whole gesture so waking cannot also seek,
        // resume, pause, capture or leave.
        if ((in == SDLK_SPACE || in == SDLK_LCTRL || in == SDLK_ESCAPE ||
             in == SDLK_LSHIFT || in == SDLK_LALT || in == SDLK_RCTRL ||
             in == SDLK_RETURN || in == SDLK_LEFT || in == SDLK_RIGHT ||
             in == SDLK_UP || in == SDLK_DOWN) &&
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
            }
            return true;
        }
        menu_down = false;
        seek_input = SDLK_UNKNOWN;
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
            seek_input = in;
            seek_started_at = now;
            seek_last_step = 0;
        }
        if (progressive_seek(event, now))
            return false;
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
    }

    if (in == SDLK_SPACE) { /* Miyoo A: resume/show progress */
        if (state == SDL_PRESSED && paused) {
            restore_backlight();
            last_activity = now;
            paused = false;
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
    if (action != SDL_GETEVENT || count <= 0)
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
        if (emit_pending_seek(&events[kept]) ||
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

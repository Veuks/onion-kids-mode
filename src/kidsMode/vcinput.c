#define _GNU_SOURCE
#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*poll_fn)(SDL_Event *);
typedef int (*wait_fn)(SDL_Event *);
typedef int (*peep_fn)(SDL_Event *, int, SDL_eventaction, Uint32);
typedef int (*overlay_fn)(SDL_Overlay *, SDL_Rect *);
typedef int (*flip_fn)(SDL_Surface *);
typedef void (*update_fn)(SDL_Surface *, Sint32, Sint32, Uint32, Uint32);
static poll_fn real_poll;
static wait_fn real_wait;
static peep_fn real_peep;
static overlay_fn real_overlay;
static flip_fn real_flip;
static update_fn real_update;
static SDL_Overlay *last_overlay;
static bool inside_event_call;
static bool paused;
static bool menu_down;
static bool menu_used;
static bool screenshot_down;
static Uint32 menu_pressed_at;
static SDLKey seek_input = SDLK_UNKNOWN;
static Uint32 seek_started_at;
static Uint32 seek_last_step;
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
static const char *duration_file;
static const char *brightness_file;
static long duration_seconds;
static Uint32 last_duration_check;
static Uint32 progress_until;
static Uint32 last_activity;
static int backlight_stage;
static long saved_brightness_raw;
static SDL_Surface *audio_artwork;
static bool audio_artwork_loaded;
static bool inside_present;
static SDLKey wake_key = SDLK_UNKNOWN;
static long last_presented_second = -1;
static bool overlay_force_redraw = true;

#define AUDIO_DIM_DELAY 10000
#define AUDIO_OFF_DELAY 15000
#define AUDIO_DIM_RAW 3

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
    if (SDL_LockYUVOverlay(last_overlay) != 0)
        return false;

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
        SDL_UnlockYUVOverlay(last_overlay);
        return false;
    }

    Uint8 *y_plane = last_overlay->pixels[0];
    Uint8 *u_plane = last_overlay->format == SDL_YV12_OVERLAY
                         ? last_overlay->pixels[2]
                         : last_overlay->pixels[1];
    Uint8 *v_plane = last_overlay->format == SDL_YV12_OVERLAY
                         ? last_overlay->pixels[1]
                         : last_overlay->pixels[2];
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
}

static void load_player_config(Uint32 now)
{
    if (player_config_ready)
        return;
    const char *kind = getenv("VC_MEDIA_KIND");
    audio_mode = kind != NULL && strcmp(kind, "audio") == 0;
    artwork_file = getenv("VC_ARTWORK_FILE");
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
        write_backlight(0))
        backlight_stage = 2;
}

static void write_position(const char *path)
{
    if (!path || !*path)
        return;
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%ld\n", position_seconds < 0 ? 0 : position_seconds);
        fclose(fp);
    }
}

static void save_position(void)
{
    write_position(position_file);
}

static void save_checkpoint(void)
{
    write_position(checkpoint_file);
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
    long previous_second = position_seconds;
    load_player_config(now);
    update_duration(now);
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
        position_seconds != previous_second)
        draw_player_overlay();
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
    case 's': return ess;
    case 'm': return em;
    default: return NULL;
    }
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
    int physical_x = surface->w - knob_x;
    int physical_y = surface->h - bar_y;
    const int radius = 5;
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
            if (dx * dx + dy * dy <= radius * radius) {
                SDL_Rect pixel = {physical_x + dx, physical_y + dy, 1, 1};
                SDL_FillRect(surface, &pixel, accent);
            }
}

static void draw_seek_notice(void)
{
    SDL_Surface *surface = SDL_GetVideoSurface();
    if (!surface)
        return;
    Uint32 black = SDL_MapRGB(surface->format, 0, 0, 0);
    if (!seek_notice[0] || SDL_GetTicks() >= seek_notice_until) {
        seek_notice[0] = '\0';
        return;
    }

    int scale = surface->w >= 600 ? 3 : 2;
    int length = (int)strlen(seek_notice);
    int width = length * 6 * scale - scale;
    // Coordinates are pre-rotated as well: logical top-left becomes the
    // physical bottom-right, and logical top-right becomes bottom-left.
    int x = seek_notice_forward ? 18 : surface->w - width - 18;
    // Leave the lowest row free for the progress line and its time labels.
    int y = 70;
    Uint32 white = SDL_MapRGB(surface->format, 255, 255, 255);
    for (int i = 0; i < length; i++)
        draw_glyph(surface, seek_notice[length - 1 - i],
                   x + i * 6 * scale, y, scale,
                   black, white);
}

static void set_seek_notice(bool forward, long step)
{
    snprintf(seek_notice, sizeof(seek_notice), "%c%ld%s",
             forward ? '+' : '-', step >= 60 ? step / 60 : step,
             step >= 60 ? "m" : "s");
    seek_notice_forward = forward;
    seek_notice_until = SDL_GetTicks() + 2000;
    progress_until = SDL_GetTicks() + 2000;
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
    if (duration_seconds > 0 && (paused || now < progress_until))
        return true;
    return seek_notice[0] != '\0' && now < seek_notice_until;
}

static bool player_overlay_needs_redraw(void)
{
    if (overlay_force_redraw)
        return true;
    // Audio time must advance once per second. For video, keep the exact same
    // composed controls frame on screen for its whole two-second lifetime.
    // Repainting it every second over the hardware YUV plane caused flicker.
    return audio_mode && position_seconds != last_presented_second;
}

static void draw_player_overlay(void)
{
    SDL_Surface *surface = SDL_GetVideoSurface();
    if (surface == NULL || !player_overlay_visible())
        return;
    paint_player_overlay(surface);
    update_screen(surface, 0, 0, surface->w, surface->h);
    last_presented_second = position_seconds;
    overlay_force_redraw = false;
}

static bool progressive_seek(SDL_Event *event, Uint32 now)
{
    if (seek_input != SDLK_LEFT && seek_input != SDLK_RIGHT)
        return false;
    if (seek_last_step != 0 && now - seek_last_step < 450)
        return false;

    Uint32 held = now - seek_started_at;
    SDLKey out;
    long step;
    if (held < 1500) {
        out = seek_input == SDLK_LEFT ? SDLK_LEFT : SDLK_RIGHT;
        step = 10;
    }
    else {
        // After the short-seek phase, stay at one-minute steps for as long as
        // the combination is held. A third five-minute tier was too abrupt.
        out = seek_input == SDLK_LEFT ? SDLK_DOWN : SDLK_UP;
        step = 60;
    }

    position_seconds += seek_input == SDLK_LEFT ? -step : step;
    if (position_seconds < 0)
        position_seconds = 0;
    save_position();
    save_checkpoint();
    set_seek_notice(seek_input == SDLK_RIGHT, step);
    seek_last_step = now;
    key(event, out, SDL_PRESSED);
    return true;
}

static bool map_event(SDL_Event *event)
{
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)
        return false;
    Uint8 state = event->key.state;
    SDLKey in = event->key.keysym.sym;
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

    if ((audio_mode || paused) && backlight_stage != 0) {
        // A, B and MENU can wake an audio screen or a paused video. Consume
        // the whole gesture so waking cannot also resume, pause or leave.
        if ((in == SDLK_SPACE || in == SDLK_LCTRL || in == SDLK_ESCAPE) &&
            state == SDL_PRESSED) {
            wake_key = in;
            restore_backlight();
            last_activity = now;
            draw_player_overlay();
        }
        return true;
    }
    if (state == SDL_PRESSED)
        last_activity = now;

    if (in == SDLK_LSHIFT && state == SDL_RELEASED)
        screenshot_down = false;

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

    if (menu_down && (in == SDLK_LEFT || in == SDLK_RIGHT)) {
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

    if (menu_down && in == SDLK_LSHIFT) { /* Miyoo X: capture paused frame */
        menu_used = true;
        if (state == SDL_PRESSED && !screenshot_down) {
            screenshot_down = true;
            if (paused)
                save_paused_frame();
        }
        return true;
    }

    if (menu_down && state == SDL_PRESSED) {
        SDLKey out = SDLK_UNKNOWN;
        // Any key used while MENU is held makes this a combination, even
        // when FFplay does not need the key (notably hardware volume keys).
        menu_used = true;
        if (in == SDLK_UP) out = SDLK_LSHIFT;       /* native X: +600 s */
        if (in == SDLK_DOWN) out = SDLK_LALT;        /* native Y: -600 s */
        if (out != SDLK_UNKNOWN) {
            if (in == SDLK_UP) position_seconds += 600;
            if (in == SDLK_DOWN) position_seconds -= 600;
            if (position_seconds < 0) position_seconds = 0;
            save_position();
            save_checkpoint();
            set_seek_notice(in == SDLK_UP, 600);
            key(event, out, SDL_PRESSED);
            return false;
        }
    }

    if (in == SDLK_SPACE) { /* Miyoo A: resume only */
        if (state == SDL_PRESSED && paused) {
            restore_backlight();
            last_activity = now;
            paused = false;
            progress_until = now + 2000;
            overlay_force_redraw = true;
            key(event, SDLK_SPACE, SDL_PRESSED);
            return false;
        }
        return true;
    }
    if (in == SDLK_LCTRL) { /* Miyoo B: pause only */
        if (state == SDL_PRESSED && !paused) {
            paused = true;
            last_activity = now;
            overlay_force_redraw = true;
            draw_player_overlay();
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
    if (progressive_seek(event, SDL_GetTicks()))
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
        if (progressive_seek(event, SDL_GetTicks()))
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
    if (action == SDL_GETEVENT && kept < numevents &&
        progressive_seek(&events[kept], SDL_GetTicks()))
        kept++;
    return kept;
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
        if (player_overlay_needs_redraw())
            draw_player_overlay();
        return 0;
    }
    if (!real_overlay)
        real_overlay = (overlay_fn)dlsym(RTLD_NEXT, "SDL_DisplayYUVOverlay");
    if (!real_overlay)
        return -1;
    last_overlay = overlay;
    int result = real_overlay(overlay, dstrect);
    update_clock();
    // Keep video controls stable instead of repainting them at the frame
    // rate or at every clock tick. Their fully composed frame changes only
    // after an explicit control action; repeated updates caused flicker.
    if (player_overlay_needs_redraw())
        draw_player_overlay();
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
    if (!audio_mode && !player_overlay_visible()) {
        real_update(surface, x, y, width, height);
        return;
    }

    if (!audio_mode) {
        // FFplay may keep refreshing its RGB window while the YUV video is
        // paused. Do not let those redundant refreshes alternate with the
        // progress overlay.
        update_clock();
        if (player_overlay_needs_redraw())
            draw_player_overlay();
        return;
    }

    // FFplay's audio visualizer can request many full redraws per second.
    // Do not present those intermediate frames: they caused alternating
    // orientations, flicker and unnecessary load. Present our stable audio
    // screen only when its displayed second or state actually changes.
    inside_present = true;
    update_clock();
    if (player_overlay_needs_redraw()) {
        paint_player_overlay(surface);
        real_update(surface, 0, 0, surface->w, surface->h);
        last_presented_second = position_seconds;
        overlay_force_redraw = false;
    }
    inside_present = false;
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
    if (!audio_mode && !player_overlay_visible())
        return real_flip(surface);

    if (!audio_mode) {
        update_clock();
        if (player_overlay_needs_redraw())
            draw_player_overlay();
        return 0;
    }

    inside_present = true;
    update_clock();
    if (!player_overlay_needs_redraw()) {
        inside_present = false;
        return 0;
    }
    // Paint before flipping so the stable cover/progress frame, rather than
    // FFplay's visualizer, is the buffer that becomes visible.
    paint_player_overlay(surface);
    int result = real_flip(surface);
    last_presented_second = position_seconds;
    overlay_force_redraw = false;
    inside_present = false;
    return result;
}
